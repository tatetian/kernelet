// SPDX-License-Identifier: MPL-2.0

//! The image ABI: the only thing that couples the kernelet image to a host.
//!
//! Six `#[repr(C)]` structures, mirrored by `endovisor/kernelet.h`:
//!
//! - [`BootArgs`], what the endovisor tells the kernelet about the machine it
//!   was given;
//! - [`Services`], the host functions the kernelet may call;
//! - [`EntryTable`], the kernelet functions the host may call, exported as the
//!   single symbol `kernelet_entry_table`;
//! - [`VcpuRec`], the one page per virtual CPU that both sides write;
//! - [`SpaceRec`], one per address space: its root and its range-lock words;
//! - [`MemStats`], counters of the host's memory paths, for the tests.
//!
//! vOSTD calls no Linux symbol. Everything it needs from the host arrives
//! through [`Services`], except the four host offsets in [`BootArgs`] (where the
//! current-task pointer and the preemption count sit in the per-CPU area, where
//! a task's hooks pointer sits in its `task_struct`, and where the vCPU record
//! sits from the hooks) that let it find its own [`VcpuRec`] and mirror its guard
//! depth into the host's preemption count without a call.

use core::{
    cell::UnsafeCell,
    sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering},
};

use crate::arch::trap::RawUserContext;

/// The ABI version. Bumped whenever any of the tables changes shape.
pub const KLET_ABI_VERSION: u64 = 7;

/// What the endovisor tells the kernelet about the machine it was given.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct BootArgs {
    /// Must equal [`KLET_ABI_VERSION`].
    pub abi_version: u64,
    /// The first physical address of the grant.
    pub grant_paddr: u64,
    /// The length of the grant in bytes.
    pub grant_len: u64,
    /// The base of the host's linear (direct) map: `vaddr = base + paddr`.
    pub linear_map_base: u64,
    /// The number of virtual CPUs the sandbox has.
    pub nr_vcpus: u64,
    /// The `gs:`-relative offset of the host's current-task pointer.
    pub gs_current_task: u64,
    /// The `gs:`-relative offset of the host's preemption count.
    pub gs_preempt_count: u64,
    /// The byte offset of the hooks pointer inside the host's task structure.
    pub task_hooks_off: u64,
    /// The byte offset of the vCPU record from the hooks pointer.
    pub hooks_vrec_off: u64,
    /// The size in bytes of a floating-point save area.
    pub fpu_area_size: u64,
    /// The alignment a floating-point save area must have.
    pub fpu_area_align: u64,
    /// The cycle counter's frequency in kHz, or 0 if the host has no invariant
    /// counter to offer.
    pub tsc_khz: u64,
    /// Non-zero if the kernelet should mirror its guard depth into the host's
    /// preemption count. The endovisor's module parameter, for the experiment
    /// that shows what the mirror buys.
    pub mirror_preempt: u64,
    /// The sandbox's command line, from KLET_CREATE: what a bootloader's
    /// kernel command line is to a machine.
    pub cmdline_ptr: u64,
    pub cmdline_len: u64,
    /// The bounds of this image's text, as the host sees them. The upcall stub
    /// range-checks the interrupted instruction pointer against them before
    /// returning to it.
    pub image_text_lo: u64,
    pub image_text_hi: u64,
    /// The endovisor's array of [`SpaceRec`], one per space slot.
    pub space_recs: u64,
    /// How many records the array has.
    pub nr_space_recs: u64,
    /// The endovisor's [`MemStats`]: counters of the memory paths, for tests.
    pub mem_stats: u64,
    /// 4, or 5 if the host runs five-level paging: the depth vOSTD's lockless
    /// walk must follow.
    pub pgtable_levels: u64,
}

/// The vm_* services' errors, returned negated.
pub const KLET_INVALID: i64 = 1;
pub const KLET_NOT_OWNED: i64 = 2;
pub const KLET_LIMIT: i64 = 3;
pub const KLET_NOMEM: i64 = 5;
pub const KLET_STATE: i64 = 6;

/// What crosses for a protection: three bits and a cache policy. The endovisor
/// builds the host's page-table bits from these; the kernelet never supplies
/// page-table bits of its own.
pub const KLET_PROT_READ: u32 = 1 << 0;
pub const KLET_PROT_WRITE: u32 = 1 << 1;
pub const KLET_PROT_EXEC: u32 = 1 << 2;
/// The experiment hook: a `vm_map` with this bit sleeps in the host, when the
/// endovisor was loaded with `klet_test_hooks=1`.
pub const KLET_PROT_TEST_SLEEP: u32 = 1 << 31;

/// The range lock's words per space.
pub const SPACE_LOCK_WORDS: usize = 16;

/// One space's shared record. Mirrors `struct klet_space_rec`.
#[repr(C)]
pub struct SpaceRec {
    /// The physical address of the space's page-table root (Linux's `mm->pgd`).
    pub root_paddr: AtomicU64,
    /// Bumped by every `vm_create` of this slot.
    pub gen: AtomicU64,
    /// The range lock's words: the kernelet's, written with compare-and-swap.
    /// Writer bit 63, waiters bit 62, writer-waiting bit 61, the reader count
    /// below.
    pub lock: [AtomicU64; SPACE_LOCK_WORDS],
}

/// Counters of the host's memory paths. Mirrors `struct klet_mem_stats`. They
/// are instrumentation for the tests, read through [`crate::klet`], and no part
/// of the design: a kernel proper never needs them.
#[repr(C)]
#[derive(Debug)]
pub struct MemStats {
    pub faults: AtomicU64,
    pub mkwrites: AtomicU64,
    pub reported: AtomicU64,
    pub kernel_faults: AtomicU64,
    pub maps: AtomicU64,
    pub unmaps: AtomicU64,
    pub unmapped: AtomicU64,
    pub protects: AtomicU64,
    pub protected: AtomicU64,
    pub flushes: AtomicU64,
    pub flush_alls: AtomicU64,
    pub waits: AtomicU64,
    pub wait_sleeps: AtomicU64,
    pub wakes: AtomicU64,
    pub spaces: AtomicU64,
    pub lifted: AtomicU64,
}

/// The host functions the kernelet may call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Services {
    /// Must equal [`KLET_ABI_VERSION`].
    pub abi_version: u64,
    /// Writes one line of the kernel log: vOSTD's own lines. No trailing
    /// newline.
    pub log_write: unsafe extern "C" fn(ptr: *const u8, len: u64),
    /// Allocates `size` bytes aligned to `align` from the host's heap.
    pub mem_alloc: unsafe extern "C" fn(size: u64, align: u64) -> *mut u8,
    /// Frees a block from [`Services::mem_alloc`].
    pub mem_free: unsafe extern "C" fn(ptr: *mut u8, size: u64, align: u64),
    /// Enters user mode with `ctx`.
    ///
    /// Returns 0 for a system call, 1 for an exception (with `trap_num`,
    /// `error_code` and `fault_addr` filled in) and 2 for "an event; loop
    /// again".
    pub user_run: unsafe extern "C" fn(ctx: *mut RawUserContext) -> u64,
    /// Creates a Linux address space with its one area over the whole user
    /// range. Returns the space id (> 0) or a negated error. Sleeps.
    pub vm_create: unsafe extern "C" fn() -> i64,
    /// Drops the endovisor's reference on the space. Sleeps.
    pub vm_destroy: unsafe extern "C" fn(space: u64) -> i64,
    /// Records the vCPU's current space (0: none), which the carrier adopts on
    /// its way to user mode. Never sleeps.
    pub vm_activate: unsafe extern "C" fn(space: u64) -> i64,
    /// Maps one frame of the grant. Sleeps when a page-table page is needed.
    pub vm_map: unsafe extern "C" fn(space: u64, va: u64, paddr: u64, prot: u32) -> i64,
    /// Clears every present entry of a range (clipped to the area) and flushes
    /// it before returning. The count is vOSTD's own. Sleeps.
    pub vm_unmap: unsafe extern "C" fn(space: u64, va: u64, len: u64) -> i64,
    /// Rewrites one page's present entry in place; no flush. Sleeps (the
    /// address-space lock).
    pub vm_protect: unsafe extern "C" fn(space: u64, va: u64, prot: u32) -> i64,
    /// Flushes a range (`len == u64::MAX`: everything) on every processor in
    /// the space's CPU mask, synchronously. Never sleeps.
    pub vm_flush: unsafe extern "C" fn(space: u64, va: u64, len: u64) -> i64,
    /// The range lock's slow path: sleeps unless lock word `stripe` of the
    /// space's record no longer holds `expected`. On a kill it does not return.
    pub vm_wait: unsafe extern "C" fn(space: u64, stripe: u32, expected: u64) -> i64,
    /// Wakes every carrier waiting on the space's lock words. Never sleeps.
    pub vm_wake: unsafe extern "C" fn(space: u64, stripe: u32) -> i64,
    /// Takes a kernelet stack out of the sandbox's pool and returns its lowest
    /// address, or 0. Every stack is the same fixed size; the kernelet's build is
    /// told which through `OSTD_TASK_STACK_SIZE_IN_PAGES`.
    pub kstack_alloc: unsafe extern "C" fn() -> u64,
    /// Frees a kernelet stack from [`Services::kstack_alloc`].
    pub kstack_free: unsafe extern "C" fn(base: u64),
    /// Starts virtual CPU `id`, which enters the image at `vcpu_entry`.
    pub vcpu_boot: unsafe extern "C" fn(id: u64),
    /// Makes virtual CPU `id` take a KICK virtual interrupt as soon as it can.
    pub vcpu_kick: unsafe extern "C" fn(id: u64),
    /// Sleeps until this vCPU has a pending virtual interrupt, `deadline_ns`
    /// nanoseconds pass (0 means no deadline), or the sandbox is dying.
    pub vcpu_idle: unsafe extern "C" fn(deadline_ns: u64),
    /// Gives the host processor back: the kernelet's guard depth just dropped
    /// to zero and the host wants to reschedule.
    pub vcpu_yield: unsafe extern "C" fn(),
    /// Called by a spin lock that has spun for a long time: the holder may be
    /// a vCPU the host has descheduled.
    pub vcpu_on_spin: unsafe extern "C" fn(),
    /// Saves this host task's live floating-point registers into `area`.
    pub fpu_save: unsafe extern "C" fn(area: *mut u8, len: u64),
    /// Loads `area` into this host task's floating-point registers.
    pub fpu_load: unsafe extern "C" fn(area: *const u8, len: u64),
    /// Stops the instance. Never returns.
    pub stop: unsafe extern "C" fn(code: u64) -> !,
    /// Writes one console line, what the kernel proper prints. No trailing
    /// newline. [`Services::log_write`] is the kernel log, vOSTD's own lines.
    pub console_write: unsafe extern "C" fn(ptr: *const u8, len: u64),
    /// Performs an experimental owner-scoped socket or ring operation.
    pub vsock: unsafe extern "C" fn(req: *mut crate::vsock::Request) -> i64,
}

/// The kernelet functions the host may call.
#[repr(C)]
pub struct EntryTable {
    /// Must equal [`KLET_ABI_VERSION`].
    pub abi_version: u64,
    /// Initialises the instance and runs `__ostd_main`, on virtual CPU 0.
    pub kernelet_entry: unsafe extern "C" fn(boot: *const BootArgs, svc: *const Services),
    /// The entry point of virtual CPU `id` (OSTD's application-processor entry).
    pub vcpu_entry: unsafe extern "C" fn(id: u64),
    /// The virtual-interrupt stub. The endovisor points an interrupted
    /// instruction pointer here; the stub is never called.
    pub virq_entry: unsafe extern "C" fn(),
}

// SAFETY: the table is read-only after link time and contains only function
// pointers and an integer.
unsafe impl Sync for EntryTable {}

/// One virtual CPU's record: a page the endovisor allocates and both sides
/// write.
///
/// `pending` is written by the endovisor from any processor and cleared by
/// vOSTD, so it is atomic. `preempt_info` is written only by the vCPU that owns
/// the record, and read by the endovisor from a hard interrupt on the same
/// processor, so a plain aligned 32-bit store is enough and an atomic
/// read-modify-write would only cost a `lock` prefix on the hottest path there
/// is.
#[repr(C)]
pub struct VcpuRec {
    /// This vCPU's index.
    pub vcpu_id: u32,
    /// How many vCPUs the sandbox has.
    pub nr_vcpus: u32,
    /// Virtual interrupts the endovisor has raised and vOSTD has not taken.
    pub pending: AtomicU32,
    /// OSTD's preemption word: bits 0..30 are the guard count, including spin
    /// locks, bit 31 is `!need_preempt`. A guard does *not* mask this vCPU's
    /// interrupts, exactly as a preemption guard on bare metal does not mask the
    /// processor's: it only forbids the task switch.
    pub preempt_info: UnsafeCell<u32>,
    /// The virtual interrupt flag, inverted: non-zero means this vCPU's
    /// interrupts are masked. vOSTD's [`crate::irq::DisabledLocalIrqGuard`] sets
    /// it, and so does the endovisor at the moment it redirects into the upcall
    /// stub — which is what hardware does to IF when it takes an interrupt. The
    /// stub clears it at the end, as `iret` would. It starts set, as a processor
    /// comes out of reset with IF clear: vOSTD clears it once it can take an
    /// interrupt, which is once the record can be found.
    pub irq_off: UnsafeCell<u32>,
    /// Non-zero exactly while the host's per-processor preemption count includes
    /// the kernelet's one. It is *not* the same as "in a critical section": it is
    /// set just after the host's count goes up and cleared just before it comes
    /// down, so that the endovisor can tell a genuine failure of the mirror from
    /// the two-instruction windows in which the kernelet is between the two
    /// updates and is about to fix it.
    pub mirrored: UnsafeCell<u32>,
    /// Where the endovisor interrupted the kernelet, for `virq_entry`.
    pub virq_ip: UnsafeCell<u64>,
    /// The address space this vCPU has activated: a space id, or 0 for none.
    /// Written by `vm_activate`, read by the endovisor when the carrier goes to
    /// user mode, to know which Linux address space to adopt.
    pub space: UnsafeCell<u64>,
    /// The low bound of the kernelet stack the current OSTD task is on, written
    /// by vOSTD at every task switch. The endovisor will not push an upcall onto
    /// a stack with too little room left.
    pub stack_limit: UnsafeCell<u64>,
    /// How long the host has kept this vCPU off a processor while it was running
    /// kernelet code. Written by the endovisor.
    pub stolen_ns: UnsafeCell<u64>,
}

// SAFETY: every field the kernelet writes belongs to the one virtual CPU that
// owns the record, and every update is a single instruction, so the endovisor —
// the only other party — never sees a torn value. `UnsafeCell` is
// `repr(transparent)`, so the layout the endovisor's C mirror describes is
// unchanged.
unsafe impl Sync for VcpuRec {}

/// Virtual interrupt lines.
pub const VIRQ_TICK: u32 = 1 << 0;
pub const VIRQ_KICK: u32 = 1 << 1;
/// The summary bit for device lines. Nothing raises it in this prototype.
pub const VIRQ_LINES: u32 = 1 << 2;

/// The one symbol the endovisor resolves in the kernelet image.
#[no_mangle]
pub static kernelet_entry_table: EntryTable = EntryTable {
    abi_version: KLET_ABI_VERSION,
    kernelet_entry: crate::kernelet_entry,
    vcpu_entry: crate::vcpu_entry,
    virq_entry: crate::vcpu::klet_virq_entry,
};

static SERVICES: AtomicUsize = AtomicUsize::new(0);
static LINEAR_MAP_BASE: AtomicUsize = AtomicUsize::new(0);
static SPACE_RECS: AtomicUsize = AtomicUsize::new(0);
static NR_SPACE_RECS: AtomicUsize = AtomicUsize::new(0);
static MEM_STATS: AtomicUsize = AtomicUsize::new(0);

/// Records the tables handed to [`crate::kernelet_entry`].
///
/// # Safety
///
/// `boot` and `svc` must point to valid tables that outlive the instance.
pub(crate) unsafe fn init_tables(boot: *const BootArgs, svc: *const Services) {
    assert_eq!((*svc).abi_version, KLET_ABI_VERSION);
    assert_eq!((*boot).abi_version, KLET_ABI_VERSION);
    LINEAR_MAP_BASE.store((*boot).linear_map_base as usize, Ordering::Relaxed);
    // The walk is four-level; refuse a five-level host rather than misread it.
    assert_eq!(
        (*boot).pgtable_levels,
        4,
        "the host uses {}-level page tables; this vOSTD walks four",
        (*boot).pgtable_levels
    );
    SPACE_RECS.store((*boot).space_recs as usize, Ordering::Relaxed);
    NR_SPACE_RECS.store((*boot).nr_space_recs as usize, Ordering::Relaxed);
    MEM_STATS.store((*boot).mem_stats as usize, Ordering::Relaxed);
    SERVICES.store(svc as usize, Ordering::Release);
    // Last: this is what makes the vCPU record reachable and the mirror live.
    crate::vcpu::init_host_offsets(&*boot);
}

/// The service table.
#[inline]
pub(crate) fn svc() -> &'static Services {
    let ptr = SERVICES.load(Ordering::Acquire);
    debug_assert!(ptr != 0);
    // SAFETY: `init_tables` stored a pointer to a table that outlives us.
    unsafe { &*(ptr as *const Services) }
}

/// The base of the host's linear map.
#[inline]
pub(crate) fn linear_map_base() -> usize {
    LINEAR_MAP_BASE.load(Ordering::Relaxed)
}

/// The shared record of space `id` (1-based), in the endovisor's memory.
pub(crate) fn space_rec(id: u64) -> &'static SpaceRec {
    let base = SPACE_RECS.load(Ordering::Relaxed);
    let n = NR_SPACE_RECS.load(Ordering::Relaxed);
    assert!(base != 0 && id >= 1 && (id as usize) <= n, "no record for space {}", id);
    // SAFETY: the endovisor allocated `n` records at `base` for the instance's
    // lifetime, and every field is atomic.
    unsafe { &*((base as *const SpaceRec).add(id as usize - 1)) }
}

/// The host's memory-path counters, if it publishes them.
pub(crate) fn mem_stats() -> Option<&'static MemStats> {
    let p = MEM_STATS.load(Ordering::Relaxed);
    if p == 0 {
        None
    } else {
        // SAFETY: the endovisor allocated the counters for the instance's
        // lifetime, and every field is atomic.
        Some(unsafe { &*(p as *const MemStats) })
    }
}

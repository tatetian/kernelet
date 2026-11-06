// SPDX-License-Identifier: GPL-2.0
// The endovisor connects the kernelet image ABI to the endovisor UAPI.
//
// Kernelet images are files, registered as kinds through `KLET_KIND_REGISTER`
// and validated by the endovisor itself in `image.c`.
// A kind shares immutable pages across independent PIE mappings
// with private data and relocations.
// A sandbox is one `struct klet_inst`,
// in a table of `KLET_MAX_SANDBOXES` slots in `sandbox.c`.
//
// A carrier is a Linux task that carries one of the sandbox's virtual CPUs:
// a child the holder forked that executed the virtual CPU's exec descriptor.
// OSTD tasks are the kernelet's own objects on their own kernelet stacks,
// and the kernelet's own scheduler multiplexes them onto the vCPUs.
// Linux schedules the carriers and never sees a kernelet task.
#ifndef _KLET_ENDOVISOR_H
#define _KLET_ENDOVISOR_H

#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/workqueue.h>
#include <linux/kernelet.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "uapi/endovisor.h"

/* ------------------------------------------------------------------ */
/* The image ABI. Mirrors vostd/src/abi.rs; keep the two in step.     */
/* ------------------------------------------------------------------ */

#define KLET_ABI_VERSION 7ULL

/* Errors of the vm_* services, returned negated. */
#define KLET_INVALID	1	/* an address outside the area, a bad argument, a space not this kernelet's, an entry already present */
#define KLET_NOT_OWNED	2	/* a frame outside the grant */
#define KLET_LIMIT	3	/* the configured maximum of spaces */
#define KLET_NOMEM	5	/* vm_create: Linux could not allocate the address space */
#define KLET_STATE	6	/* vm_wait with the vCPU's interrupts off: the sleeping contract */
/* KLET_LIMIT is also a vm_map whose page-table page Linux could not allocate. */

/*
 * What crosses for a protection: three bits and a cache policy. The endovisor
 * builds Linux's pgprot_t from its own constants; a kernelet never supplies
 * raw page-table bits.
 */
#define KLET_PROT_READ		(1u << 0)
#define KLET_PROT_WRITE		(1u << 1)
#define KLET_PROT_EXEC		(1u << 2)
#define KLET_PROT_CACHE_SHIFT	4	/* 3 bits; only 0, write-back, is accepted */
#define KLET_PROT_CACHE_MASK	(7u << KLET_PROT_CACHE_SHIFT)
#define KLET_PROT_LARGE		(1u << 8)	/* a 2 MiB page: refused in this version */
/* Test hook, honoured only with klet_test_hooks=1: sleep inside vm_map. */
#define KLET_PROT_TEST_SLEEP	(1u << 31)
#define KLET_PROT_VALID		(KLET_PROT_READ | KLET_PROT_WRITE | KLET_PROT_EXEC | \
				 KLET_PROT_CACHE_MASK | KLET_PROT_LARGE | KLET_PROT_TEST_SLEEP)

/* How many address spaces one kernelet may have at once. */
#define KLET_MAX_SPACES 64
/* The range lock's words per space: one per 2 MiB region, modulo this. */
#define KLET_SPACE_LOCK_WORDS 16

/*
 * One space's record, shared: the endovisor writes the root when it creates
 * the space; the range lock's words are the kernelet's, written with CAS, and
 * read by vm_wait. Lives in the endovisor's memory so that vm_wait need not
 * validate a kernelet pointer.
 */
struct klet_space_rec {
	u64 root_paddr;		/* __pa(mm->pgd); 0 while the slot is free */
	u64 gen;		/* bumped by every vm_create of this slot */
	/* writer 63, waiters 62, writer-waiting 61, reader count below */
	u64 lock[KLET_SPACE_LOCK_WORDS];
};

/*
 * Counters of the memory paths, shared read-only with the kernelet so that a
 * test kernel can take deltas around a test. Instrumentation for the tests, no
 * part of the design: a kernel proper never needs them.
 */
struct klet_mem_stats {
	atomic64_t faults;		/* calls of the area's ->fault */
	atomic64_t mkwrites;		/* calls of the area's ->pfn_mkwrite */
	atomic64_t reported;		/* of those, recorded as a tenant exception */
	atomic64_t kernel_faults;	/* kernel-mode accesses, answered SIGBUS */
	atomic64_t maps;		/* vm_map calls */
	atomic64_t unmaps;		/* vm_unmap calls */
	atomic64_t unmapped;		/* entries cleared by them */
	atomic64_t protects;		/* vm_protect calls */
	atomic64_t protected_;		/* entries rewritten by them */
	atomic64_t flushes;		/* vm_flush calls over a range */
	atomic64_t flush_alls;		/* vm_flush calls for everything */
	atomic64_t waits;		/* vm_wait calls */
	atomic64_t wait_sleeps;		/* of those, ones that slept */
	atomic64_t wakes;		/* vm_wake calls */
	atomic64_t spaces;		/* spaces created */
	atomic64_t lifted;		/* vm_* entered with the kernelet's mirror raised, lifted */
};

struct klet_boot_args {
	u64 abi_version;
	u64 grant_paddr;
	u64 grant_len;
	u64 linear_map_base;
	u64 nr_vcpus;
	/*
	 * How vOSTD finds its own vCPU record, and mirrors its guard depth into
	 * Linux's preemption count, without calling a service: two dependent
	 * loads from gs: and one instruction.
	 */
	u64 gs_current_task;
	u64 gs_preempt_count;
	u64 task_hooks_off;
	u64 hooks_vrec_off;
	u64 fpu_area_size;
	u64 fpu_area_align;
	u64 tsc_khz;
	u64 mirror_preempt;
	/* The sandbox's command line, from KLET_CREATE. */
	u64 cmdline_ptr;
	u64 cmdline_len;
	/* The bounds of the image's text, for the stub's own range check. */
	u64 image_text_lo;
	u64 image_text_hi;
	/* Where the address spaces' shared state is. */
	u64 space_recs;		/* struct klet_space_rec[KLET_MAX_SPACES] */
	u64 nr_space_recs;
	u64 mem_stats;		/* struct klet_mem_stats */
	u64 pgtable_levels;	/* 4, or 5 under LA57: what vOSTD's walk must follow */
};

/* The eighteen general registers, in the order vOSTD's GeneralRegs has them. */
struct klet_user_ctx {
	u64 rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	u64 r8, r9, r10, r11, r12, r13, r14, r15;
	u64 rip, rflags;
	u64 trap_num;
	u64 error_code;
	u64 fault_addr;
};

/*
 * One virtual CPU's record: written by both sides.
 *
 * `preempt_info` is OSTD's preemption word. Bits 0..30 are the guard depth and
 * bit 31 is `!need_preempt`. While the depth is non-zero or `irq_off` is set,
 * vOSTD has raised Linux's own per-processor preemption count by exactly one
 * (the mirror), however deep it nests. vOSTD updates the word with single
 * read-modify-write instructions; the endovisor only reads
 * it, and only to decide whether to deliver a virtual interrupt now or leave it
 * pending. It is never trusted for anything that must be right — in particular
 * un-mirroring measures Linux's own count instead.
 */
struct klet_vcpu_rec {
	u32 vcpu_id;
	u32 nr_vcpus;
	atomic_t pending;
	/*
	 * OSTD's preemption word: the guard count in bits 0..30, `!need_preempt` in
	 * bit 31. A guard does *not* mask this vCPU's interrupts, exactly as a
	 * preemption guard on bare metal does not mask the processor's.
	 */
	u32 preempt_info;
	/*
	 * The virtual interrupt flag, inverted. vOSTD sets it for a critical
	 * section; the endovisor sets it at the moment it redirects into the upcall
	 * stub, which is what hardware does to IF; the stub clears it at the end, as
	 * iret would. It starts at 1, as a processor comes out of reset with IF
	 * clear, and vOSTD clears it once it can take a virtual interrupt.
	 */
	u32 irq_off;
	/*
	 * Non-zero exactly while Linux's per-processor preemption count includes the
	 * kernelet's one. Not the same as "critical": the kernelet sets it after the
	 * count goes up and clears it before the count comes down, so this — and not
	 * the guard count — is what says whether an involuntary deschedule was a
	 * failure of the mirror or happened in a window the kernelet was already
	 * leaving.
	 */
	u32 mirrored;
	u64 virq_ip;
	/*
	 * The address space this vCPU has activated: a space id, 0 for none.
	 * Written by vm_activate, read on the way to user mode to know which
	 * Linux address space the carrier must adopt.
	 */
	u64 space;
	/*
	 * The low bound of the kernelet stack the current OSTD task is on, written
	 * by vOSTD at every task switch. The endovisor refuses to deliver an upcall
	 * unless there is room below the interrupted rsp for Linux's own reserve
	 * and the upcall frame, because the upcall is pushed on *that* stack.
	 */
	u64 stack_limit;
	/* How long the host has kept this vCPU off a processor while it was
	 * running kernelet code. Written by the endovisor, read by the kernelet. */
	u64 stolen_ns;
};

/*
 * How much of a kernelet stack must be left below the interrupted instruction
 * for an upcall to be delivered there: 16 KiB for Linux's own use on this task
 * (a hard interrupt taken on top of the kernelet stack) and 16 KiB for the
 * upcall's own frames.
 */
#define KLET_UPCALL_HEADROOM (32 * 1024)

#define KLET_PREEMPT_GUARD_MASK 0x7fffffffu

/* Virtual interrupt lines. */
#define KLET_VIRQ_TICK  (1u << 0)
#define KLET_VIRQ_KICK  (1u << 1)
/* The summary bit for device lines. Nothing raises it in this prototype. */
#define KLET_VIRQ_LINES (1u << 2)

struct klet_vsock_req;

struct klet_services {
	u64 abi_version;
	void (*log_write)(const char *ptr, u64 len);
	void *(*mem_alloc)(u64 size, u64 align);
	void (*mem_free)(void *ptr, u64 size, u64 align);
	u64 (*user_run)(struct klet_user_ctx *ctx);
	/* Tenant memory: a VmSpace is a Linux address space (space.c). */
	s64 (*vm_create)(void);
	s64 (*vm_destroy)(u64 space);
	s64 (*vm_activate)(u64 space);
	s64 (*vm_map)(u64 space, u64 va, u64 paddr, u32 prot);
	s64 (*vm_unmap)(u64 space, u64 va, u64 len);
	s64 (*vm_protect)(u64 space, u64 va, u32 prot);
	s64 (*vm_flush)(u64 space, u64 va, u64 len);
	s64 (*vm_wait)(u64 space, u32 stripe, u64 expected);
	s64 (*vm_wake)(u64 space, u32 stripe);
	u64 (*kstack_alloc)(void);
	void (*kstack_free)(u64 base);
	void (*vcpu_boot)(u64 id);
	void (*vcpu_kick)(u64 id);
	void (*vcpu_idle)(u64 deadline_ns);
	void (*vcpu_yield)(void);
	void (*vcpu_on_spin)(void);
	void (*fpu_save)(void *area, u64 len);
	void (*fpu_load)(const void *area, u64 len);
	void (*stop)(u64 code);
	/* The console, what the kernel proper prints; log_write is vOSTD's log. */
	void (*console_write)(const char *ptr, u64 len);
	// Performs an experimental owner-scoped socket or ring operation.
	s64 (*vsock)(struct klet_vsock_req *req);
};

struct klet_entry_table {
	u64 abi_version;
	void (*kernelet_entry)(const struct klet_boot_args *boot,
			       const struct klet_services *svc);
	void (*vcpu_entry)(u64 id);
	/* Never called: the endovisor points an interrupted regs->ip here. */
	void (*virq_entry)(void);
};

/* Return values of the user_run service. */
#define KLET_RUN_SYSCALL	0ULL
#define KLET_RUN_EXCEPTION	1ULL
#define KLET_RUN_EVENT		2ULL

/* ------------------------------------------------------------------ */
/* Endovisor-internal types                                           */
/* ------------------------------------------------------------------ */

/* The tables of the UAPI: registered kinds and live sandboxes. */
#define KLET_MAX_KINDS		8
#define KLET_MAX_SANDBOXES	8

/*
 * The grant is one physically contiguous run, allocated at the sandbox's start,
 * of memory_initial bytes in whole 2 MiB units: at most order 10, 4 MiB, the
 * largest run the page allocator gives (MAX_PAGE_ORDER).
 */
#define KLET_GRANT_UNIT		(2UL << 20)
#define KLET_GRANT_MAX		(PAGE_SIZE << MAX_PAGE_ORDER)

/*
 * The first bytes of an exec descriptor's file, which the binary-format handler
 * uses only to turn other files away quickly: the file is recognized by its
 * inode, never by these bytes (sandbox.c).
 */
#define KLET_EXEC_MAGIC		"KLET-EXEC"
#define KLET_EXEC_MAGIC_LEN	10	/* with its NUL */
struct klet_exec_head {
	char magic[KLET_EXEC_MAGIC_LEN];
	u16 slot;
	u32 vcpu;
} __packed;

/* Each of the console and log rings. */
#define KLET_OUT_RING		(64 * 1024)

/*
 * Every kernelet stack is this big, and they all come from one per-sandbox pool
 * that is never given back to vmalloc until the sandbox is destroyed. A kernel
 * that forks and exits churns task stacks, and vfree() of a vmalloc area costs
 * the whole machine a lazy-purge TLB flush eventually; a pool costs the sandbox
 * a few megabytes of address space and the machine nothing. The kernelet's build
 * is told the same number through OSTD_TASK_STACK_SIZE_IN_PAGES.
 */
#define KLET_STACK_SIZE (256 * 1024)

/*
 * The bound on the pool is the sandbox's max_tasks (KLET_CREATE) plus one
 * bootstrap stack per virtual CPU. Every stack is charged to the sandbox's
 * memory cgroup, and the bound keeps a kernel proper that spawns tasks without
 * end from taking more.
 */

/* The lowest address of a space's area: the tree's VMAR_LOWEST_ADDR, 64 KiB. */
#define KLET_WINDOW_START 0x10000UL

/* The eviction sweep's period. */
#define KLET_SWEEP_INTERVAL_NS (1000 * 1000)

/* The per-processor watch timer's period: the tick, the preemption point for a
 * kernelet that has none, and the virtual-interrupt delivery point. */
#define KLET_WATCH_INTERVAL_NS (1000 * 1000)

/* An idle vCPU with no deadline of its own sleeps to the next tick. */
#define KLET_IDLE_MAX_NS KLET_WATCH_INTERVAL_NS
/* And never for less than this, or arming the timer costs more than the sleep. */
#define KLET_IDLE_MIN_NS (50 * 1000)

/* What a carrier is doing. (The sandbox's own states are the UAPI's.) */
enum klet_cstate {
	KLET_NEW,	/* the vCPU has not entered the image yet */
	KLET_RUNNING,	/* the carrier is running kernelet code */
	KLET_USER,	/* the carrier is in, or on its way to, user mode */
	KLET_DEAD,	/* the vCPU has stopped or been abandoned */
};

struct klet_inst;

/*
 * One address space of the kernelet's (a vOSTD VmSpace): a Linux mm from
 * kernelet_mm_alloc(), a file with an inode of its own, and the one PFN-mapped
 * area over the whole user range. The mm's page table *is* the tenant
 * process's page table.
 */
struct klet_space {
	struct klet_inst *inst;
	unsigned int id;		/* slot + 1; 0 is "no space" */
	bool live;
	bool dying;
	struct mm_struct *mm;		/* the endovisor's mm_users reference, until vm_destroy */
	struct file *file;		/* the endovisor's reference */
	unsigned long start, end;	/* the area; found with vma_lookup() on every use */
	/* Service calls in flight: vm_destroy waits for them. */
	atomic_t users;
	/* The range lock's slow path: carriers waiting for a lock word. */
	wait_queue_head_t wq;
	struct klet_space_rec *rec;
};

struct klet_carrier {
	/* Must be first: the published hooks->vrec offset assumes nothing, but
	 * keeping the hooks first makes the two offsets easy to read. */
	struct carrier_event_hooks hooks;
	/* The shared record. Its offset from `hooks` is in the boot arguments. */
	struct klet_vcpu_rec vrec;

	struct klet_inst *inst;		/* a reference, for as long as the record lives */
	struct task_struct *task;
	enum klet_cstate state;
	int vcpu;

	/* the bootstrap coroutine of this vCPU */
	void *stack;
	void *coro_sp;
	void *linux_sp;
	u64 run_ret;
	struct klet_user_ctx *ctx;

	/*
	 * How deep the carrier is inside host services. Zero means the kernelet
	 * is running its own code on its own stack, which is the only state in
	 * which the watch timer may hijack its instruction pointer.
	 */
	int depth;

	/*
	 * The mirror. `pc_base` is Linux's preempt_count() as it was when this
	 * carrier last entered kernelet code; `excess` is how much above that the
	 * kernelet had raised it when the carrier last left, which is what has to
	 * go back on the way in. Measuring Linux's own count is deliberate: the
	 * kernelet writes `masked`, and nothing the kernelet writes may decide how
	 * much the endovisor subtracts from Linux's state.
	 */
	int pc_base;
	int excess;
	/*
	 * Linux's count as klet_mirror_out found it when it was neither pc_base
	 * nor pc_base + 1, or -1. klet_mirror_out may run on a kernelet stack, where
	 * no Linux function may be called, so the warning is printed by klet_enter
	 * once the carrier is back on its Linux stack.
	 */
	int mirror_fault_pc;

	/*
	 * Set on the Linux stack when a fatal signal is found pending on the
	 * carrier, in a service (klet_svc_body) or by the watch timer in kernelet
	 * code: the carrier must not go back into the kernelet, but leave it for
	 * good and die, so that a SIGKILL ends the sandbox wherever the carrier
	 * was. `killed_in` says which, for the log.
	 */
	bool doomed;
	const char *killed_in;
	/* The resume hook has said that Linux made a pending signal fatal. */
	bool forced_fatal_seen;

	/* Set while the carrier is asleep in vcpu_idle. */
	bool idle;
	/* When the host last took this carrier off a processor, and the sum. */
	u64 off_cpu_ns;
	/* Rate limit for vcpu_on_spin: at most once per tick. */
	unsigned long last_on_spin;
	/* When the first of two strikes was recorded. */
	u64 strike1_ns;
	/* Where fpu_save writes and fpu_load reads: never the kernelet's buffer. */
	void *fpu_stage;

	/* address spaces */
	struct mm_struct *blank_mm;	/* the carrier's own, empty one */
	struct mm_struct *cur_mm;	/* whatever it has adopted */

	/*
	 * pt_regs hold the tenant's live registers, because ctx was applied to
	 * them and the tenant has run since. Only then may the resume hook take a
	 * snapshot of them into ctx before entering the kernelet for an exception
	 * or an event.
	 */
	bool user_live;

	/* a pending exception to report through user_run */
	bool exc_pending;
	u64 exc_trap;
	u64 exc_error;
	u64 exc_addr;

	/* the eviction sweep set our instruction pointer to the exit stub */
	bool evicted;

	/* where the yield stub has to put us back */
	unsigned long yield_ip;

	/* the boot handshake for a vCPU other than 0, and where this carrier
	 * sleeps: waiting to be booted, and waiting in vcpu_idle */
	bool boot_requested;
	wait_queue_head_t idle_wq;

	/*
	 * Set by the watch timer when it redirects into the yield stub because a
	 * critical section kept the processor past its second strike (strike1_ns
	 * above), so that the yield is counted as a forced one.
	 */
	bool forced;

#ifdef CONFIG_PREEMPT_NOTIFIERS
	struct preempt_notifier pnotifier;
	bool pnotifier_on;
#endif

	refcount_t refs;
	struct list_head link;
};

/* What the log says about the first few forced yields of a sandbox. */
#define KLET_FORCED_RECORDS 3
struct klet_forced_rec {
	int vcpu, pid, cpu;
	unsigned long ip;
	u32 guards, irq_off;
};

/*
 * Counters, per sandbox, and added into the module's totals (klet_totals) when
 * the sandbox is destroyed; the module prints the totals when it unloads, which
 * is what the tests read. Every field is an atomic_t that is summed.
 */
struct klet_counters {
	atomic_t evictions;
	atomic_t ticks;
	atomic_t watch_fires;		/* requested firings of the watch timer (kicks) */
	atomic_t kick_ipis;		/* kick IPIs that found their carrier running */
	atomic_t fires_redirected;	/* requested firings that delivered an upcall */
	atomic_t fires_elsewhere;	/* ... that found user mode, a service or Linux code */
	atomic_t fires_masked;		/* ... that found a critical section */
	atomic_t virqs;			/* upcalls delivered by redirect */
	atomic_t deferrals;		/* ticks that found the vCPU masked */
	atomic_t forced_yields;
	atomic_t yields;		/* voluntary vcpu_yield service calls */
	atomic_t bad_preempt;		/* involuntary switch-out while masked */
	atomic_t on_spins;
	atomic_t yield_tos;
	atomic_t mm_switches;
	atomic_t fpu_swaps;
	atomic_t fpu_rejects;
	atomic_t mirror_faults;
	/* Where a fatal signal to a carrier of the live sandbox was acted on. */
	atomic_t kills_in_service;
	atomic_t kills_in_kernelet;
	atomic_t stolen_ms;
};

// A loadable segment of the restricted PIE image.
struct klet_image_segment {
	u64 offset, filesz, memsz;
	u32 flags;
};

// A relocation whose target is private to an instance.
struct klet_image_reloc {
	u64 offset, addend;
};

#define KLET_IMAGE_SEGMENTS 8
#define KLET_IMAGE_SPAN_MAX (16UL << 20)

// One registered PIE image, with immutable pages shared by its instances.
struct klet_kind {
	bool used;
	char name[KLET_NAME_LEN];
	u8 *image;			// Unrelocated template, including zeroed BSS.
	size_t image_size;
	struct page **shared_pages;	// `NULL` for pages private to each instance.
	unsigned int nr_pages, nr_segments, nr_relocs;
	struct klet_image_segment segments[KLET_IMAGE_SEGMENTS];
	struct klet_image_reloc *relocs;
	u64 entry_offset;
	size_t text_size, data_size;	// Shared and private mapped bytes.
	unsigned long text_lo, text_hi;	// Executable offsets within the image.
	unsigned int nr_sandboxes;
};

/* The sandbox's view of one virtual CPU, for KLET_GET_STATS. */
struct klet_vcpu_slot {
	u32 state;			/* enum klet_vcpu_state, the UAPI's */
	bool claimed;			/* an exec of its descriptor got past the checks */
	pid_t pid;
	u64 run_ns, stolen_ns;		/* frozen when the carrier is gone */
	struct file *exec_file;		/* the exec descriptor's file, made lazily */
};

/* One of the console and the log: the holder's file and the ring before it. */
struct klet_out {
	struct file *file;
	struct kfifo fifo;
	spinlock_t lock;
};

struct klet_inst {
	// The experimental channel identity and runtime-authorized peers.
	u64 vsock_cid;
	kuid_t vsock_uid;
	u64 vsock_allow[16];
	unsigned int vsock_nr_allow;
	refcount_t refs;		/* the sandbox file's, and each carrier record's */
	struct klet_kind *kind;
	void *image_base;
	struct page **image_pages;
	const struct klet_entry_table *entry;
	unsigned long text_lo, text_hi;
	int slot;
	struct mutex mutex;		/* the sandbox operations, against each other */

	/* KLET_CREATE */
	u64 memory_initial, memory_max, max_tasks;
	struct cgroup *cgrp;		/* the sandbox's control group, held */

	/* the grant */
	struct page *grant_pages;
	unsigned int grant_order;
	struct klet_boot_args boot;

	unsigned int nr_vcpus;
	struct klet_carrier *vcpus[KLET_MAX_VCPUS];
	struct klet_vcpu_slot vslot[KLET_MAX_VCPUS];

	spinlock_t lock;
	/* vCPU 0 waits here until every carrier has arrived */
	wait_queue_head_t arrive_wq;
	unsigned int arrived;
	bool started;			/* the first carrier has arrived */

	/* carriers */
	struct list_head carriers;
	atomic_t nr_carriers;

	/* address spaces, and their shared records */
	struct mutex spaces_lock;
	struct klet_space spaces[KLET_MAX_SPACES];
	struct klet_space_rec *space_recs;

	/* counters of the memory paths, shared with the kernelet */
	struct klet_mem_stats *mem_stats;

	/*
	 * The memory controller's owner of every tenant address space: the
	 * holder's group leader, pinned from KLET_CREATE until the sandbox's
	 * spaces are gone, and cleared from every remaining mm before it is
	 * unpinned.
	 */
	struct task_struct *owner;

	/* the kernelet-stack pool: every stack ever made, busy or free */
	struct list_head kstack_all;
	unsigned int kstack_max;	/* the pool's bound: max_tasks + nr_vcpus */
	unsigned int kstack_made;	/* the pool's size, against its bound */
	unsigned int kstack_total;	/* stacks made over the sandbox's life */
	unsigned int kstack_returned;	/* ... and given back to vmalloc */

	/*
	 * The kernelet's heap: every block mem_alloc handed out that mem_free
	 * has not taken back, by number (carrier.c, klet_do_mem_alloc). What is
	 * left in it when the sandbox ends is freed then.
	 */
	struct klet_heap {
		spinlock_t lock;
		void **block;	/* block[n]: the block numbered n, or NULL */
		u32 *free;	/* the numbers not in use, a stack */
		u32 nr_free;
		u32 size;	/* entries in block[] and free[] */
	} heap;
	atomic64_t heap_allocs;		/* blocks handed out */
	atomic64_t heap_frees;		/* ... freed by the kernelet */
	atomic64_t heap_reaped;		/* ... freed when the sandbox ended */
	atomic64_t heap_bad_frees;	/* mem_free of an address that is not a block */

	/*
	 * The end (sandbox.c, klet_end). `stopped`: the kernel proper powered
	 * off. `dying`: the sandbox has been marked dying, for whatever reason,
	 * stop included; no carrier may arrive after it. `exited`: every carrier
	 * is gone since, and `exit` is final. `released`: the last copy of the
	 * sandbox descriptor is closed. `destroyed`: everything is given back.
	 */
	bool stopped;
	bool dying;
	bool exited;
	bool released;
	bool destroyed;
	bool exit_claimed;		/* someone is running klet_sandbox_exited() */
	bool sweep_armed;
	struct klet_exit exit;
	wait_queue_head_t exit_wq;
	struct work_struct destroy_work;

	/* the sandbox's command line, from KLET_CREATE */
	char *cmdline;

	/* the console and the log, drained by out_work */
	struct klet_out console, log;
	struct work_struct out_work;
	atomic64_t log_dropped;

	atomic64_t service_calls;
	struct klet_counters n;
	atomic_t longest_defer_us;	/* the longest deferral, in microseconds */
	atomic_t longest_tick_gap_us;	/* the longest gap between two ticks of a running watch timer */
	atomic_t forced_recorded;
	struct klet_forced_rec forced_rec[KLET_FORCED_RECORDS];

	/* What one service costs, measured on the first log line (carrier.c). */
	u64 stub_cycles;
	u64 body_cycles;
	bool stub_reported;
};

/* The module's totals, of every sandbox destroyed (main.c). */
struct klet_totals {
	struct klet_counters n;
	atomic_t sweeps;
	atomic_t longest_defer_us;
	atomic_t longest_tick_gap_us;
	atomic_t forced_recorded;
	struct klet_forced_rec forced_rec[KLET_FORCED_RECORDS];
	atomic64_t heap_allocs, heap_frees, heap_reaped, heap_bad_frees;
	atomic_t kstack_total, kstack_returned;
	/* Carrier records allocated and freed: equal once every carrier is gone. */
	atomic_t carriers_made, carriers_freed;
	atomic_t sandboxes;
	struct klet_mem_stats mem;
};
extern struct klet_totals klet_totals;

extern const struct klet_services klet_services;
extern bool klet_mirror;

static inline struct klet_carrier *klet_carrier_of(struct carrier_event_hooks *hooks)
{
	return container_of(hooks, struct klet_carrier, hooks);
}

static inline struct klet_carrier *klet_current_carrier(void)
{
	return klet_carrier_of(current->carrier_hooks);
}

/*
 * What the kernelet says about itself. Believed only for "deliver the upcall now
 * or leave it pending": nothing the kernelet writes decides how much the endovisor
 * changes Linux's own state.
 */
static inline u32 klet_guards(struct klet_carrier *c)
{
	return READ_ONCE(c->vrec.preempt_info) & KLET_PREEMPT_GUARD_MASK;
}

static inline bool klet_irq_off(struct klet_carrier *c)
{
	return READ_ONCE(c->vrec.irq_off) != 0;
}

/* The kernelet is in a critical section. */
static inline bool klet_critical(struct klet_carrier *c)
{
	return klet_guards(c) != 0 || klet_irq_off(c);
}

/* Linux's preemption count currently includes the kernelet's one. */
static inline bool klet_mirrored(struct klet_carrier *c)
{
	return READ_ONCE(c->vrec.mirrored) != 0;
}

/*
 * Is this carrier running the kernelet's own code right now?
 *
 * The state field only ever says something reliable about vcpu_idle and user_run,
 * so "in the kernelet" is the conjunction of the three facts the endovisor itself
 * maintains: not in a host service, not in or on the way to user mode, not asleep.
 */
static inline bool klet_in_kernelet(struct klet_carrier *c)
{
	return READ_ONCE(c->state) == KLET_RUNNING &&
	       READ_ONCE(c->depth) == 0 && !READ_ONCE(c->idle) && !c->evicted;
}

static inline bool klet_in_grant(struct klet_inst *inst, u64 paddr, u64 len)
{
	return paddr >= inst->boot.grant_paddr &&
	       paddr + len <= inst->boot.grant_paddr + inst->boot.grant_len;
}

// Is `ip` inside this sandbox's executable image mapping?
static inline bool klet_in_image_text(struct klet_inst *inst, unsigned long ip)
{
	return ip >= inst->text_lo && ip < inst->text_hi;
}

/* ------------------------------------------------------------------ */
/* Services that run on the carrier's Linux stack                     */
/* ------------------------------------------------------------------ */

enum klet_svc_op {
	KLET_SVC_NOP,		/* nothing; with a count, the stub measurement */
	KLET_SVC_LOG_WRITE,
	KLET_SVC_MEM_ALLOC,
	KLET_SVC_MEM_FREE,
	KLET_SVC_KSTACK_ALLOC,
	KLET_SVC_KSTACK_FREE,
	KLET_SVC_VCPU_BOOT,
	KLET_SVC_VCPU_KICK,
	KLET_SVC_VCPU_IDLE,
	KLET_SVC_VCPU_YIELD,
	KLET_SVC_VCPU_ON_SPIN,
	KLET_SVC_FPU_SAVE,
	KLET_SVC_FPU_LOAD,
	KLET_SVC_STOP,
	KLET_SVC_VM_CREATE,
	KLET_SVC_VM_DESTROY,
	KLET_SVC_VM_ACTIVATE,
	KLET_SVC_VM_MAP,
	KLET_SVC_VM_UNMAP,
	KLET_SVC_VM_PROTECT,
	KLET_SVC_VM_FLUSH,
	KLET_SVC_VM_WAIT,
	KLET_SVC_VM_WAKE,
	KLET_SVC_CONSOLE_WRITE,
	KLET_SVC_VSOCK,
};

/*
 * The one-off measurement of the stub: KLET_MEASURE_ROUNDS rounds of
 * KLET_MEASURE_N empty services each, and the least round counts, because the
 * machine beneath the guest can take the processor away in the middle of a
 * round, interrupts off or not.
 */
#define KLET_MEASURE_N 2000
#define KLET_MEASURE_ROUNDS 7

struct klet_svc_args {
	u64 op;
	u64 a0, a1, a2, a3, a4;
	u64 ret;
};

/* switch.S */
void klet_switch(void **save_sp, void *new_sp);
void klet_coro_trampoline(void);
void klet_coro_main(struct klet_carrier *c);
void klet_run_on_linux_stack(void *linux_sp, struct klet_svc_args *args);
void klet_exit_stub(void);
void klet_yield_stub(void);

/* carrier.c */
void klet_svc_body(struct klet_svc_args *args);
u64 klet_vsock_new_cid(void);
long klet_vsock_socket(void);
long klet_vsock_permit(struct klet_inst *a, struct klet_inst *b);
s64 klet_vsock_service(struct klet_inst *inst, struct klet_vsock_req *req);
void klet_vsock_wake(void);
void klet_vsock_gone(struct klet_inst *inst);
void klet_vsock_report(void);
long klet_vsock_profile_report(bool reset);
void klet_evict_leave(void);
unsigned long klet_yield_body(void);
struct klet_carrier *klet_carrier_new(struct klet_inst *inst, int vcpu);
struct klet_carrier *klet_carrier_get(struct klet_carrier *c);
void klet_carrier_put(struct klet_carrier *c);
int klet_carrier_attach(struct klet_carrier *c);
void klet_carrier_lifeline_closed(struct klet_carrier *c);
void klet_carrier_die(struct klet_carrier *c);
void klet_kill_carriers(struct klet_inst *inst);
void klet_sandbox_gone(struct klet_inst *inst);
void klet_stop_instance(struct klet_inst *inst, u64 code);
void klet_virq_raise(struct klet_carrier *c, u32 bit);
extern const struct carrier_event_hooks klet_carrier_hooks;

/* main.c */
struct file *klet_lifeline_create(struct klet_carrier *c);
int klet_publish_host_offsets(struct klet_inst *inst);
u64 klet_fpu_area_size(void);
extern struct workqueue_struct *klet_wq;
extern struct mutex klet_table_lock;
extern struct klet_kind klet_kinds[KLET_MAX_KINDS];
extern struct klet_inst *klet_sandboxes[KLET_MAX_SANDBOXES];

/* image.c: the kernelet image loader */
int klet_image_load(struct klet_kind *kind, const void *buf, size_t len);
void klet_image_free(struct klet_kind *kind);
int klet_image_start(struct klet_inst *inst);
void klet_image_stop(struct klet_inst *inst);

/* sandbox.c: KLET_CREATE, the sandbox descriptor, the exec descriptors, the end */
long klet_sandbox_create(struct klet_create __user *uarg);
struct linux_binprm;
struct klet_inst *klet_exec_lookup(struct linux_binprm *bprm, unsigned int *vcpu);
int klet_exec_claim(struct klet_inst *inst, unsigned int vcpu);
void klet_exec_unclaim(struct klet_inst *inst, unsigned int vcpu);
int klet_sandbox_start(struct klet_inst *inst);
void klet_inst_get(struct klet_inst *inst);
void klet_inst_put(struct klet_inst *inst);
void klet_end(struct klet_inst *inst, u32 how, s32 code, u32 reason);
void klet_sandbox_exited(struct klet_inst *inst);
void klet_out_write(struct klet_inst *inst, struct klet_out *out, const char *ptr, u64 len);
void klet_check_exited_locked(struct klet_inst *inst, bool *claim);

/* main.c: the test hook (module parameters) */
extern bool klet_test_hooks;
extern unsigned int klet_test_sleep_ms;

/* space.c: tenant memory, a VmSpace as a Linux address space */
int klet_spaces_init(struct klet_inst *inst);
void klet_spaces_free(struct klet_inst *inst);
void klet_spaces_destroy(struct klet_inst *inst);
int klet_space_adopt(struct klet_carrier *c, u64 id);
void klet_leave_space(struct klet_carrier *c);
s64 klet_do_vm_create(struct klet_carrier *c);
s64 klet_do_vm_destroy(struct klet_carrier *c, u64 id);
s64 klet_do_vm_activate(struct klet_carrier *c, u64 id);
s64 klet_do_vm_map(struct klet_carrier *c, u64 id, u64 va, u64 paddr, u32 prot);
s64 klet_do_vm_unmap(struct klet_carrier *c, u64 id, u64 va, u64 len);
s64 klet_do_vm_protect(struct klet_carrier *c, u64 id, u64 va, u32 prot);
s64 klet_do_vm_flush(struct klet_carrier *c, u64 id, u64 va, u64 len);
s64 klet_do_vm_wait(struct klet_carrier *c, u64 id, u32 stripe, u64 expected);
s64 klet_do_vm_wake(struct klet_carrier *c, u64 id, u32 stripe);
void klet_spaces_wake_all(struct klet_inst *inst);

/* evict.c */
int klet_evict_init(void);
void klet_evict_exit(void);
void klet_evict_arm(struct klet_inst *inst);

/* watch.c */
extern bool klet_watch;
int klet_watch_init(void);
void klet_watch_exit(void);
void klet_watch_arm(struct klet_carrier *c);
bool klet_virq_redirect(struct klet_carrier *c, struct pt_regs *regs);
void klet_watch_fire_now(void);

/* the kernelet-stack pool, in carrier.c */
u64 klet_kstack_alloc(struct klet_inst *inst);
void klet_kstack_put(struct klet_inst *inst, u64 base);
unsigned int klet_kstack_pool_destroy(struct klet_inst *inst, unsigned int *busy);

/* the heap, in carrier.c */
void klet_heap_init(struct klet_inst *inst);
unsigned long klet_heap_destroy(struct klet_inst *inst, unsigned long *bytes);

/* sched.c */
int klet_sched_init(void);
void klet_sched_exit(void);
void klet_sched_attach(struct klet_carrier *c);
void klet_sched_detach(struct klet_carrier *c);
void klet_do_on_spin(struct klet_carrier *c);
void klet_kick_remote(struct klet_carrier *c);

#endif /* _KLET_ENDOVISOR_H */

# 2026-11-13 Direct page-table updates in vOSTD

- Status: Draft
- Author(s): Plucky923
- Discussion: Revised from the initial `2026-11-13` batched-services draft; batched services appear under [Alternatives and tradeoffs](#alternatives-and-tradeoffs).
- Affected chapters: [The kernelet ABI](../accepted-design/the-kernelet-abi.md), [Carrier hooks and patch overview](../accepted-design/carrier-hooks.md), [Into user mode and back](../accepted-design/into-user-mode-and-back.md), [Stopping a kernelet](../accepted-design/stopping-a-kernelet.md), [Tenant memory on Linux's page tables](../accepted-design/tenant-memory-on-linux-page-tables.md), [What vOSTD adds on top](../accepted-design/what-vostd-adds-on-top.md), and [What it costs](../accepted-design/what-it-costs.md).
- Implementation or experiments: Not yet implemented in the book prototype; baseline `fork` measurements appear below.

## Summary

The accepted design maps tenant memory under a full-range `VM_PFNMAP` VMA and crosses into Linux for `map`, `protect`, and `unmap`. Nearly all of `map`'s 447 cycles/page goes to host `mmap_lock`, VMA lookup, double walks, and tracking tables. Because the host MMU multiplexer owns only `mm_struct` containers, this proposal deletes the VMA along with `vm_map`, `vm_protect`, `vm_unmap`, `vm_wait`, and `vm_wake`. `vOSTD` allocates lower page tables from sandbox grants and updates user PTEs directly in memory, while Linux retains one VMA-free `mm_struct` per `VmSpace` for `CR3` scheduling, kernel-half sync, and `vm_flush` TLB shootdowns. At trap entry, `on_exception` captures user faults and traps, and `on_resume` delivers them to the kernelet without VMA callbacks or POSIX signals.

## Problem and motivation

### Who manages tenant page tables

Each carrier runs as a Linux task. Its page tables must work with Linux scheduling, confine mappings to the sandbox grant, and update fast enough for workloads such as `fork`.

To reuse Linux's PTE helpers without handing physical-page accounting to Linux, the accepted design ([Tenant memory on Linux's page tables](../accepted-design/tenant-memory-on-linux-page-tables.md#the-problem)) gives each `VmSpace` one `mm_struct` and one full-range `VM_PFNMAP` VMA. That VMA indirection burdens two paths:

- **PTE updates (`map`, `protect`).** Each single-page update crosses into the endovisor. The endovisor takes `mmap_read_lock` and looks up the VMA, forcing `vOSTD` to handle cursor contention with sleeping `vm_wait` and `vm_wake` shims. Linux walks the page table to write the PTE, and the endovisor walks it a second time to verify the slot because `vmf_insert_pfn_prot()` returns success even when an existing entry skips the insert.
- **Page-fault delivery (`#PF`).** User page faults traverse VMA lookup, `handle_mm_fault()`, and the VMA fault callback before `on_resume`, while other user traps fall back to Linux signals. Deleting the VMA removes the `#PF` callback, so a direct trap hook replaces both paths.

### What that choice costs

We measured five-boot medians on one KVM VM (2 vCPUs, 512 MiB RAM, Linux 6.12, 3.19 GHz Ryzen 7 5800H). Native Linux forks 64 MiB of 4 KiB private anonymous pages (`MADV_NOHUGEPAGE`, 16,384 distinct pages) in 0.34 ms (0.337 ms, ~21 ns or ~66 cycles/page), including task, VMA, and PTE setup. Linux walks each 2 MiB leaf table once, updates PTEs in batches, and flushes the TLB once.

In the same VM, the baseline kernelet benchmark measures only `VmSpace` creation and copy-on-write PTE replication across 16,384 entries (`protect` and `map` per page over 256 frames mapped 64 times due to the small baseline grant), excluding task creation. Even with that narrower scope and warmer frame reuse, the kernelet takes 7.85 ms (25.07 M cycles, ~479 ns or ~1,530 cycles/page), 23x slower than Linux's full `fork()`.

| Kernelet operation (3.19 GHz VM) | Median | Median per page |
|---|---:|---:|
| `map`, one page per call | 447 cycles (140 ns) | 447 cycles (140 ns) |
| `map`, 512 pages under one cursor | n/a | 472 cycles (148 ns) |
| `protect` 2 MiB read-only + flush (512 pages) | 243,808 cycles (76.3 µs) | ~476 cycles |
| `munmap` 64 MiB in one range call (16,384 pages) | 872,480 cycles (0.27 ms) | ~53 cycles |

One `CursorMut` across 512 pages still costs 472 cycles/page, and a single range `munmap` drops to ~53 cycles/page. Because `mem-perf` brackets loops with unserialized `rdtsc` calls and retains cold initial rounds, short single-call rows carry timer noise. Still, a null crossing takes only 36 cycles on the 3.79 GHz host in [What it costs](../accepted-design/what-it-costs.md#the-crossing), so nearly all of `map`'s 447 cycles/page and the 23x `fork` gap come from host VMA locks, double walks, and tracking tables.

## Proposed change

### Why Linux need not maintain user page tables

Linux neither sets tenant paging policy nor audits individual PTE writes. The kernel proper runs demand paging and copy-on-write, and trusted `vOSTD` confines every mapping to sandbox grants. Because the x86-64 MMU walks `CR3` without consulting Linux VMAs, and grant-backed page tables allow whole-sandbox reclamation on kill, Linux only needs to manage each address space's `mm_struct` shell.

### Overview

Four layers split page-table management:

- **Kernel proper (unchanged).** Uses OSTD `VmSpace`, `CursorMut`, and `TlbFlusher` APIs and handles user faults as on bare metal.
- **`vOSTD`.** Allocates lower page-table pages from sandbox grants and writes user PTEs and root user slots directly in memory. It keeps only `vm_create`, `vm_activate`, `vm_flush`, and `vm_destroy`.
- **Endovisor.** Manages VMA-free `mm_struct` shells across `vm_create`, `vm_activate`, `vm_flush`, and `vm_destroy`, and forwards user traps to `on_resume`.
- **Linux and carrier hooks.** Owns `mm->pgd` allocation, kernel-half sync, `CR3` scheduling, cross-CPU TLB shootdowns, and `on_exception` trap capture.

| Kernel proper call or event | Accepted design | Proposed design |
|---|---|---|
| `VmSpace::new()` | `vm_create` builds `mm`, file, and `VM_PFNMAP` VMA | `vm_create` allocates VMA-free `mm_struct` and exposes its root PFN |
| `vm_space.activate()` | `vm_activate` records space and switches before user entry | `vm_activate` pins `VmSpace` and switches `current->mm` directly |
| `cursor.map(frame, prop)` | `vm_map` crossing (or withheld map if `!R`) | `vOSTD` allocates grant PT pages and writes PTE locally (including `!R`) |
| `cursor.protect_next(len, op)` | `vm_protect` crossing (or `vm_unmap`/`vm_map` if `!R`) | `vOSTD` updates PTE atomically in memory (caller flushes via `TlbFlusher`) |
| `cursor.unmap(len)` | Walk + `vm_unmap` crossing + RCU wait | `vOSTD` clears PTEs locally, calls `vm_flush`, waits RCU, and drops frames |
| `flusher.dispatch_tlb_flush()` | `vm_flush` (VMA range flush) | `vm_flush` (`mm_struct` range flush) |
| `VmSpace` drop | Walk + `vm_unmap` + `vm_destroy` | Detaches root user slots, runs RCU wait + `vm_destroy`, frees subtree locally |
| Tenant `#PF` / user trap | VMA fault callback or POSIX signal drain | `on_exception` captures user traps, and `on_resume` delivers them |

### VMA-free mm_struct and root page

On x86-64, one 4-level root (`mm->pgd`) maps both halves: shared Linux kernel slots `256..511` and tenant slots `0..255`. Deleting the VMA removes Linux's user-range tracking, but each `VmSpace` retains one VMA-free `mm_struct` so Linux can:

1. **Own and sync the root page.** Allocate `mm->pgd` and propagate host kernel updates in `256..511`.
2. **Restore `CR3` on schedule.** Reload `mm->pgd` across host preemptions and CPU migrations.
3. **Track CPUs and PCIDs.** Maintain `mm_cpumask` and PCID generations for TLB shootdowns.
4. **Switch tenant spaces.** Swap `current->mm` when a carrier changes tenant threads.

Previously, `vm_create` called `vm_mmap()`, which required a backing file and switching `current->mm` twice to attach the VMA. Without VMAs, `vm_create` allocates only a VMA-free `mm_struct` and publishes its root physical address (`root_paddr`) in the shared space record. Because slots `256..511` carry Linux's direct map (`linear_map_base`), `vOSTD` reads and edits any `VmSpace` at `linear_map_base + paddr` without switching `CR3`.

Running tenant code still loads the target `mm->pgd` into `CR3`. Previously, `vm_activate` deferred switching `current->mm` until user-mode entry because synchronous `mmput()` can sleep if the old `mm` hits zero references while `VmSpace::activate()` runs in a non-sleepable context. Now `vm_activate` switches `current->mm` immediately and drops the previous `mm` via non-sleeping `mmput_async()`. Because that `mm` has no VMAs, `mmput_async()` tears down only the Linux `mm_struct` shell, leaving `vOSTD` to reclaim the grant-backed lower page tables on `VmSpace` drop.

### Direct page-table writes in vOSTD

`vOSTD` runs OSTD's page-table logic (`CursorMut::map`, `protect_next`, and `unmap`) directly in memory. Lower tables (levels 1–3) use grant frames, [`MetaSlot`](2026-11-16-allocate-the-kernelet-heap-from-the-grant.md#use-ostds-frame-model-within-the-grant) node locks (`PageTablePageMeta`, excluded from `AnyUFrameMeta`), and hand-over-hand locking (`vcpu::on_spin()` on contention). Concurrent reads (`VmReader`, `VmWriter`, `query`) and writes use aligned `AtomicU64` accesses, with `Release` stores publishing zeroed child tables before readers traverse them. Extra rules handle Linux's `mm->pgd` root, lockless host walkers, and x86 PTE bits.

<div class="background">

**Linux: page-table isolation (PTI).** To mitigate Meltdown, PTI removes full kernel mappings from user mode's `CR3`. Under `X86_FEATURE_PTI`, Linux allocates two adjacent 4 KiB pages per `mm->pgd` on an 8 KiB boundary: the kernel root (`root_paddr`) and the user root (`root_paddr | PAGE_SIZE`), whose upper half maps only minimal entry structures. Both roots share the same user subtree across slots `0..255`. Linux loads `root_paddr` on kernel entry and `root_paddr | PAGE_SIZE` on user return, and [`__pti_set_user_pgtbl()`](https://elixir.bootlin.com/linux/v6.12/source/arch/x86/mm/pti.c#L124) sets `_PAGE_NX` on present user entries in the kernel root.

</div>

Because Linux allocates `mm->pgd` outside the grant `MetaSlot` array, `VmSpace` keeps a dedicated lock for level 4 and maintains both roots when `pti_enabled` holds. At startup, `vOSTD` checks `BootArgs` for 4-level paging (`pgtable_levels == 4`), `_PAGE_NX` support, and unencrypted RAM (`sme_me_mask == 0`). Under the root lock, `vOSTD` zeroes new child tables before publishing them; with PTI enabled, it writes `root_paddr | PAGE_SIZE` first and then `Release`-stores `root_paddr` with `_PAGE_NX` on present user slots.

An empty VMA tree stops VMA-based Linux scanners (`pagemap`, MGLRU, `exit_mmap()`). Two lockless Linux paths still walk `mm->pgd` on `current->mm`:

<div class="background">

**Linux: lockless page-table walkers and non-present PTE bits.** `perf_virt_to_phys()` walks `mm->pgd` in NMI context via `get_user_page_fast_only()`. `gup_fast_pgd_range()` (`gup_fast` in `mm/gup.c`) walks page tables with interrupts disabled before checking VMAs. `gup_fast_pte_range()` rejects an entry when `pte_special()` (`_PAGE_SPECIAL`), `pte_protnone()`, devmap, or permission checks fail; otherwise it pins the frame's `struct page`. On x86-64, Linux reuses bit 8 (`_PAGE_GLOBAL`) on non-present PTEs as `_PAGE_PROTNONE` and defines `pte_present()` as `_PAGE_PRESENT | _PAGE_PROTNONE` (`arch/x86/include/asm/pgtable_types.h`).

</div>

`on_syscall` blocks carrier syscalls such as `O_DIRECT` from calling `gup_fast`, while host `perf` still samples carriers in NMI context. To protect lockless walkers from reading freed tables without running `synchronize_rcu()` on every unmap, `CursorMut::unmap` clears leaf PTEs and leaves lower tables (levels 1–3) attached until `VmSpace` destruction.

Before writing a leaf PTE, `vOSTD` checks a typed `UFrame`, alignment, user-range bounds, and multi-segment `in_grant(paddr)` ([2026-11-16](2026-11-16-allocate-the-kernelet-heap-from-the-grant.md)), then enforces four PTE bit rules:

- **Writeback PAT policy.** `vOSTD` restricts mappings to `CachePolicy::Writeback` (`PWT = PCD = PAT = 0`), matching Linux's direct map without touching `IA32_PAT`.
- **Special and global bits.** Mapped leaves set `_PAGE_SPECIAL` (bit 9) so lockless `perf`/`gup_fast` paths skip `struct page` lookups, and clear `_PAGE_GLOBAL` (bit 8) to avoid cross-PCID aliasing and `_PAGE_PROTNONE` confusion.
- **Saved-dirty write protection.** Because x86 CET reserves `WRITE = 0` with `DIRTY = 1` for shadow stacks (`arch/x86/include/asm/pgtable.h`), write-protecting a leaf moves `_PAGE_DIRTY` (bit 6) to `_PAGE_SAVED_DIRTY` (bit 58), and `PageFlags::DIRTY` reads both.
- **L1TF-safe `!R` leaves.** Non-readable (`!PageFlags::R`) entries replace the accepted `withheld` set by clearing `PRESENT` and `_PAGE_GLOBAL`, setting `SOFTW2` (bit 10), and inverting PFN bits within `physical_mask` (`arch/x86/include/asm/pgtable-invert.h`) to block L1TF speculation. The kernel proper resolves `SEGV_ACCERR` versus `SEGV_MAPERR` from its VMAR tree.

### TLB flushing and space teardown

`vOSTD` sees only virtual CPUs and edits page tables without changing `CR3`. Only Linux tracks physical CPUs in `mm_cpumask`, sends cross-CPU IPIs, and runs host RCU:

- **Runtime TLB flushes (`vm_flush`).** Local `invlpg` misses remote physical CPUs in `mm_cpumask`, inactive spaces edited via the direct map, and PTI user PCIDs. Callers must dispatch `TlbFlusher` (`vm_flush` calling `flush_tlb_mm_range()`) before relying on tighter permissions (`protect_next`) or reusing unmapped frames.
- **Space teardown (`vm_destroy`).** Without VMAs, `exit_mmap()` skips user tables and table-freeing TLB flushes. Reclaiming grant page tables requires evicting CPUs holding stale `CR3` roots and draining host RCU readers. On `KLET_KILL`, kernelet cleanup never runs, so the endovisor must sever those host references itself.

<div class="background">

**Linux: lazy TLB mode.** On kernel threads, x86 Linux leaves the last user `mm` in `CR3` (`arch/x86/mm/tlb.c`). Page shootdowns skip lazy CPUs until they exit lazy mode. Freeing page-table pages requires `flush_tlb_mm_range()` with `freed_tables = true`: CPUs cache upper-level paging structures, walk `CR3` speculatively, and must switch lazy CPUs back to `init_mm`.

</div>

Runtime `VmSpace` retirement and sandbox exit or kill run four host teardown steps in order:

1. Wait for active `vm_activate` and `vm_flush` calls on the space to finish, and detach referencing vCPUs.
2. Zero user entries `0..255` in `mm->pgd` (and `root_paddr | PAGE_SIZE` under PTI) without reading lower tables.
3. Run `flush_tlb_mm_range(mm, 0, TLB_FLUSH_ALL, 0, true)` to clear paging-structure caches and switch lazy-TLB CPUs to `init_mm`.
4. Run host `synchronize_rcu()` to drain lockless `perf` and `gup_fast` walkers, and call `mmput(mm)`.

On runtime `VmSpace` retirement, `vOSTD` detaches user PGD slots `0..255`, drains its own RCU readers (`rcu::synchronize()`) and runs `vm_destroy` (steps 1–4), and then drops the detached lower tables and `UFrame`s (decoding `SOFTW2` inverted PFNs) back into the grant. On sandbox exit or `KLET_KILL`, the endovisor runs steps 1–4 on each space without touching lower tables and reclaims whole grant segments once active I/O leases ([TypedIO](2026-11-08-typed-io.md)) drain.

### User-mode exception hook

Deleting the `VM_PFNMAP` VMA removes Linux's `.fault()` and `.pfn_mkwrite()` callbacks, which would drop user `#PF` events into POSIX `SIGSEGV` signals. Extending `carrier_event_hooks` with `on_exception(regs, trapnr, error_code, addr)` lets Linux forward user traps directly before VMA lookup or `force_sig()` signal queuing.

Because `on_exception` runs in non-sleepable trap-entry contexts (such as `#DB`'s IST stack), it splits capture from delivery. At trap entry, `on_exception` records `(trapnr, error_code, addr)` on the carrier, sets `TIF_NOTIFY_RESUME`, and returns `true`. On the sleepable `exit_to_user_mode_loop()` path, `on_resume` delivers the captured trap to the kernelet (`UserEvent::Exception` under `UserEventHandler`, or `KLET_RUN_EXCEPTION` in the accepted stack model).

Three constraints bound the hook:

- **User traps only.** `on_exception` claims faults only when `user_mode(regs)` and `KLET_USER` hold; kernel-mode faults still fail via Linux's exception table (`-EFAULT`).
- **Host checks and vector split.** Linux runs per-vector sanitization first (such as `#PF` error-code sanitization), retains host and hardware vectors (`#DF`, `#MC`, lazy-FPU `#NM`, fatal `#BR`), and forwards the 11 Asterinas-handled user traps directly instead of routing them through `force_sig()` and `klet_drain_signals()`.
- **No host VMAs or notifiers.** Because `on_syscall` blocks host memory system calls, no carrier can attach a VMA or `mmu_notifier` to a tenant `mm`.

## Alternatives and tradeoffs

| Alternative | Benefits | Costs and limits |
|---|---|---|
| **Keep per-page Linux calls** | Avoids PTI root sync and x86 PTE encoding in `vOSTD` | Baseline `VmSpace` `fork` takes 7.85 ms (excluding task creation), 23x slower than native Linux's full `fork()` |
| **Batch Linux PTE calls** | Cuts crossing counts on large contiguous ranges | Single-page `#PF` and small ranges still cross into Linux, take `mmap_lock`, and run double walks |

## Implementation and integration

- **Linux carrier patch** (`prototype/linux/patches/0001-carrier-hooks.patch`). Add `on_exception` to `carrier_event_hooks` for user `#PF` and forwarded user exceptions.
- **Endovisor** (`prototype/linux/endovisor/`). Validate startup paging constraints, export `pti_enabled` and `physical_mask` in `BootArgs`, remove per-page `vm_*` services and `VM_PFNMAP` setup, and update `vm_activate`, 4-step `vm_destroy`, and `on_exception` capture.
- **`vOSTD`** (`prototype/vostd/src/mm/`, `arch/`). Use the shared `MetaSlot` and `in_grant()` model ([2026-11-16](2026-11-16-allocate-the-kernelet-heap-from-the-grant.md)) for lower page tables, add root locking, PTI dual-root writes, and x86 PTE rules, reclaim subtrees locally on `unmap` and `VmSpace` drop, and handle `UserEvent::Exception`.
- **Book.** On acceptance, update the seven Accepted Design chapters listed above. Any future grant-reclaim design must keep installed page-table pages pinned until `vm_destroy` finishes.

## Decision and follow-up

Pending prototype implementation, benchmark results, and review.

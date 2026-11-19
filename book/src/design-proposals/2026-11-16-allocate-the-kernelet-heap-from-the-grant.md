# 2026-11-16 Allocate the kernelet heap from the grant

- Status: Draft
- Author(s): Plucky923
- Discussion: No public review opened yet.
- Affected chapters: [The first kernel we will run](../accepted-design/the-first-kernel-we-will-run.md), [The kernelet ABI](../accepted-design/the-kernelet-abi.md), [Carriers](../accepted-design/carriers.md), [Stopping a kernelet](../accepted-design/stopping-a-kernelet.md), [What vOSTD adds on top](../accepted-design/what-vostd-adds-on-top.md), and [What it costs](../accepted-design/what-it-costs.md).
- Implementation or experiments: Not yet implemented. The argument below comes from source inspection; no local-heap performance or fault-injection results are available.

## Summary

This proposal removes `mem_alloc`, `mem_free`, and the endovisor's per-block heap tracking table. Linux provisions multi-segment startup grants via `BootArgs`, accounts for them, and reclaims them whole after execution, host accesses, and descriptor references end. Inside those grants, `vOSTD` uses OSTD's `Frame`, `MetaSlot`, and slab model to implement Rust's `#[global_allocator]` (`Box`, `Vec`, `Arc`) locally, eliminating host crossings (449 cycles per 64-byte pair) and sleeping `kmalloc` calls under kernelet spin locks.

## Problem and motivation

### Host page ownership vs. kernelet objects

A kernelet needs physical pages for user spaces (`UFrame`), its Rust heap (`Box`, `Vec`, `Arc`), and per-page metadata. On the host side, Linux must meter every backing page and reclaim them whole after sandbox exit and final descriptor close ([Stopping a kernelet](../accepted-design/stopping-a-kernelet.md#the-shape-of-a-death)). Forwarding fine-grained Rust allocations across the host boundary solves reclamation only by paying a per-object tax.

### How the accepted host heap works and what it costs

Today, `FrameAllocOptions` draws user pages from a [grant](../accepted-design/the-kernelet-abi.md#the-grant-and-the-direct-map) capped at 4 MiB as one contiguous Linux allocation. Because 4 MiB is too tight to hold both user pages and kernel objects, `HostHeap` (`prototype/vostd/src/mm/heap.rs`) forwards Rust allocations to `mem_alloc` and `mem_free`.

Forwarding every Rust object to Linux adds three costs:

1. **Per-object crossings and lock contention.** Each `alloc` or `free` switches to the Linux stack and takes `inst->heap.lock` to update the host block table (`inst->heap.block`). Benchmarks in [What it costs](../accepted-design/what-it-costs.md#the-crossing) report 449 cycles for one 64-byte `mem_alloc`/`mem_free` pair, including ~120 cycles for that block table.
2. **Sleeping allocations under spin locks.** `mem_alloc` calls `kmalloc(GFP_KERNEL_ACCOUNT)`, which may sleep. Calling `Box` or `Vec` under a kernelet spin lock breaks the non-sleeping rule under locks and can stall sibling vCPUs ([What vOSTD adds on top](../accepted-design/what-vostd-adds-on-top.md#the-rule-about-sleeping)).
3. **Per-block padding and teardown scans.** The endovisor pads every block by alignment (`size + align`) and maintains a dynamically grown `inst->heap.block` table, which `klet_heap_destroy` must scan to `kfree` leaked blocks at teardown.

## Proposed change

Linux continues to provision, meter, and whole-reclaim the grant, while `vOSTD` subdivides grant pages into kernel objects, metadata, and user frames locally.

### Use OSTD's frame model within the grant

Sharing the grant between kernel objects and user memory requires tracking each page's role so kernel pages never escape to user space. Adopting OSTD's `Frame`, `UniqueFrame`, `MetaSlot`, and buddy allocator provides type safety, refcounts, coalescing, and slab metadata:

<div class="background">

**OSTD: direct-map frames and slabs.** OSTD maps physical RAM once at boot via a fixed offset (`paddr_to_vaddr`) and never remaps kernel allocations. OSTD's [`GlobalFrameAllocator`](https://github.com/asterinas/asterinas/blob/ab9a4cfdc726263b3f41ccea0337633e24b443fc/ostd/src/mm/frame/allocator.rs#L130-L175) trait fronts the page allocator, while each page has a 64-byte [`MetaSlot`](https://github.com/asterinas/asterinas/blob/ab9a4cfdc726263b3f41ccea0337633e24b443fc/ostd/src/mm/frame/meta.rs#L75-L118) storing `M`, its refcount, vtable, and list ID (`in_list`). [`Frame<M>`](https://github.com/asterinas/asterinas/blob/ab9a4cfdc726263b3f41ccea0337633e24b443fc/ostd/src/mm/frame/mod.rs#L81-L121) shares read access to `M`, and [`UniqueFrame<M>`](https://github.com/asterinas/asterinas/blob/ab9a4cfdc726263b3f41ccea0337633e24b443fc/ostd/src/mm/frame/unique.rs#L18-L54) holds exclusive write ownership. OSTD's [`Slab`](https://github.com/asterinas/asterinas/blob/ab9a4cfdc726263b3f41ccea0337633e24b443fc/ostd/src/mm/heap/slab.rs#L23-L52) slices a `UniqueFrame` via `paddr_to_vaddr`, keeping free links in unused slots and `SlabMeta` in `MetaSlot`; only `UFrame` mappings edit user page tables.

</div>

1. **Supply startup grant segments.** Pass one or more startup `alloc_pages` segments via `BootArgs` to lift the 4 MiB single-allocation cap without runtime resizing.
2. **Reserve grant-resident `MetaSlot` arrays.** Carve pinned `MetaSlot` pages (`MetaPageMeta`, 64 bytes per page, ~1.56%) from each segment's start before initializing allocators.
3. **Map bidirectionally across non-contiguous segments.** Because multi-segment grants have physical address gaps, `frame_to_meta` and `meta_to_frame` check alignment and segment bounds without overflow in both directions.
4. **Manage free pages with OSDK's buddy allocator.** Store buddy links (`Link<FreeHeadMeta>`) inside unused `MetaSlot`s, and drain per-vCPU caches and empty slabs before returning `Error::NoMemory`.

When the last reference drops during execution, `vOSTD` runs `M`'s `on_drop` and Rust destructor before returning the page to the buddy pool. On sandbox failure or exit, Linux reclaims all grant segments whole without invoking `on_drop`.

### Connect Rust allocation to the local heap

Replace `HostHeap` (`prototype/vostd/src/mm/heap.rs`) with OSTD's `AllocDispatch` (`#[global_allocator]`) backed by OSDK's `HeapAllocator` implementing `GlobalHeapAllocator`. Objects up to 2,048 bytes with natural alignment allocate from single-page `Slab` classes (`Link<SlabMeta<SLOT_SIZE>>`). Requests above 2,048 bytes or requiring larger alignment route to aligned `LargeAllocFrameMeta` `HeapSlot` segments from the buddy pool. Address conversions (`paddr_to_vaddr` and `HeapSlot::paddr`) read Linux's runtime `linear_map_base` directly. Exhaustion returns a null pointer to `GlobalAlloc` and enters Rust's allocation-error handler without calling `kmalloc`.

### Keep heap pages private to the kernel

Live kernel pages hold Rust objects or metadata and must stay hidden from untrusted code and user space:

- **Typed-frame and `VmIo` boundary.** `SlabMeta`, `LargeAllocFrameMeta`, `PageTablePageMeta`, and `MetaPageMeta` do not implement `AnyUFrameMeta`, and `vOSTD` restricts `VmIo` byte access and `is_untyped()` casts to `AnyUFrameMeta`.
- **Mandatory zeroing.** Keep `FrameAllocOptions::zeroed(false)` internal to `vOSTD` (`pub(crate)`), so every untyped frame exposed to `kernel proper` or user space starts zeroed.
- **Trusted allocator code.** Allocator code stays inside trusted `vOSTD` under the [language-based isolation model](../accepted-design/why-a-kernel-of-your-own.md#the-same-kernel-a-different-framework), leaving no hardware boundary inside the grant.

### Initialize the heap without depending on itself

Bootstrap must break circular dependencies between metadata, page allocation, and the heap:

1. **Strict startup order.** Initialize `MetaSlot` storage, the buddy allocator, and the local heap before secondary vCPUs or heap consumers (`cmdline` parsing and `__ostd_main`).
2. **Allocation-free teardown, panic, and OOM paths.** Keep `panic_handler`, `alloc_error_handler`, `CursorMut::unmap`, and `VmSpace` retirement allocation-free so OOM or teardown cannot recurse into the heap.
3. **Interrupt-safe locking.** Guard allocators with `SpinLock<..., LocalIrqDisabled>` so allocators make no `mem_alloc` crossings, form no lock-order cycles, and call `vcpu_on_spin` only on extended contention.

### Reclaim the grant after a failure

When a sandbox exits or fails, Linux reclaims grant segments from its own host-side segment array without reading grant metadata:

1. **Stop execution.** Halt all carriers and block further sandbox entry so no kernelet or user-context access remains active.
2. **Drain host references.** Finish in-flight host calls, active I/O leases ([TypedIO](2026-11-08-typed-io.md)), and address-space, TLB, and RCU teardown ([Stopping a kernelet](../accepted-design/stopping-a-kernelet.md#the-shape-of-a-death)).
3. **Free all grant segments.** Release every recorded `(struct page *, order)` segment on startup rollback or after exit and final sandbox descriptor close.

### Account for the shared capacity

Linux already charges grant segments via `GFP_KERNEL_ACCOUNT` (`prototype/linux/endovisor/sandbox.c`), so subdividing them into Rust objects adds no host charge. Heap objects, `UFrame`s, lower page tables, `MetaSlot` pages, and OSDK caches share this fixed startup budget (contiguous runs stay bounded by one segment). Sizing `memory_initial` above the prototype's 4 MiB single-block cap via multiple startup segments absorbs those caches without runtime resizing. Linux separately charges per-instance private image pages (`image.c`), address spaces, and, in the current prototype, interim host stack pools, while shared read-only kind pages belong to kind registration.

## Alternatives and tradeoffs

| Alternative | Benefits | Costs and limits |
|---|---|---|
| **Keep the host heap** | Reuses `kmalloc`/`kfree` outside the grant | Keeps 449-cycle alloc/free pairs, `inst->heap.block`, and sleeping calls under spin locks |
| **Carve a fixed heap sub-region in the grant** | Separates kernel heap and `UFrame` quotas statically without sharing one buddy pool | Strands idle pages on one side when either the heap or user space fills |

## Implementation and integration

- **`vOSTD`** (`prototype/vostd/src/mm/`, `lib.rs`, `panic.rs`). Replace `HostHeap` with OSTD's grant-backed `MetaSlot`, typed frame isolation (`AnyUFrameMeta`, `VmIo`, `pub(crate)` zeroing), buddy frame allocator, and local slab/large-block heap using runtime `linear_map_base`, keeping bootstrap, panic, OOM, and teardown allocation-free.
- **Endovisor and ABI** (`prototype/vostd/src/abi.rs`, `prototype/linux/endovisor/`). Deliver multi-segment startup grants via `BootArgs`, remove `mem_alloc`, `mem_free`, and `inst->heap.block`, and reclaim segments whole on failure or teardown.
- **Book and proposals.** This frame and `MetaSlot` layer is the shared foundation for both local heap allocation and [direct page-table updates in vOSTD](2026-11-13-direct-page-table-updates-in-vostd.md). Update the six Accepted Design chapters listed above on acceptance.

## Decision and follow-up

Status remains Draft pending implementation and benchmark evidence.

# 2026-11-09 FPU state management

- Status: Draft
- Author(s): Plucky923
- Discussion: [PR #7](https://github.com/tatetian/kernelet/pull/7).
- Affected chapters: [Tasks and virtual CPUs](../accepted-design/tasks-and-virtual-cpus.md), [The kernelet ABI](../accepted-design/the-kernelet-abi.md), [Into user mode and back](../accepted-design/into-user-mode-and-back.md), [Virtual interrupts](../accepted-design/virtual-interrupts.md), and [What it costs](../accepted-design/what-it-costs.md).
- Implementation or experiments: [Direct-execution measurements](#validation-and-evidence) are available. Checked import, main-prototype integration, and complete validation remain pending.

## Summary

Let vOSTD save and restore tenant FPU state directly, validate imported state, and skip transfers on eligible same-tenant syscalls. Linux retains carrier preservation and fault handling. Experiments cover direct execution; checked import and full protocol validation remain pending.

## Problem and motivation

<div class="background">

**x86: FPU state.** FPU state includes floating-point and vector registers and their control fields. x87 holds floating-point values; SSE, AVX, and AVX-512 provide overlapping vector registers. MXCSR controls SSE and AVX operations. `XSAVE` saves extended processor state to memory; `XRSTOR` restores it. Invalid format or control fields can cause a general-protection exception (`#GP`) during restoration. See [Intel's restore rules](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf#page=336).

</div>

**Native Linux baseline.** [Linux v6.12](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/fpu/sched.h) saves live FPU state when switching a task out and restores it as needed before returning to user mode. A syscall needs neither transfer if no task switch, kernel FPU use, or explicit FPU-state operation occurs.

**Check external FPU state when importing it.** Signal return and `ptrace` writes can supply user-controlled FPU state. Ordinary syscalls need no FPU content checks. Internally saved or validated state needs no repeated checks while it remains protected from external writes. Invalid MXCSR bits can make `XRSTOR` raise `#GP` in kernel mode. Asterinas #3920, item 128, reports this failure: [x86 rt_sigreturn accepts an invalid MXCSR image and can abort the kernel during FPU restore](https://github.com/asterinas/asterinas/issues/3920#issuecomment-5848180215). Even after software validation, `XRSTOR` can raise `#GP` if checks miss a condition or a writable alias permits later changes. The kernel therefore needs a recovery path for `#GP` at FPU restore instructions to prevent a restore fault from aborting the kernel.

**The PoC checks each FPU load in the endovisor, inside the Linux kernel.** The load service copies the tenant image into a staging page, checks its header, applies a fixed MXCSR mask, and restores that copy. The private page prevents changes to the checked bytes before restoration. On a restore fault, the PoC only skips the failed instruction; Linux's [FPU restore fixup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/mm/extable.c#L112) resets the registers. The PoC neither resets FPU state nor reports failure, so the caller can continue with partially restored state. Each save/load pair adds two service calls, two buffer copies, two page clears, and one image validation.

Earlier measurements show that per-call FPU saves and loads add about **330–340 ns per syscall** over conditional `CpuSync`. The internal A→B→A workload costs about **410–580 ns more per round trip** than Linux thread switching. This second gap includes kernelet scheduling and handoff costs as well as FPU handling.

## Proposed change

### Design overview

**Validate imported FPU state in vOSTD.** A checked import interface reads external state into a private `FpuContext` and validates that allocation. vOSTD controls later writes and uses the same buffer for direct saves and restores. This removes endovisor FPU services, staging copies, and repeated content checks. Linux must restore carrier state before vOSTD resumes after scheduling or kernel FPU borrowing. On an FPU instruction fault, the Host cleans up carrier state and terminates the entire instance.

**vOSTD saves and restores tenant FPU state directly.** It validates imported state and controls later writes, so `XSAVE` and `XRSTOR` can use tenant buffers without an endovisor staging page. Let tenants A and B [share Linux task C, their carrier](../accepted-design/tasks-and-virtual-cpus.md#one-carrier-many-threads). Each stays on C and owns a `FpuContext`; Linux keeps C's separate save area.


### Task A makes a syscall and resumes

When tenant task A enters a syscall, carrier C still preserves task A's FPU state, so task A remains the **FPU owner**. If no other tenant runs and no FPU-state operation is requested, resume task A without saving, restoring, or validating a tenant image. Kernelet code must leave FPU registers unchanged.

### vOSTD switches from task A to task B on carrier C

When task A changes an FPU register, its saved copy in `A.FpuContext` does not change. With task A as the FPU owner, vOSTD saves its latest state before loading task B's state:

```text
Save task A: CPU registers -> A.FpuContext    XSAVE
Load task B: B.FpuContext -> CPU registers    XRSTOR
```

Keep tenant switching disabled through the save, load, and owner update. After task B's state loads successfully, record task B as the owner and let it continue. With no owner, skip the save but still load task B's context, using its initialized state on first entry. Both instructions access tenant buffers directly, without an endovisor FPU service or staging copy.

### Linux deschedules carrier C or uses the FPU for kernel work

Linux saves carrier C's live FPU registers to C's Linux save area when needed. If task A is the owner, this preserves task A's state without updating `A.FpuContext`.

<div class="background">

**Linux: temporary FPU use.** Linux kernel code can use FPU registers for calculations such as [encryption](https://github.com/torvalds/linux/blob/v6.12/arch/x86/crypto/aesni-intel_glue.c#L146). If those registers hold task A's state, Linux [saves it when needed](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/core.c#L421) in carrier C's Linux save area. Kernel code then overwrites the registers for its own work. This can happen without descheduling carrier C.

**Linux: deferred restoration.** After kernel FPU use or scheduling, Linux can continue running kernel code without first restoring task A's registers. Task A's state remains in carrier C's Linux save area. `TIF_NEED_FPU_LOAD` tells Linux to check and restore the registers as needed before returning to user mode.

</div>

vOSTD can switch from task A to task B without first returning task A to user mode. It must save task A's latest FPU state before loading task B. Waiting until Linux returns to user mode would therefore be too late. Direct register access while Linux restoration remains pending can cause two errors:

1. **Wrong contents saved for task A.** The registers may still hold another Linux task's state or values from kernel FPU use. A direct `XSAVE` would save those contents into `A.FpuContext` as task A's state.
2. **Task B's restored state overwritten.** vOSTD loads task B while Linux still has a pending restore for carrier C. Linux can then overwrite task B's registers with task A's state from C's Linux save area.

Before carrier C first enters vOSTD, restore its FPU state if needed. Update Linux's records to show that the CPU's FPU registers now hold carrier C's state. After carrier scheduling or Linux kernel FPU use, run Linux's [restore logic](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/context.h#L54) before vOSTD resumes. Restore carrier C's state when needed, update CPU ownership, and clear the pending-load flag. With this guarantee, ordinary syscall entry and service return need no additional `fpregs_lock()` section to check `TIF_NEED_FPU_LOAD`.

Staging removal leaves carrier recovery unchanged. If task A's latest state is only in C's Linux save area, Linux must restore it before vOSTD saves `A.FpuContext`. This costs one Linux restore in addition to saving task A and loading task B. Earlier restoration can also add work if carrier C is descheduled again before tenant execution or a vOSTD FPU operation. Linux restores C's registers on resume, then saves them again on the next deschedule. Deferred restoration could leave the state in memory throughout that interval, avoiding both transfers.

### Signal return or ptrace supplies new FPU state

**OSTD and vOSTD need the same checked FPU import interface.** Read external state directly into a private candidate `FpuContext`. Check its layout, permitted components, reserved fields, and MXCSR bits; reject invalid input before loading or installing it. In vOSTD, exclude tenant switches while checking ownership, loading, and installing the candidate. If the target is the FPU owner, load the candidate before installing it; otherwise, load it when that tenant is next selected. Install the accepted allocation itself, without an endovisor staging copy.

Initialize context storage once. vOSTD guarantees capacity, alignment, lifetime, and exclusive access during writes. Neither the kernel proper nor tenants may modify accepted state through writable aliases. State produced by initialization, checked import, or successful saves needs no repeated content validation while these guarantees hold.

### An exception occurs in kernelet kernel code

vOSTD currently has no recovery interface for kernel-mode exceptions. **Any CPU exception in kernelet kernel code must terminate the entire instance**, including `#GP` from `XRSTOR`. For an FPU instruction fault, the Host must reset carrier user FPU state and repair its ownership records before termination. Adapt Linux's [signal-return recovery](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/signal.c#L294) and [user-state cleanup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/core.c#L741) for this path. Do not resume kernelet execution after the fault. Import validation does not remove this fallback.

<div class="background">

**Linux: FPU restore faults.** Linux v6.12 registers [exception handlers for restore instructions](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/xstate.h#L130). Its [restore fixup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/mm/extable.c#L112) [resets FPU registers](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/fpu/core.c#L190) and continues after the failed instruction; it does not terminate the task.

</div>

## Alternatives and tradeoffs

**Let Linux save directly into the active tenant context.** This could avoid restoring A from the carrier's save area only to save it again into `A.FpuContext`. Each tenant switch would also need to update Linux's FPU save target and coordinate its ownership and pending-restore records. Linux scheduling and kernel FPU use must follow that selection. This requires broader changes to Linux's FPU management than the selected design, which retains the carrier save area and changes restoration timing.

## Implementation and integration

The kernel proper uses Asterinas's `CpuSync` policy to track FPU ownership and calls vOSTD's `FpuContext` interface. Linux keeps its per-carrier FPU save area and save logic.

- **vOSTD: checked imports and direct register access.** Provide `FpuContext::read_from` as proposed in [Asterinas #4210](https://github.com/asterinas/asterinas/pull/4210). The kernel proper uses it for signal return and other external FPU-state writes. Keep mutable access to context storage inside vOSTD. Save and restore directly between CPU registers and `FpuContext`, without endovisor FPU services or repeated image validation.
- **Linux: earlier carrier restoration.** For participating carriers, complete pending restoration before first vOSTD entry and before vOSTD resumes after scheduling or kernel FPU use. The existing Linux restore helper loads registers when needed, updates ownership, and clears `TIF_NEED_FPU_LOAD`. Ordinary syscall entry and service return need no additional FPU check.
- **Host: fault cleanup and instance termination.** Connect kernelet kernel exceptions to the existing termination path. For an FPU instruction fault, reset the carrier's user FPU state and repair ownership records before terminating the entire instance.

## Validation and evidence

### Same-tenant syscalls

One thread makes N consecutive `getpid` syscalls. Linux returns the process ID; kernelet returns `123` and resumes the same tenant. FPU setup and checks are outside timing; no FPU arithmetic occurs between calls. Values are **mean ns/call**.

All kernelet variants use the [optimized syscall PoC](https://github.com/tatetian/kernelet/pull/3). Original PoC omits `CpuSync`; the other variants use it.

| Preloaded state | Linux reference | Original PoC | PoC: save/load each syscall | PoC with `CpuSync` | This design |
|---|---:|---:|---:|---:|---:|
| x87 + SSE | 56.997 | 62.959 | 399.749 | 67.098 | 66.459 |
| + AVX | 57.011 | 63.079 | 405.611 | 68.042 | 66.636 |
| + AVX-512 | 56.963 | 63.298 | 413.857 | 67.525 | 66.612 |

Per-call save/load adds two FPU service calls, one register save and restore, **5,376 copied bytes**, 8,192 cleared bytes, and one image validation. Conditional `CpuSync` and this design make **zero tenant saves or restores**. Task-local access, callbacks, and state checks remain.

### Tenant switches within one carrier

A and B perform FPU arithmetic before each syscall. Linux switches two threads with `SCHED_FIFO` and `sched_yield`; kernelet switches two tenants on one carrier. Values are **mean ns per A→B→A**, including both tasks' arithmetic, two syscalls, and two switches. User work, address-space sharing, and guest CPU placement are the same.

| User work | Linux threads | PoC with `CpuSync` | This design | Change from PoC |
|---|---:|---:|---:|---:|
| x87 + SSE | 796.1 | 1339.6 | 848.0 | −36.7% |
| x87 + AVX | 818.1 | 1294.8 | 850.6 | −34.3% |
| x87 + AVX-512 | 865.8 | 1392.3 | 861.3 | −38.1% |

This design removes FPU services, staging copies, page clears, and repeated validation, reducing time by **34.3–38.1%**.

### Carrier scheduling

A and B run on separate carriers. Each performs FPU arithmetic and makes a syscall. The handler uses the test service `fpu_bench` to call Linux `yield()` and schedule the other carrier. Each carrier keeps its tenant. Native threads use `sched_yield`. Values are **mean ns per A→B→A**, including two syscalls and two Linux switches.

| User work | Linux threads | PoC with `CpuSync` | This design |
|---|---:|---:|---:|
| x87 + SSE | 789.5 | 798.9 | 787.8 |
| x87 + AVX | 816.7 | 825.7 | 822.0 |
| x87 + AVX-512 | 864.7 | 865.0 | 867.9 |

All three paths record **two Linux saves and two restores per round trip**, with **zero tenant FPU services or copies**. Boot ranges overlap; the small timing gaps are inconclusive.

### Cost of earlier FPU restoration

**This test measures the extra cost when a carrier resumes kernel code, then yields again before running user code.** Linux carrier C1 runs user thread A; carrier C2 runs user thread B. Each carrier keeps the same tenant. The test forces this sequence:

1. C1 runs A's FPU arithmetic. A enters a syscall, and C1 yields the CPU.
2. C2 runs B's FPU arithmetic. B enters a syscall, and C2 yields the CPU.
3. **C1 resumes the same syscall in Linux, then yields again. No user code or FPU computation runs during this visit.**
4. C2 runs B's FPU arithmetic again. B enters another syscall, and C2 yields the CPU.
5. C1 finishes A's original syscall and returns to A in user mode.

Both policies use the same test service. Values are **mean ns for this full sequence**, including **three syscalls and four Linux switches**.

| User work | PoC with `CpuSync` | This design | Increase |
|---|---:|---:|---:|
| x87 + SSE | 1343.0 | 1459.3 | 116.3 |
| x87 + AVX | 1365.7 | 1504.3 | 138.6 |
| x87 + AVX-512 | 1438.6 | 1590.4 | 151.7 |

With deferred restoration, Linux leaves B's FPU registers intact during step 3, so B can reuse them in step 4. This design instead restores A in step 3, saves A when C1 yields again, then restores B in step 4.

Diagnostics confirm **one extra Linux FPU save and two extra restores**: four saves and four restores, compared with three saves and two restores. Neither policy transfers tenant buffers. The **116.3–151.7 ns increase** covers this full sequence and its FPU state tracking, not a single restore instruction.

## Open questions

## Decision and follow-up

- pending review

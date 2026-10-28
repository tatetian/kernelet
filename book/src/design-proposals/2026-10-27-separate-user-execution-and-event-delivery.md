# 2026-10-27 Separate user execution and event delivery

- Status: Draft
- Author(s): Plucky923
- Discussion: Not yet available.
- Affected chapters: [The first kernel we will run](../accepted-design/the-first-kernel-we-will-run.md), [The kernelet ABI](../accepted-design/the-kernelet-abi.md), [Two stacks](../accepted-design/two-stacks.md), [Into user mode and back](../accepted-design/into-user-mode-and-back.md), [Tasks and virtual CPUs](../accepted-design/tasks-and-virtual-cpus.md), [Virtual interrupts](../accepted-design/virtual-interrupts.md), and [Stopping a kernelet](../accepted-design/stopping-a-kernelet.md).
- Implementation or experiments: Experimental Linux-host implementation. See [Validation and evidence](#validation-and-evidence).

## Summary

This proposal makes UserMode::execute non-returning and delivers syscalls, exceptions, and selected kernel events through a registered handler. Each delivered event starts a new invocation on the Task's kernel stack, without resuming an earlier execute call. The proposed contract applies to both native OSTD and vOSTD.

## Problem and motivation

OSTD currently exposes user execution through a returning call:

```rust
fn execute<T: UserModeHooks>(&mut self, hooks: &T) -> ReturnReason;
```

The kernel proper calls execute to run a user thread. When the call returns, the caller handles the event using the registers and exception details in `UserContext`, then calls execute again. This return-based interface requires the caller's execution state to survive user execution.

Native x86-64 OSTD implements this contract by saving six callee-saved registers and the stack pointer before entering user mode. The syscall entry path restores this state so that `execute` can return to its caller.

On x86-64 Linux, a syscall handler completes through Linux's exit path. Its completed frames need not survive user execution, and the next syscall starts at the kernel stack top.

The original PoC uses `klet_switch` to transfer control between the Linux hook and the kernelet on separate stacks. The hook resumes the suspended `execute` call to deliver an event; the next `execute` resumes the suspended hook so that Linux can finish returning to user mode. Each syscall round trip therefore invokes `klet_switch` twice, for a total of twelve register saves, twelve register restores, two stack-pointer saves, and two stack-pointer restores.

## Proposed change

### Execution contract

`execute` retains its name and hooks argument but becomes non-returning. Before handing control to the host, it disables local interrupts and calls `hooks.pre_user_run` to restore per-thread floating-point, FS-base, and GS-base state. Event delivery is handled separately through a registered handler.

#### Interface

Before any Task enters user mode, the kernel proper registers an event handler and a query for pending kernel-proper work. It supplies hooks on every call to `execute`. The proposed vOSTD interface is:

```rust
pub trait UserModeHooks {
    fn pre_user_run(&self, guard: &DisabledLocalIrqGuard) {}
}

pub fn register_user_event_handler(
    handler: fn(ReturnReason, UserMode) -> !,
    has_kernel_event: fn() -> bool,
);

impl UserMode {
    pub fn new(context: UserContext) -> Self;
    pub fn context(&self) -> &UserContext;
    pub fn context_mut(&mut self) -> &mut UserContext;
    pub fn execute<H: UserModeHooks>(&mut self, hooks: &H) -> !;
}
```

`UserMode` is an exclusive handle to the current Task's `UserContext`. The context is owned by the Task and survives individual handler invocations. The handle is movable but is neither `Copy`, `Send`, nor `Sync`. `execute` borrows the handle and passes access to the context to the host.

`ReturnReason` retains `UserSyscall`, `UserException`, and `KernelEvent`. Syscalls and exceptions are delivered directly to the handler, which accesses registers and exception details through `UserMode`. After a virtual interrupt, vOSTD calls `has_kernel_event` and delivers `KernelEvent` only if kernel-proper work is pending; otherwise, it resumes user execution without invoking the handler. The pending-work query is registered separately and remains independent of `UserModeHooks`.

The handler processes the event and either terminates the Task or resumes user execution by calling `execute` with its hooks:

```rust
fn on_user_event(reason: ReturnReason, mut user_mode: UserMode) -> ! {
    let hooks = current_hooks(); // the current Task's hooks
    handle_event(reason, user_mode.context_mut());
    user_mode.execute(&hooks)
}
```

Before calling `execute`, the handler must release temporary resources such as lock guards and heap allocations, either explicitly or by ending their scopes. The handoff abandons the current call chain without unwinding it, so values left in those frames receive no scope-exit cleanup. After this handoff, the next user event reuses the Task's kernel stack from the top.

Blocking or yielding during event handling preserves the unfinished invocation's stack so that it can resume. The call chain is abandoned when the handler hands control back through `execute`. The kernel proper saves user FPU state before a Task switch and restores it before user execution.


#### Protocol

On the Linux host, the hook enters vOSTD at the Task's kernel stack top. vOSTD starts a new handler invocation for events that require kernel-proper handling. The endovisor does not restore an earlier `execute` caller or save the hook's callee-saved registers for a later return.

After handling the event and releasing temporary resources, the handler calls `execute`. The host completes Linux's exit work on a fresh call chain and returns to user mode without resuming the discarded hook.

The Task stores user registers in `UserContext` and preserves an unfinished handler's stack across blocking or yielding. The handler's call chain is abandoned when it hands control back through `execute`. The kernel proper saves user FPU state before a Task switch and restores it before user execution.

## Alternatives and tradeoffs

1. **Keep `execute`.** Retaining the accepted return-based interface avoids an API migration. Linux entry code could be modified to bypass the hook and resume execute on the kernelet's stack, but the caller's state would still need to survive until the call returns. The handler interface removes this requirement at the cost of moving persistent state into Task-owned storage and requiring temporary resources to be released before each handoff.

2. **Change `IA32_LSTAR`.** Changing the syscall entry point is independent of the event-delivery API. The host could use the per-CPU IA32_LSTAR register to direct syscalls to a stub that prepares kernelet entry. On Intel processors, updating `IA32_LSTAR` with `WRMSR` is serializing, so the resulting overhead depends in part on update frequency. [Dune](https://www.usenix.org/system/files/conference/osdi12/osdi12-final-117.pdf) (§3.5) and other work also explain that MSR registers are avoided from being read or written in order to avoid performance overhead.

## Implementation and integration

The Linux-host implementation requires changes to vOSTD, the endovisor, Linux's entry code, and the kernel proper.

**vOSTD.** In `prototype/vostd/src/`, `task/mod.rs` and `user.rs` move persistent user state into the Task, since the frames that call `execute` no longer survive a handoff. A new event entry checks that the supplied context belongs to the current Task and is available for exclusive use before dispatching events to the registered handler. `abi.rs` exposes this entry to the host and extends the user-run service to pass the context and Task stack top.

**Endovisor.** In `prototype/linux/endovisor/`, `carrier.c` and `switch.S` change the event hooks to save user registers and enter vOSTD at the Task's stack top, abandoning the hook's call frames. When vOSTD calls `user_run`, the service establishes a completion frame on the Linux stack, restores user registers, and invokes Linux's return entry. Stop and eviction after the first user handoff also use fresh Linux frames. `kernelet.h` and `image.c` extend and validate the entry table for event delivery. Bootstrap and other paths that resume saved continuations retain the existing switching mechanism.

**Linux.** Because event delivery discards the hook frames, `user_run` can no longer resume the hook to complete Linux's return-to-user processing. We therefore modify `arch/x86/entry/common.c` and `entry_64.S` to provide an entry that completes the outstanding exit work from the new Linux frame and applies Linux's checks before taking the existing SYSRET or IRET path. Declarations in `include/linux/kernelet.h` make this entry available to the endovisor.

**Kernel proper.** Initialization registers the event handler, and thread startup calls `execute` once to begin user execution. The handler processes subsequent events and calls `execute` to resume the thread, replacing the old return-based loop. Per-thread state needed across events moves into Task-owned storage, and handlers release temporary resources before each handoff because abandoned frames do not run destructors.

## Validation and evidence

We compared three paths on Linux 6.12 under QEMU/KVM: native `getpid`, the original PoC, and the non-returning PoC. Each VM had two vCPUs and 512 MiB on an Intel Xeon Platinum 8259CL host. User execution ran on guest CPU 0, pinned to host CPU 24. Each path ran 12 boots.

The user loop contains only syscall 39 and loop control. The program checks return values before warm-up and after the timed loops. Timing uses the timestamp counter. Register saves and restores stay outside the timed region. Warm-up runs for 200 ms, and calibration doubles the call count until a loop reaches 500 ms. Each boot records ten timed repetitions.

We divide total time by total calls to get each boot mean. The table gives the equal-weight mean of the 12 boot means.

| Path | Mean time per call (ns) | Range (ns) |
| --- | ---: | ---: |
| Native Linux `getpid` | 56.375 | 55.9–56.7 |
| Original PoC | 125.082 | 123.9–125.9 |
| Non-returning PoC | 112.805 | 109.7–127.1 |

## Open questions

Linux saves the user general-purpose registers, instruction pointer, and flags in `pt_regs`. `klet_regs_to_ctx` copies this state into the Task's `UserContext`, and `klet_ctx_to_regs` copies it back before user execution. Both functions are in `prototype/linux/endovisor/carrier.c`. The two copies stay in the syscall path and add work beyond the register saves and restores at the user/kernel boundary. They are the next optimization target.

## Decision and follow-up

Pending review.

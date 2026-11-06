# Experimental kernelet vsock kernel

This safe Rust kernel exercises the proposal in
`book/src/design-proposals/2026-11-06-kernelet-vsock.md`. It adds a small socket
descriptor table to a one-process test kernel. The tenant uses the usual
x86-64 data-call register convention and scalar arguments for socket control;
it is not a Linux ABI compatibility implementation.

Run `make -C prototype vsock` from the repository root. The host driver runs
connection, authorization, wraparound, fault, backpressure, death, and survivor
checks, then measures host-user to tenant-user echo against paired Linux pipes.
The experiment defaults to notification mode 2; `VSOCK_OPT=0` retains the
reference policy and `VSOCK_OPT=1` suppresses reservation notifications only.
The separate `make -C prototype vsock-compare VSOCK_LABEL=trial VSOCK_OPT=2`
target measures real virtio-vsock as well, using controlled physical CPU
placement and MSI-X. It requires KVM, vhost-vsock, host AF_VSOCK, and three
available physical cores. Use a fresh label for each run and run harnesses
sequentially because they share staging paths. Profiling is separate:
`make -C prototype vsock-profile VSOCK_LABEL=profile VSOCK_OPT=2`.
`prototype/bench/vsock/README.md` explains reproduction, retained results,
CPU accounting, and fairness limits. `results.txt` there is historical data.

The test roles selected by the command line are:

| Role | Tenant behavior |
|---|---|
| 0 | Listen, accept, and echo `loops` transfers of `size` bytes |
| 1 | Connect to `peer`, echo round trips, and verify every returned byte |
| 2 | Listen and accept, then stall until killed |
| 3 | Listen without accepting, allowing a backlog to fill |
| 4 | Read until a peer reset, then reconnect to the host and verify an echo |
| 5 | Exercise partial and zero-byte translation faults on ring reads/writes |
| 6 | Verify concurrent write shutdown delivers an admitted prefix before EOF |
| 7 | Echo through the vOSTD channel API without tenant syscalls |
| 8 | Gate consumption on another connection to test a full-ring writer waking |
| 9 | Publish a position-dependent 4 KiB payload, then stall for RX-lease tests |
| 10 | Require EOF after an admitted TX lease is cancelled with zero bytes |
| 11 | Gate accepts through another connection to release a full backlog |

The broker's global mutex and carrier-blocking waits are experimental
limitations. The proposal records task-level readiness delivery and full
socket integration as follow-up work.

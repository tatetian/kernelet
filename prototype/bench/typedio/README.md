# TypedIO Vsock Implementation Progress and Performance Comparison Plan

## 1. Completed Work

### 1.1. TypedIO Core Mechanisms and Vsock Implementation

The TypedIO core mechanisms and Vsock device have been implemented following the [TypedIO Design Proposal](https://tatetian.github.io/kernelet/design-proposals/2026-11-08-typed-io.html).

The implementation includes:

- **Message / Schema / Protocol:** Data types and protocols for communication across components.
- **Device Discovery / Attachment:** Device discovery, authorization, and attachment.
- **Queue:** Request submission, completion notification, and asynchronous processing.
- **Byte Pipe:** Shared byte buffers and Producer / Consumer data transfer.
- **Resource Management:** Resource authorization, ownership transfer, and lifecycle management.
- **Host Framework / Notification:** Host-side request processing, resource management, and interrupt notification.
- **Vsock Frontend / Backend:** Integration with the Asterinas Socket interface and the Host-side Vsock service.

The two repositories have the following responsibilities:

- **Kernelet:** TypedIO Rust libraries and macros implementing Message, Schema, Queue, Byte Pipe, and related mechanisms; Endovisor C/Rust interfaces; the Host Framework, Linux File Adapter, Vsock Backend, and resource management; and extensions to vOSTD and the Host ABI.
- **Asterinas:** A TypedIO Vsock Frontend integrated with the existing Socket interface, together with the Kernelet platform configuration and boot integration needed for complete Asterinas to use Vsock through TypedIO.

The current implementation covers the TypedIO core and Vsock. Other TypedIO devices, such as Block and Filesystem, have not been implemented.

Implementation repositories:

- [Kernelet (Modified Repository)](https://github.com/yanming404/kernelet/tree/implement/typedio-vsock)
- [Asterinas (Modified Repository)](https://github.com/yanming404/asterinas/tree/integrate/typedio-vsock)

The latest synchronized implementation fixes are Kernelet `4778330555f79b7324b637b56651a5ce3be5a1d8` and Asterinas `b3b39ee8333b8e58586442f5dc5aaab4503c8281`. These include component initialization identity matching, pending interrupt delivery after idle returns, ECONNRESET propagation after peer death, and preservation of graceful EOF during context removal.

### 1.2. Complete Asterinas-on-Kernelet Adaptation

Following the Kernelet Book's design in which OSTD and vOSTD expose matching interfaces, the existing vOSTD has been extended to support complete Asterinas.

The platform work includes:

- **Memory management and virtual memory:** Frame, VM, VmIO, and other interfaces required by complete Asterinas.
- **Tasks and scheduling:** Adaptation of the Kernelet Carrier / vCPU execution model and Asterinas logical-thread scheduling.
- **Exceptions and interrupts:** Exception handling, virtual IRQs, and wakeup mechanisms needed by the complete kernel.
- **FS/GS base management:** A TLS Host Service supporting per-thread save and restore, extending the image ABI to ABI 8.
- **FPU state management:** Integration of floating-point and SIMD state save and restore across thread switches.
- **Complete kernel image:** Linking Asterinas with vOSTD into a PIE image accepted by the Kernelet Loader, including component initialization integration.

The main design references are:

- [What vOSTD adds on top](https://github.com/tatetian/kernelet/blob/main/book/src/accepted-design/what-vostd-adds-on-top.md)
- [Into user mode and back](https://github.com/tatetian/kernelet/blob/main/book/src/accepted-design/into-user-mode-and-back.md)
- [The Kernelet ABI](https://github.com/tatetian/kernelet/blob/main/book/src/accepted-design/the-kernelet-abi.md)
- [Tasks and virtual CPUs](https://github.com/tatetian/kernelet/blob/main/book/src/accepted-design/tasks-and-virtual-cpus.md)

Matching OSTD/vOSTD interfaces and per-thread FS base management are part of the original design. The ABI 8 TLS Host Service, complete kernel boot integration, and additional platform interfaces are the concrete implementation work described here.

### 1.3. Real Execution and Communication Validation

The following execution path has been validated on the new server:

```text
Physical server
    |
QEMU / KVM
    |
Guest Linux 6.12 (Kernelet Host)
    |
    +-- Endovisor
    +-- TypedIO Host Framework / Vsock Backend
    |
Kernelet Tenant
    |
    +-- vOSTD
    +-- Complete Asterinas
             |
        TypedIO Vsock Frontend
             |
        Asterinas userspace application
```

Linux schedules the carriers, with one carrier per Kernelet vCPU. Each Asterinas instance schedules its own logical threads. The complete PIE executes component initialization, kernel-thread and process initialization, VFS/initramfs setup, and a real userspace `/init`.

The completed real QEMU/KVM checks include:

- Guest Linux 6.12 and Endovisor startup.
- Loading the complete Asterinas PIE and reaching userspace.
- FS/GS, FPU, thread switching, and 1/2/4/8 vCPU tests.
- Noncanonical FS/GS rejection, canonical high FS/GS values through FSGSBASE and syscalls, signal/rt_sigreturn state restoration, XSAVE operation, and the 512-byte FXSAVE fallback.
- Carrier migration observed on both Guest Linux CPUs over 200 affinity changes.
- TypedIO Guest-to-Host communication.
- Guest-to-Guest communication between two independent Kernelet Tenants.
- Concurrent connections, backlog, partial I/O, ring wraparound, nonblocking I/O, poll/edge-triggered epoll, MSG_PEEK, backpressure, half-close, EOF, Reset, and peer death.
- Same-CID replacement and useful communication from the surviving endpoint after a real carrier SIGKILL.
- Zero live heap allocations, kernel stacks, and carrier records at teardown, followed by Endovisor unload.

Here, **Guest-to-Host (GH)** means an Asterinas Tenant communicating with a Linux Host application inside the same QEMU Guest. The Host application uses the TypedIO Linux File Adapter. **Guest-to-Guest (GG)** means two complete Asterinas/Kernelet instances inside the same Guest Linux. Neither term implies a second QEMU VM or a TypedIO peer on the Outer Host.

The original complete-kernel boot evidence is in `runs/bootstrap-1vcpu-component-fix/`, relative to this directory. Subsequent real KVM regressions are in `prototype/.build/typedio-kvm/sync-*`, relative to the repository root: `sync-platform-{1,2,4,8}`, `sync-fxsave`, `sync-migration`, `sync-gh`, `sync-gg`, `sync-backlog`, `sync-gg-reset`, and `sync-gh-reset`. Their environment records report PASS. Earlier failed attempts remain separate from the successful runs.

The 94 C/Rust ABI 8 layout checks, 96-byte cross-compiler schema comparison, image contract checks, and Native fixtures also passed. Compilation and Native fixtures are separate evidence from real execution; the tests do not exhaust all CPU extended states or interrupt interleavings.

### 1.4. Initial Performance Measurements

Real communication validation is complete, and performance measurement has begun. Initial performance data is available in [results.csv](results.csv), covering TypedIO measurements at different message sizes in both Guest-to-Host and Guest-to-Guest scenarios, with every repetition retained.

Currently, the available performance data consists of **21 GH measurements from `gh-a` and 21 GG measurements from `gg-a`**, with one independent QEMU boot per scenario and seven repetitions for each of 4, 16, and 64 KiB messages. Each measurement uses 64 warmup rounds, 8192 measured Echo RTTs, a 64 KiB capacity per direction, and full payload verification. GH starts complete Asterinas for each case; GG keeps two independent complete Asterinas instances running across its 21 cases. Initialization, sorting, output, and teardown are outside the timed window.

The environment was an Intel Xeon Platinum 8259CL server with Outer Linux 6.12.107+deb13-amd64, QEMU 10.0.13, and Guest Linux 6.12.0, using real KVM, 2 vCPUs, and 512 MiB RAM. The echo carrier, caller, and QEMU housekeeping were assigned to physical CPUs 20, 21, and 22 respectively. The governor was schedutil and halt_poll_ns was 200000; shared-server load and frequency variation were not isolated.

The table reports medians across seven repetitions. The goodput range is the observed minimum and maximum. RTT columns are medians of per-run percentiles, rather than percentiles recomputed from pooled RTT samples.

| Scenario | Message KiB | Samples | Bidirectional Echo MiB/s (range) | RTT p50 us | RTT p95 us | Endpoint CPU ns/B | Outer CPU ns/B |
|---|---:|---:|---:|---:|---:|---:|---:|
| GH | 4 | 7 | 11.500 (11.261-11.685) | 669.863 | 918.323 | 146.140 | 166.561 |
| GH | 16 | 7 | 42.019 (41.871-42.575) | 683.006 | 966.672 | 39.903 | 45.398 |
| GH | 64 | 7 | 133.154 (132.867-133.477) | 934.189 | 1038.954 | 13.285 | 14.394 |
| GG | 4 | 7 | 5.331 (5.290-5.372) | 1387.999 | 2167.924 | 349.500 | 358.701 |
| GG | 16 | 7 | 19.195 (19.143-19.346) | 1396.939 | 2624.125 | 96.678 | 99.750 |
| GG | 64 | 7 | 54.917 (54.816-55.589) | 2263.956 | 2497.497 | 33.524 | 34.831 |

**All 21 GG performance measurements passed**, including payload verification, completion by both Tenants, and clean resource teardown. The Client uses CID 42 on Guest CPU1 and the Echo Server uses CID 7 on Guest CPU0, in separate sandboxes with distinct carriers and private mappings. Echo payload flows directly through the TypedIO Byte Pipe between these Tenants; the Host handles only test control and measurement collection. Each scenario still has only one boot, which does not establish cross-boot stability, statistical significance, or a performance advantage over VirtIO.

#### Data sources and measurement conventions

The GH source is `performance/gh-a/serial.txt`, checked against its `outer.txt` and `environment.json`: 21 TypedIO rows are selected from 63 measurements, excluding all Pipe/VirtIO rows. The GG source is `performance/gg-a/serial.txt`, with 21 records explicitly marked `scenario=gg transport=typedio`, checked against its `outer.txt` and `environment.json`. The performance checker passed both complete matrices. GG uses VirtIO only for the Outer measurement-control channel; it runs no VirtIO Echo workload or Host userspace payload proxy. GG commands, checks, and summary are retained in `performance/gg-a/commands.txt`, `check-results.txt`, and `summary.json`.

Each CSV row includes its source file, one-based line number, log SHA256, boot identifier, experiment start time, base commits, local patch hashes, and image SHA256. The GH measurements started at `2026-11-18T13:42:28.501098+00:00`, before the fixes were committed, using Kernelet base `5036e59fb4e96f6a2d389c1bd6f4a579814d1f7b` and Asterinas base `48860174799d0796d4aa446c87274ad08a76dc53` plus the recorded patches. GG was run from the latest synchronized implementation commits listed above. Its kernel, configuration, Endovisor, Asterinas image, and benchmark binary hashes match the original GH artifacts. Per-row metadata preserves the separate experiment identities; the original GH rows have not been changed.

- `echo_bytes = 2 * message_bytes * measured_rounds`.
- `goodput_mib_s = echo_bytes * 10^9 / wall_ns / 2^20`. This is bidirectional Echo Goodput, not unidirectional streaming throughput.
- Raw time fields are in nanoseconds, and byte fields are in bytes. Derived metrics retain nine decimal places; raw integer measurements are preserved.
- `rtt_p50_ns`, `rtt_p95_ns`, `rtt_min_ns`, and `rtt_max_ns` are the recorded statistics for each run. The original implementation selects sorted zero-based indices 4096 and 7782 for p50 and p95 without interpolation. Individual RTT samples are unavailable, so missing means, other percentiles, or pooled distributions cannot be reconstructed.
- In GH, `caller_cpu_ns` is the Guest Linux caller's CLOCK_PROCESS_CPUTIME_ID and `peer_cpu_ns` is the Asterinas carrier's `/proc/PID/schedstat` delta. In GG, both fields are carrier schedstat deltas for the Client and Server, including kernel and userspace execution; their window brackets the start/end control barriers and is slightly wider than the Client's timed Echo window. Endpoint CPU ns/B uses the same formula, their sum divided by `echo_bytes`; it is not transport-only CPU. Both scenarios use CLOCK_MONOTONIC and the same percentile indices, but GG reads the clock through Asterinas syscalls while GH uses Linux clock_gettime.
- Outer CPU ns/B is the sum of QEMU task CPU excluding identified vhost workers, separate vhost CPU, and the Outer measurement-control process CPU, divided by `echo_bytes`. For TypedIO rows, `outer_peer_control_cpu_ns` measures only the control process, not the TypedIO echo peer.
- The Outer window includes the start ACK and end token and is slightly wider than the Guest wall-time window. Idle/polling costs are not subtracted. **Outer QEMU CPU includes Guest execution, so Endpoint and Outer CPU must not be added together.** The data does not isolate individual Queue, Byte Pipe, Message, copy, notification, or scheduling costs.

Missing measured fields must remain empty; scenarios without actual measurements must not receive placeholder samples. The existing raw logs and build artifacts remain local evidence and are not part of the README/CSV deliverable.

## 2. Next Step: TypedIO Vsock Performance Comparison

The next step is to compare TypedIO Vsock with traditional VirtIO-Vsock across message sizes and communication workloads, measuring throughput, latency, and CPU cost to assess performance trade-offs and possible advantages.

Ideally, the operating system, Socket API, userspace program, and workload would remain the same, with only the underlying communication mechanism changed.

The working TypedIO path is:

```text
Guest Linux 6.12 (Kernelet Host)
          |
Endovisor / Host Framework
          |
       TypedIO
          |
Kernelet Tenant (vOSTD)
          |
Complete Asterinas
```

TypedIO connects the Host-side Framework with Tenant-side vOSTD through Queue, Byte Pipe, Notification, and related mechanisms. Asterinas accesses it through its Vsock Frontend.

**The main obstacle is that TypedIO cannot currently be replaced directly with VirtIO-Vsock inside the existing Kernelet Tenant.**

QEMU's VirtIO-Vsock PCI device is attached to the virtual PCI bus owned by Guest Linux. Guest Linux discovers and accesses it through its PCI and VirtIO drivers. A Kernelet Tenant is independent kernel code executed through Linux carriers; it is not a VM with its own virtual PCI bus. The current platform does not expose the device discovery, MMIO/PCI configuration, queue/DMA, and interrupt capabilities needed for direct Tenant access.

In addition, the Asterinas Kernelet configuration selects the TypedIO transport at compile time. The VirtIO transport used by the ordinary hardware platform is not connected to the current Kernelet execution path. Merely retaining the native VirtIO driver does not make the device available to the Tenant.

Relevant source files:

- [Kernelet Socket Transport](https://github.com/yanming404/asterinas/blob/integrate/typedio-vsock/kernel/core/src/net/socket/vsock/socket_transport_kernelet.rs)
- [Ordinary Hardware Socket Transport](https://github.com/yanming404/asterinas/blob/integrate/typedio-vsock/kernel/core/src/net/socket/vsock/socket_transport.rs)
- [Asterinas Cargo Configuration](https://github.com/yanming404/asterinas/blob/integrate/typedio-vsock/kernel/core/Cargo.toml)

Two approaches are under consideration:

1. **Expose or delegate VirtIO-Vsock device access to the Kernelet Tenant.** Make the capabilities already available to Guest Linux accessible to the Tenant so Asterinas can use its native VirtIO-Vsock Frontend. This would bring the comparison closer to using the same execution environment, but requires additional device access, queue, and interrupt interfaces, together with clear device ownership, driver coordination, address translation, and reset/lifecycle rules. Access to `/dev/vsock` alone is not equivalent to exposing a native VirtIO device.
2. **Run complete Asterinas in a separate QEMU/KVM Guest.** Reuse Asterinas's existing VirtIO-Vsock driver as the comparison path. This reduces driver development work, but changes the execution architecture and isolation boundary. The measured difference would include Kernelet-versus-VM architectural effects and could not be attributed entirely to the transport.

An additional fallback is a **Guest Linux Host Proxy** that bridges Tenant communication to Guest Linux's VirtIO-Vsock endpoint. It adds proxying, forwarding, and potentially extra copies and scheduling. Such results must be labeled and measured separately; they would not represent direct Tenant use of VirtIO-Vsock and would not replace the two approaches above.

The existing `gh-a` run already includes a Guest Linux-to-Outer Host VirtIO reference path. Its endpoints and topology differ from the TypedIO Tenant-to-Guest Linux Host path, so it is not a comparison using the same Asterinas Tenant workload. Those VirtIO records are excluded from `results.csv`.

The next decision is therefore to select a suitable comparison architecture while reusing existing implementations and limiting additional code changes. After correctness validation, use matched workloads, 4/16/64 KiB messages, a 64 KiB directional capacity, 64 warmup rounds, 8192 measured RTTs, and seven repetitions per group. Keep GH and GG separate, obtain at least two independent boots per scenario, retain every repetition, and report CPU scopes without double counting.

The goal is to quantify TypedIO's throughput, latency, CPU usage, and actual costs across workloads. The current 21 GH measurements are preliminary evidence. Neither proposed VirtIO integration approach has been implemented or validated in these experiments.

# The kernelet-in-Linux prototype

This is the companion prototype of *Building Kernelets on Linux*. It
runs Asterinas's 100-line Hello World kernel, unedited, as a **kernelet**: a
kernel compiled against an OSTD-shaped library (vOSTD), loaded into a patched
Linux 6.12 as part of a module, running in kernel mode on its own stacks,
serving its tenant program's system calls, mapping its memory in a Linux page
table, scheduled by Linux on ordinary tasks, and stopped by Linux like any
process. Four more kernels proper test the parts the 100-line kernel does not
reach: exceptions and eviction, preemption of kernel code, a scheduler of the
kernelet's own, and tenant memory.

Every claim the book makes about the prototype is a `make` target here that
builds what it needs, boots it in QEMU, and checks the serial log.

## The layers

From the tenant program down to the hardware, with the directory that holds
each layer and the chapter of the book that explains it:

```text
  tenant program       kernels/hello/hello.S, probe.S, ...        user mode
 ------------------------------------------------------------------------------
  kernel proper        kernels/hello/src/lib.rs (never edited),   ch. 3
                       kernels/probe/ yield/ sched/ mem/
  vOSTD                vostd/  (with libs/, two Asterinas crates) ch. 4, 9, 10, 11, 14
 ----------- the image ABI: services, entry table, boot arguments, ------------
             vCPU records   vostd/src/abi.rs = linux/endovisor/kernelet.h  ch. 4
  endovisor            linux/endovisor/  (endovisor.ko)            ch. 4, 6-13
  the endovisor UAPI    linux/endovisor/uapi/endovisor.h            ch. 2
  the carrier hooks    linux/patches/0001-carrier-hooks.patch     ch. 5
  Linux 6.12           .build/linux-6.12, configured by linux/config.fragment
 ------------------------------------------------------------------------------
  kernelet runtime     host/runtime/holder.c, sandbox.c, run-*.c  user mode on the host
  initramfs (PID 1)    host/initramfs/init.c
  QEMU with KVM        the Makefile's `run`
```

The source directories are: `kernels/` (the five teaching kernels and a PIE isolation test kernel), `vostd/` and `libs/` (the framework and the two
crates it and the kernels take unchanged from the Asterinas tree), `linux/`
(everything that is Linux's: the configuration fragment, the patch and the
endovisor module), `host/` (what runs in the booted Linux's user space: the
kernelet runtimes and PID 1), `bench/` (the scripts that turn the
measurement logs into tables), and `tools/` (the build-time ELF image checker).

The kernel proper and vOSTD are compiled as position-independent code and
linked into one self-contained PIE (`ET_DYN`), the **image**, a file of its own
(`.build/<kernel>.image`). The holder registers it as a *kind*. The endovisor
maps its shared physical code pages at a separate virtual address for each
sandbox, with private writable data and relative pointer relocations; one
`endovisor.ko` serves every kernel proper. The image calls no
Linux symbol: everything it needs from the host is a service in the table the
endovisor hands it at entry. The endovisor is an ordinary out-of-tree module
apart from the patch to Linux, which adds 223 lines (202 of them not blank)
and removes none, most of them in the generic entry code and `kernel/fork.c`.
The kernelet runtime is the user-space program that creates a sandbox through
the endovisor UAPI, `/dev/endovisor`: its holder creates the sandbox and forks
one child per virtual CPU, which executes that virtual CPU's exec descriptor
and so becomes its carrier.

Each directory has a `README.md` saying what is in it.

## Building and running

The optional `make vsock` experiment builds a safe socket test kernel and runs
host-to-tenant and peer-to-peer communication, failure checks, and pipe
comparisons. Its scope and limitations are documented in
[`kernels/vsock/README.md`](kernels/vsock/README.md) and the
[Kernelet Vsock proposal](../book/src/design-proposals/2026-11-06-kernelet-vsock.md).
It is not part of Accepted Design.

`make vsock-compare VSOCK_LABEL=trial VSOCK_OPT=2` adds a real virtio-vsock
comparison with physical CPU pinning and MSI-X. It needs host AF_VSOCK,
`/dev/vhost-vsock`, and three available physical cores in addition to KVM.
`make vsock-profile VSOCK_LABEL=profile VSOCK_OPT=2` collects separate
diagnostics. Labels must be unique; run harnesses sequentially. The retained
methodology, raw data, and replay commands are in
[`bench/vsock/README.md`](bench/vsock/README.md).

### What it needs

- **Linux on x86-64 with KVM**, and QEMU's `qemu-system-x86_64`, found on
  `PATH` (or named with `make QEMU=/path/to/qemu-system-x86_64 ...`). The
  options the boots use (`-enable-kvm -cpu host`, `-kernel`/`-initrd`,
  `-serial file:`, a raw virtio drive) are old; QEMU 6.2 (Ubuntu 22.04's) and
  10.x (a 10.1 pre-release, which reports itself as 10.0.92) have both been
  tested. A QEMU that does not start fails the boot with a
  message saying so. The guests use two CPUs and 512 MiB.
- **The kernel's configuration.** The Linux the tests boot is built here,
  from `tinyconfig` and `linux/config.fragment`, and two of the fragment's
  settings are preconditions rather than conveniences. Vsyscall emulation
  (`CONFIG_X86_VSYSCALL_EMULATION`) must be off: with it, a tenant that jumps
  to the vsyscall page has Linux run `gettimeofday`, `time` or `getcpu` for
  it in the page-fault handler, without passing the syscall hook, and the endovisor
  refuses to load on such a kernel (it cannot read whether `vsyscall=none` was
  given at boot, so it refuses either way). And `CONFIG_BUG` must be on:
  `tinyconfig` turns it off, and then every `WARN_ON()` in Linux is silent,
  so that the checks' search for `WARNING:` would prove nothing.
- **Rust** through `rustup`: `rust-toolchain.toml` names the nightly
  (`nightly-2026-07-21`, with `rust-src` and the `x86_64-unknown-none`
  target), and `rustup` installs it on first use. The image is built with
  `-Zbuild-std` for `x86_64-unknown-none`.
- **What the build needs**: `gcc` with a static C library, including the
  static `libpthread` (`libc6-dev` on Debian and Ubuntu, `glibc-static` on
  Fedora), because the runtimes and `init` are linked `-static`; `make`,
  `patch`, `flex`, `bison`, `bc`, `perl`, the `libelf` headers (objtool),
  binutils (`ld`, `objcopy`, `readelf`, `nm`), `tar`, `xz`, `gzip`, `curl`,
  `python3`, `awk`, `sed`, `grep`, GNU coreutils (`timeout`, `truncate`,
  `stat`, `seq`), GNU findutils
  (the initramfs list is written with `find -printf`), `cmp` (diffutils, for
  `kernel-proper-check`), and `mkswap` for `make mem-pressure`.
- **No root.** Nothing is installed, and the initramfs is written by the
  kernel's own `usr/gen_init_cpio` from a file list, so its `/dev/console`
  node needs no `mknod`. QEMU needs read and write access to `/dev/kvm`.
- **Disk**: the unpacked and built kernel tree takes 1.8 GB under `.build/`,
  the Rust build about 230 MB, the logs and the 512 MiB sparse swap file of
  `make mem-pressure` about 120 MB. Two steps refuse to start with less than
  6 GiB free on the filesystem `.build/` is on (`MIN_FREE_GB`): the download
  and unpacking of the tree (`make source`, or the first `make linux`), and
  `make linux` while the tree has no `bzImage` or `Module.symvers` yet. Once
  the kernel is built, no target checks the free space, and a boot needs a
  few megabytes.
- **Variables** a user may set on the command line: `QEMU` (above), `JOBS`
  (parallel jobs for the Linux and module builds, default 8), `TIMEOUT`,
  `MIN_FREE_GB` (default 6), and `MEM_PERF_BOOTS` (how many boots
  `make mem-perf` makes, default 5). `TIMEOUT` is how many seconds one boot
  may take before QEMU is killed and the boot fails. Each test sets its own
  (120 s for `hello`, `probe`, `sched-policy` and `sched-fpu`, 180 s for the
  other boots, 300 s for `mem-perf`'s); a `TIMEOUT` given on the command
  line, as in `make test TIMEOUT=600`, wins over all of them, for every boot
  of every target, so a slow host can raise it. For a single boot run by
  hand: `KLET_KERNEL` (which kernel proper, above all for `make image` and
  `make endovisor-module`), `WATCH=0` (load the module with `klet_watch=0`, for
  `make stage-yield`) and `MIRROR=0` (`klet_mirror=0`, for the sched
  kernel's boots). The rest, `SCHED_*` (`SCHED_VCPUS`, `SCHED_MODE`,
  `SCHED_DUR`, `SCHED_ARGS`, `SCHED_RUNMODE`, `SCHED_TIMEOUT`), `MEM_*`
  (`MEM_VCPUS`, `MEM_ARGS`, `MEM_MODPARAMS`, `MEM_RUNMODE`, `MEM_TIMEOUT`,
  `MEM_LOG`, `MEM_TARGET`), `PROBE_RUNMODE`, `KILL_WHERE`, `BOOTS_ONLY` and
  `REPORT_ONLY`, are how the tests drive their own boots and checks, and are
  set by the targets themselves.

### From a fresh clone

```sh
make crates   # once, online: the Rust crates of Cargo.lock and of -Zbuild-std
              # (the first build does this itself if the registry lacks them)
make test     # fetches, unpacks, patches, configures and builds Linux the first
              # time; then builds the image, the module, the runtime and the
              # initramfs for each test, boots, and checks the log
make bench    # the measurements
```

The first `make test` downloads the v6.12 tarball (about 140 MB) from
cdn.kernel.org, unpacks it into `.build/linux-6.12`, deletes the tarball,
applies the patch, configures and builds the kernel (a `tinyconfig`
kernel, so this is quick), builds the Rust image, and then runs the tests:
315 s measured from an empty `.build/` on an eight-core machine, 134 s of them
in the first test (measured before the kernel gained `CONFIG_BUG` and `probe`
its second boot, and not measured again since). After that, `make test` takes about four
minutes (252 s measured three times, the three boots of `probe` 23 s of them), and
`make bench` a little less (190 s: eight boots of the sched kernel and five of
the mem kernel). Every build is `--offline`, so only the toolchain (once,
through `rustup`), the crates and the one download of Linux need the network;
an offline build that finds a crate missing says to run `make crates`.

`make test` runs, in order, and stops at the first failure: `hello`, `pie`, `probe`,
`yield`, `sched-policy`, `sched-coop`, `sched-fpu`, `mem-func`, `mem-r2`,
`mem-conc`, `mem-kill`, `mem-pressure`. It prints one line per test; each
test's whole output is in `.build/test-<name>.out`. `make bench` runs
`sched-fair`, `sched-perf` and `mem-perf` and prints their tables; their
assertions depend on the host being quiet, so they are reported and never
fail the suite. Each one's boots go to `.build/bench/<name>.out`, and its
check, run once after them, to `.build/bench/<name>.table`, which ends with
the verdict, `PASS` or `FAIL`.

## The targets

Run them from `prototype/`, or from the repository root as
`make -C prototype <target>`; the root `Makefile` itself has only `test`,
`bench` and `crates` for the prototype.

| target | what it does | chapter |
|---|---|---|
| `make test` | the twelve tests below, in order | – |
| `make bench` | `sched-fair`, `sched-perf`, `mem-perf`, reported | 14 |
| `make hello` | first `uapi-check`, the endovisor UAPI's refusals and promises one by one (114 checks, including descriptor reads and readiness, last-close cleanup, malformed PIE images, and multiple sandboxes of one kind); then the 100-line kernel, byte-identical to `kernels/hello/upstream/`, with its console and log in files: "Hello, world" in the console file, a clean stop, a clean module unload; also the one-off measurements of what a service costs ("service stub: N cycles") and of what a heap allocation and its free cost ("heap: ... N cycles the pair") | 2, 3, 6, 7 |
| `make pie` | one registered image, concurrent mappings with shared physical code and private data; relocated pointers, survivor progress after peer destruction, and fresh state on replacement | 2 |
| `make probe` | three boots. The probe: demand paging, a 4 MiB `write` whose buffer the heap refuses (the kernel proper answers `-ENOMEM`, and Linux's page allocator is never asked), exceptions other than page faults (`ud2` twice in a row, trap 6; `int3`, trap 3; `hlt`, trap 13, a general-protection fault), each fault signal ignored again after it was drained, and eviction of a kernelet spinning in kernel mode when its carrier is sent `SIGKILL` (by the watch timer). Then the same tenant under a seccomp filter that kills the process on its endless system call: Linux forces `SIGSYS` on the carrier as a fatal signal, the resume hook leaves it to Linux, the carrier dies by it, and its death ends the sandbox, with no warning. Then the probe ended by `KLET_KILL`, which leaves the spinner to the eviction sweep | 2, 4, 7, 11, 12 |
| `make yield` | two boots under `preempt=none`, the watch timer off (the competitor starves) and on (it does not, and the checksums agree) | 9, 10 |
| `make sched-policy` | the kernelet's own scheduler: its trace checked against a reference model | 8 |
| `make sched-coop` | three boots: the mirror on (no involuntary deschedule in a critical section), off (the control), and an overstayer forced to yield at the first tick at least a millisecond after the first strike, about 2 ms on the machine the book was measured on (the check allows 4000 us, because a late tick lengthens it) | 10 |
| `make sched-fpu` | floating-point registers of two tenant threads in two address spaces survive every switch | 8 |
| `make mem-func` | E2, E3, E4, E10: no Linux fault on mapped pages, fault-around, protection changes, a map under a guard, the withheld set | 12, 13 |
| `make mem-r2` | E5: the sleeping contract, broken on purpose, ends the kernelet | 13 |
| `make mem-conc` | E8, E11 on two vCPUs: lockless copies, the contended range lock, a process exiting under a copy, the retire list | 13 |
| `make mem-kill` | E11 and the carriers as Linux tasks: one carrier of a two-thread process is preempted by a competitor, sent SIGTERM and SIGUSR1 and then 2000 rounds of SIGQUIT, SIGABRT, SIGTSTP, SIGTTIN and SIGTTOU (it lives and runs, and none of them reaches its queue; the carrier of vCPU 0 is sent a SIGTERM, a SIGQUIT and a SIGTSTP, and lives too), stopped and continued, and killed while its sibling faults in the same address space; the kill ends the whole sandbox cleanly. Its tenant runs the rewind check meanwhile: a register count and a memory count that must never part. Then two short boots, the carrier of vCPU 1 killed while it is idle in `vcpu_idle` and while it runs a kernel task that calls no service: each kill must end the sandbox within two seconds | 7, 11, 12 |
| `make mem-pressure` | E7: the area's entries survive reclaim and swap | 12 |
| `make sched-fair` | measurement: a sibling group's share of the processors with 1, 50, and 50 tasks and an overstayer | 10, 14 |
| `make sched-perf` | measurement: a kernelet task switch, a wake-up on the same and on the other vCPU, and a round trip between two tenant processes, beside native Linux, five boots; fails if a kernelet measurement has no samples | 14 |
| `make mem-perf` | measurement: E6 and E9, the memory costs, five boots | 14 |
| `make sched`, `make mem` | the sched tests (`sched-policy`, `sched-coop`, `sched-fpu`); the mem tests (`mem-func`, `mem-r2`, `mem-conc`, `mem-kill`, `mem-pressure`). The measurements are `make bench`'s | 8, 10, 12, 13 |
| `make crates` | download the Rust crates, once | – |
| `make source` | download and unpack Linux v6.12 if `.build/linux-6.12/Makefile` is not there, and apply the patch | 4 |
| `make patch` | apply the patch; idempotent | 4 |
| `make linux` | configure (once, from `linux/config.fragment`; `make config` does only that) and build `bzImage`, `modules` (which writes the tree's `Module.symvers`, without which no out-of-tree module links) and `modules_prepare` | 4 |
| `make image` | build vOSTD and the kernel proper named by `KLET_KERNEL` (`hello`, `probe`, `yieldk`, `schedk`, `mem`, `piecheck`) and link `.build/<kernel>.image` as a self-contained PIE; check its load segments, entry table and private `R_X86_64_RELATIVE` relocations | 2, 3 |
| `make endovisor-module` | `linux/endovisor/endovisor.ko`, which carries no image | 3 |
| `make runtime`, `make initramfs`, `make run` | the runtimes (the holder, `run-test`, the drivers); the initramfs from `.build/stage`; one boot | 2, 5, 7 |
| `make clean` | remove the harness's artifacts: the image and module, the runtimes, the staging area and the initramfs, the Rust build, every log and test output. It leaves the Linux tree, its build, the Cargo registry and `.build/crates.stamp` (which says the registry has the crates) | – |
| `make distclean` | `clean`, then `make mrproper` in the Linux tree, which removes its build and its configuration (`.config`, `include/generated`). It leaves the unpacked source, the patch applied to it and the patch's stamp, so the next `make linux` configures and builds again but neither downloads nor patches | – |

Internal targets, which the ones above call: `all` (`linux initramfs`),
`module DIR=<dir>` and `stage-module DIR=<dir>` (an out-of-tree module, built
against the tree and staged), `kernel-proper-check` (the `cmp` of `kernels/hello/`
against `kernels/hello/upstream/`, before every `make hello`), `df-guard` (the free
space check above), and the `stage-*`, `*-boot` and `check-*` targets.

## Module parameters

The endovisor takes the following parameters, which the tests set through the initramfs's
`/modules/order`:

| parameter | default | what it does | used by |
|---|---|---|---|
| `klet_watch` | 1 | the per-processor watch timer: the kernelet's tick, the delivery of virtual interrupts into kernelet code, and the two-tick bound on a critical section | `make yield` (0 for the control boot) |
| `klet_mirror` | 1 | vOSTD mirrors its guard depth into Linux's preemption count, so that Linux does not deschedule a carrier in a kernelet critical section | `make sched-coop` (0 for the control boot) |
| `klet_test_hooks` | 0 | test-only delayed `vm_map`, bounded channel TX/RX delays, and registered channel-demand inspection | `make mem-conc` and channel tests (1) |
| `klet_test_sleep_ms` | 20 | how long such a `vm_map` sleeps, in milliseconds | – |
| `klet_vsock_opt` | 0 | read-only notification policy: 0 reference, 1 suppress reservation wakes, 2 also separate poll and gate blocking commit wakes | channel Make targets select `VSOCK_OPT`, default 2 |
| `klet_vsock_profile` | 0 | opt-in channel counters and elapsed intervals; not enabled in normal comparisons | `make vsock-profile` (1) |

## The serial log

Everything the guest prints goes to its serial port, which QEMU writes to
`.build/serial.log`; each boot overwrites it. The kernelet's own output is
the lines prefixed `kernelet:`: the endovisor's own, and, for a sandbox
given no console and no log (the holder's `-k`, which every test but `hello`
uses), the kernel proper's console lines (`println!`, the `console_write`
service) and vOSTD's log lines (the `log_write` service), both `printk`.
`INIT:` lines are PID 1's, `RUN-TEST:` lines a test's steps, and `holder:`,
`RUN-*:` and `UAPI-CHECK:` lines the runtime's, which it writes through
`/dev/kmsg` so that they are in order with the endovisor's. Each test keeps a
copy of its boot's log before it checks it:

| test | log |
|---|---|
| `hello`, `sched-fpu` | `.build/serial-<test>.log` |
| `probe` | `.build/serial-probe.log`, `.build/serial-probe-seccomp.log`, `.build/serial-probe-klet-kill.log` |
| `yield` | `.build/serial-yield-off.log`, `.build/serial-yield-on.log` |
| `sched-policy`, `sched-coop`, `sched-fair`, `sched-perf` | `.build/serial-sched-*.log` |
| `mem-*` | `.build/mem/<name>.log` (`func`, `r2`, `conc`, `kill`, `kill-idle`, `kill-spin`, `pressure`, `perf-1` ... `perf-5`) |

A check fails on any `BUG:`, `Oops`, `WARNING:`, `Call Trace` or `switching
to a task already running` (vOSTD's report of a scheduler handing it a task
that is still running elsewhere) in any of its boots' logs, the control boots
included (Linux prints `WARNING:` only because the kernel is built with
`CONFIG_BUG`, above), on a missing line it expects, on
a sandbox whose end did not give back what it held or that was never
destroyed, on a module reference count other than 0 when the holder is done
(read once, with every carrier reaped and every descriptor closed), and on an
unclean module unload: init's `delete_module endovisor failed`, a log without
the endovisor's `kernelet: STATS` line or init's `INIT: done`, or anything the
endovisor allocated for the sandbox still live at unload.

When a sandbox's last carrier dies, the endovisor prints `kernelet: sandbox N
is gone:` with how many heap blocks the kernelet still held and how many
kernelet stacks it returned to `vmalloc`, then `kernelet: sandbox N exited:`
with how it ended, and `kernelet: sandbox N destroyed` when it is destroyed.
The counters at unload are the totals of every sandbox the module ran. When the module unloads, it
prints its counters, in this order: `eviction:` (only if a sweep ran),
`forced yield of vcpu ...` (the first few, only if there were any), `watch
timer:` (ticks, upcalls, voluntary yields), `kicks:` (kick IPIs and the
watch timer's requested firings), `STATS`, `MEMSTATS`, `kills:` (where a
fatal signal to a carrier was found), `heap:` (blocks allocated, freed by the
kernelet, freed at teardown, live), `kernelet stacks:` (made, returned, live)
and `carrier records:` (made, freed, live); `kernelet: endovisor unloaded`
ends them, and init's `INIT: done` and Linux's power-off follow. Every check
requires 0 live in each of the last three.

## Signals, stops and memory

- **Signals.** Every signal that can be ignored is ignored by every carrier:
  when the holder's child executes its exec descriptor, the endovisor sets
  all of them but `SIGKILL` and `SIGSTOP` to `SIG_IGN` and clears the signal
  mask. An ignored signal is discarded when it
  is sent, so an operator's `SIGTERM`, a `SIGQUIT` or `SIGABRT`, a `SIGTSTP`,
  `SIGTTIN` or `SIGTTOU`, never becomes pending on a carrier. Draining them
  on the way to user mode would not be enough: the exit loop delivers
  signals right after the resume hook in the same pass, if the flags it read
  before the hook said one was pending, so one that arrived between the
  drain and that delivery would be Linux's to act on, a core dump and the
  end of the carrier and its sandbox, or a stop; and a signal whose default
  is to terminate without a core dump is fatal the moment it is sent, before
  any drain. What does reach a carrier's queue is what Linux forces on it,
  which first sets the signal back to `SIG_DFL`: a fault of the tenant's own
  instruction, and seccomp's `SIGSYS`. The resume hook drains it before
  Linux can deliver it, since it was queued before the exit loop began, and
  then ignores it again, so that the next fault is forced, and drained, the
  same way. The one exception is a signal Linux has made fatal for good,
  which it marks `SA_IMMUTABLE`: seccomp's `SIGSYS` when a filter kills
  (`SECCOMP_RET_KILL_PROCESS` or `_THREAD`), which arrives before the syscall
  hook ever sees the call. Seccomp has then already marked the task dead, and a
  carrier that lived on would hit seccomp's `WARN` at its next system call.
  So the drain never takes such a signal, and the resume hook, finding one
  pending, returns at once and lets Linux deliver it: the carrier dies by it
  (`ended by signal 31` in the log), and its death ends the sandbox. A
  filter that only traps (`SECCOMP_RET_TRAP`) sends an ordinary `SIGSYS`,
  which is drained and dropped, and the call it trapped is never served. A fault signal becomes the CPU exception the kernel proper sees:
  `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGTRAP` with a fault's
  `si_code`, and `SIGSEGV`, `SIGBUS`, `SIGTRAP` and `SIGFPE` with
  `SI_KERNEL`, which is how Linux reports #GP, #TS, #NP, #SS, #BP and the
  legacy #MF; anything else drained is dropped. `SIGKILL` is Linux's, and
  kills the carrier. `SIGSTOP` stops a carrier the way it stops any
  task, and **a stopped carrier is a stopped virtual CPU**: the kernelet's
  tasks on it do not run, a sibling vCPU waiting for one of them waits,
  until `SIGCONT` (which continues a stopped task although it is ignored).
  A carrier being traced (`ptrace`) is the
  exception: Linux queues even ignored signals for a tracer, so a debugger
  attached to a carrier sees them.
- **Losing a carrier ends the sandbox.** When any carrier dies while the
  sandbox is live, killed by an operator or by the OOM killer, the endovisor
  kills the rest and evicts whatever is still running kernelet code: a
  sandbox that has lost a virtual CPU would otherwise run on with the others
  spinning on whatever locks the lost one held.
- **A `SIGKILL` to a carrier ends the sandbox wherever the carrier is.** Linux
  acts on it only on the way to user mode, so the endovisor covers the other
  places. In user mode, Linux kills the carrier, and its death ends the
  sandbox. Asleep in a service (`vcpu_idle`, `vm_wait`, a waiting boot) or
  running one, the sleep is killable, and on the way out of every service
  (`klet_svc_body`) a pending fatal signal ends the sandbox and the stub
  leaves the kernelet for good instead of returning into it. Running kernelet
  code, the watch timer's next tick finds the signal and evicts the carrier,
  which then dies. The last needs `klet_watch=1`: with the watch timer off, a
  carrier that loops in kernelet code without calling a service cannot be
  reached until something else ends the sandbox.
- **An idle virtual CPU is not load.** A carrier asleep in `vcpu_idle`, or
  waiting for the other carriers or to be booted, sleeps with `TASK_NOLOAD`,
  so it does not count in the host's load average.
- **The end of a sandbox is the endovisor's bookkeeping.** Whatever ends it
  (the kernel proper's power-off, `KLET_KILL`, the last close of its
  descriptor, a carrier lost while it is live) marks it dying, records why,
  kills every carrier and arms the eviction sweep, since no root carrier is
  left to take a spinning virtual CPU's processor back; it has exited when
  its last carrier is gone, or at once if it had none.
- **The sandbox's end gives everything back.** When its last carrier dies,
  the endovisor destroys its address spaces, frees every heap block the
  kernelet still held (every block `mem_alloc` hands out is recorded, so
  that a stopped or evicted kernelet's tasks, address spaces and run queues
  do not leak), and returns the whole kernelet-stack pool to `vmalloc`, the
  stacks still in use included. The grant, the exec files, the console and
  log, the control group and the owner go when the sandbox has exited and
  its last descriptor is closed. Closing the last descriptor while it is live
  requests its death; that close does not wait for the carriers to stop.
- **Memory.** What the kernelet allocates through the host is charged to the
  memory cgroup of the carrier that asked, which is the sandbox's: its heap
  (`mem_alloc`, `GFP_KERNEL_ACCOUNT`, and the table that records its blocks), its kernelet stacks (`__vmalloc` with
  `GFP_KERNEL_ACCOUNT`, at most the sandbox's `max_tasks` and one per virtual
  CPU, of 256 KiB each; beyond that `kstack_alloc` returns 0 and vOSTD reports
  `Error::NoMemory`), the page-table pages of its address spaces, which
  Linux allocates and charges itself, and the grant, the 2 or 4 MiB of frames
  the kernelet maps for its tenants (`alloc_pages` with `GFP_KERNEL_ACCOUNT`,
  by the first carrier to arrive, which is in the sandbox's group). The
  memory controller has to be enabled in the parent's `cgroup.subtree_control`
  for the group to have its own counter; the tests do not enable it.

## Known problems

- **`make sched-coop`'s deferral is measured in wall clock.** The design
  forces a critical section off the processor at the first watch tick at
  least 1 ms after the first strike, two ticks at most, and on the machine
  the book was measured on the longest deferral is 2.0 to 2.1 ms. The check allows 4000 us, and prints
  beside the deferral the boot's `stolen_ms` (the time its carriers spent off
  a processor while running kernelet code) and the longest gap between two
  watch ticks, because a tick that a busy host delivers late lengthens a
  deferral beyond two ticks.
- `make sched-fair` and `make sched-perf` can fail on a busy host. The
  common `sched-fair` failure is the spread: the check wants the sibling
  group's share to be the same, within 3%, whether the sandbox runs 1, 50, or
  50 tasks and an overstayer, and a busy host moves it by a few percent (3.1%
  has been seen). The rarer one is the share itself, outside 0.90 to 1.10 of
  a processor (0.8 has been seen). `sched-perf` fails on a busy host when its
  native reference, the round trip between two Linux processes, has no
  samples; a kernelet measurement without samples fails it too, but that is a
  fault of the scenario, not noise. They are measurements, which is why `make bench` does not
  fail on them; rerun them on a quiet machine.
- **The heap does not catch every bad free.** `mem_free` refuses a pointer
  that `mem_alloc` never handed out, and reads the block's number below it
  without trusting it (`get_kernel_nofault`), so a wild pointer is refused
  rather than an oops. It cannot catch a double free after `kmalloc` has
  handed the same address out again for a later block: the number and the
  address then match the new block, which is freed while the kernelet still
  uses it. Safe Rust cannot free twice; only a bug in vOSTD's unsafe code can,
  and such a bug could harm the host anyway.

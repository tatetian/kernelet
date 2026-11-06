# Kernelet Vsock evidence

The final series compares real virtio-vsock, two pipes, a tenant klet-vsock peer, and a direct-vOSTD API peer. `comparison-summary.txt` summarizes 504 validated measurements: seven repetitions at three sizes for four paths, in two boots for each of three notification policies. `final-{0a,1a,2a,2b,1b,0b}/` retain complete serial logs, outer-task counters, environment/configuration fingerprints, QEMU diagnostics, and build output. The execution order is the directory order just listed. Separate `profile-mode-{0,2}/` runs enable instrumentation; `profile-summary.json` extracts their diagnostic counts. These instrumented runs are not speed measurements.

Run from the repository root:

```sh
python3 prototype/tools/check-vsock-comparison.py prototype/bench/vsock/final-{0a,1a,2a,2b,1b,0b}
python3 prototype/tools/check-vsock.py prototype/bench/vsock/validation-mode-0.txt
python3 prototype/tools/check-vsock.py prototype/bench/vsock/validation-mode-1.txt
python3 prototype/tools/check-vsock.py prototype/bench/vsock/validation-mode-2.txt
```

To collect fresh data, use unique labels. Builds reuse the existing Linux tree; do not run two harnesses concurrently because their staging/build paths are shared. The runner requires KVM, vhost-vsock, host AF_VSOCK, and three available physical cores. The Make targets build the configured Linux kernel first. The comparison preserves the host's global configuration.

```sh
make -C prototype vsock-compare VSOCK_LABEL=new-0a VSOCK_OPT=0
make -C prototype vsock-compare VSOCK_LABEL=new-1a VSOCK_OPT=1
make -C prototype vsock-compare VSOCK_LABEL=new-2a VSOCK_OPT=2
make -C prototype vsock-compare VSOCK_LABEL=new-2b VSOCK_OPT=2
make -C prototype vsock-compare VSOCK_LABEL=new-1b VSOCK_OPT=1
make -C prototype vsock-compare VSOCK_LABEL=new-0b VSOCK_OPT=0
make -C prototype vsock-profile VSOCK_LABEL=new-profile-0 VSOCK_OPT=0
make -C prototype vsock-profile VSOCK_LABEL=new-profile-2 VSOCK_OPT=2
```

Mode 0 retains reserve/commit broadcasts on the original shared queue. Mode 1 suppresses reserve broadcasts only. Mode 2 additionally separates poll notification and gates blocking data-commit broadcasts on ring demand. Queue separation and gating are evaluated together. The experiment Makefile now defaults to mode 2; the manual broker module default and runner CLI default remain mode 0. Explicit mode arguments reproduce every retained run.

Each directional pipe/ring and each endpoint's advertised vsock receive credit is 64 KiB. Workload sizes are 4/16/64 KiB, with 64 warmup and 8,192 timed serialized round trips. Both-direction payload bytes count toward echo goodput. Every reply is fully verified and includes a changing sequence word. Latency columns are medians of per-case p50/p95 RTTs, not pooled percentiles. Normal comparison runs disable instrumentation and leave bounded-delay hooks at zero on benchmark endpoints.

The common Linux 6.12 guest caller is on guest CPU1/physical core1. Guest CPU0 and the pipe/klet/API peers use physical core0. The actual virtio peer is on outer Linux 6.8.0-100-generic/core0, alongside its vhost worker. QEMU housekeeping uses core2; these are distinct physical cores rather than SMT siblings. The device is modern PCI virtio-vsock with MSI-X. Effective affinity and delivery-count deltas confirm data interrupts on guest CPU1; they do not measure subsequent workqueue placement. Exploratory legacy-IRQ runs are excluded.

This arrangement makes the virtualization boundary real without nesting and keeps the caller/data cores common. Peer kernel version and peer placement inside/outside the VM still differ. It is a comparison of configured implementations, not a universal transport ranking. Governors were `powersave`; frequency, SMT-sibling activity, and unrelated host load were not controlled. Per-run metadata retains host load and affinities. The observed cross-size result is explicit: optimized klet loses to virtio at 4 KiB and wins at 16/64 KiB.

Endpoint CPU includes caller plus echo process, excluding asynchronous virtio backend work. Outer-task CPU sums QEMU-other, vhost, and outer echo/control process. Vhost is a thread in QEMU's TGID here, so it is excluded from QEMU-other before being added separately. Guest endpoint CPU overlaps the outer metric and must never be added to it. Neither metric is all-machine CPU. Outer windows include a small start-ACK/end-token interval, retained separately from guest wall time. Unchanged KVM `halt_poll_ns=200000` can consume idle CPU; there is no idle subtraction or claim of transport-only CPU. Virtio uses its data stream for control tokens; other paths use a separate vsock control stream.

The current source/config/binary hashes are recorded separately for each run. The subsequent Makefile-only selection of mode 2 changes that file's hash; all retained runs selected their policy explicitly. Code and kernel hashes establish which implementation was measured. The host and guest Linux kernel versions differ, so source audit of the Linux 6.12 reference is not dynamic copy-count instrumentation of the outer 6.8 backend.

`optimization-plan.txt` and the three `optimization-design-*.txt` reports predate notification implementation. `optimization-reviews.txt` records their conditions, subsequent inspection, and full-page reviews. `validation-mode-*.txt` are the shorter all-mode correctness runs. `final-regression.txt` records all 12 existing tests passing. `final-book-check.txt` and `final-book-build.txt` record successful book validation. `selected-default.txt` and `selected-default-serial.txt` retain the final mode-2 default check.

The initial, unpinned preoptimization experiment remains as `results.txt` and `serial.txt`, with its original two-round reviews in `reviews.txt`. It is historical evidence, not the matched mode-0 baseline. Its smaller correctness suite requires its archived checker:

```sh
python3 prototype/bench/vsock/check-initial.py prototype/bench/vsock/serial.txt
```

`historical-channels.txt` records the old book's ownership-transfer extension and provenance. Historical performance numbers are not reused as measurements of this implementation.


## Regenerating the proposal figures

The proposal embeds checked-in SVG figures generated with Matplotlib. Line plots compare transfer sizes; grouped bars compare categorical profiling diagnostics. Whiskers on goodput plots show observed minima/maxima, not confidence intervals. Latency percentiles are summarized per case; CPU scopes remain separate. No benchmark rerun is needed to redraw the figures.

From the repository root, install the plotting dependency into the ignored book cache and regenerate:

```sh
python3 -m pip install --target book/.cache/plot-deps -r book/tools/plot-requirements.txt
PYTHONPATH="$PWD/book/.cache/plot-deps" make -C book vsock-plots
make check
make build
```

`book/tools/plot-vsock.py` reads all 504 retained measurements directly for goodput, latency, CPU, and policy ablation. It reads `profile-summary.json` and the raw diagnostic logs for the profiling bars. The generator checks sample counts and preserves unrounded values before calculating medians and ratios. It writes five self-contained SVGs under `book/src/design-proposals/figures/klet-vsock/`, retaining selectable text, accessible descriptions, and deterministic IDs. Normal book builds use those SVGs without requiring Matplotlib.

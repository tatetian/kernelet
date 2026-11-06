# 2026-11-06 Benchmarking Sandbox Memory Efficiency and Returning Unused Frame Grants

**Status:** Under review  
**Submission type:** Documentation proposal backed by an experimental PoC  
**Evaluation date:** November 4, 2026  
**Document date:** November 6, 2026  
**Reference baseline:** `9cb55eccba6a0afe7ec162b286752ccc5ff267c8`  
**Affected chapters:** [The kernelet ABI](../accepted-design/the-kernelet-abi.md), [Tenant memory on Linux's page tables](../accepted-design/tenant-memory-on-linux-page-tables.md), and [What vOSTD adds on top](../accepted-design/what-vostd-adds-on-top.md).

## Summary

This proposal has two parts. The first defines a compact benchmark for sandbox memory lifetimes and fixed-budget queue completion. It combines real-program resource capture, Frame-lifetime replay, and synthetic queues to measure memory-time cost, allocation turnover, CPU cost, and completed work. The selected evaluation contains **five inputs, 18 policy replays, and nine queue configurations**.

The second introduces independently allocated and returnable **2 MiB extents** in the experimental Kernelet Frame allocator. Live Frames retain their physical frame numbers (PFNs). Compared with the grow-only policy in the same PoC, immediate return reduces the parent-cgroup memory-time integral by **80.59%** for the burst-and-release fixture and **41.16%** for the mixed-lifetime fixture with the primary layout. Under a **792 MiB global grant budget**, the fixed staggered queue completes **two sessions instead of one**. Persistent live objects and interleaved lifetimes limit the benefit; simultaneous live peaks remain constrained by the total budget.

The proposal documents the benchmark, implementation, measured tradeoffs, and next design steps. The experimental implementation remains outside the mainline and Accepted Design.

## Part I — Benchmark Design and Scoring

### 1. Where the workloads come from

The benchmark combines two real tool-action traces with three focused runtime workloads. The tool traces come from **Terminal-Bench 2.1 tasks and public Terminus2 action histories**. We execute the recorded commands in Linux containers and measure their memory use. R1–R3 are purpose-built programs run in a Linux VM to represent common sandbox memory lifetimes.

| Input | Source | Memory behavior represented |
|---|---|---|
| headless-terminal | Terminal-Bench task; recorded Terminus2 tool actions | Background processes and terminal interaction |
| openssl-selfsigned-cert | Terminal-Bench task; recorded Terminus2 tool actions | Short certificate-generation commands |
| R1 | Linux runtime fixture | Repeated allocation bursts, followed by release and idle periods |
| R2 | Linux runtime fixture | Persistent objects that remain live while execution is idle |
| R3 | Linux runtime fixture | A persistent memory base plus temporary allocation bursts |

These inputs cover both memory that becomes unused after a tool operation and memory that must remain allocated between operations. The benchmark measures how allocation policies handle that difference.

### 2. From real measurements to Frame replay

The evaluation follows three stages:

1. **Capture the source demand (A).** Run the recorded tool actions or runtime program and sample anonymous memory, file memory, and `memory.current` over time. Recorded minimum waits and 500 ms inter-action gaps are retained for the tool traces.
2. **Build a page-lifetime model.** Convert the anonymous/file-memory curves into demand for 4 KiB model pages. Rising demand creates pages; falling demand ends page lifetimes according to the selected release layout. Pages already live at the start are actually allocated before they are used.
3. **Replay in Kernelet (B).** A dedicated program performs real Frame allocation, access, mapping, unmapping, and release using that model. Replays preserve the requested byte amounts and time scale (`byte_scale=time_scale=1`). We record actual memory and CPU use for each policy.

The source programs run in Linux; Kernelet runs the reconstructed Frame workload. The model preserves the sampled demand amounts and their schedule, while page identities and placement follow explicit assumptions. It therefore evaluates the memory mechanism under source-derived demand, rather than reproducing the complete application's execution.

The primary layout uses **first-fit allocation and tail-drop release**: new pages use earlier available positions, and a reduction in demand releases pages from the tail of the model's allocated sequence. This tends to leave empty extents near the end. R3 also uses **64-page bands of interleaved long- and short-lived pages**. That layout can leave live pages in every extent after temporary demand ends. The comparison exposes how placement affects whole-extent return; the primary layout is not a lifetime-aware application allocator.

### 3. Policies and experiment setup

Each of the five inputs runs under three policies. R3 has one additional layout, giving **18 replay configurations and 12 paired comparisons**.

| Policy | Behavior after demand falls |
|---|---|
| Grow-only | Retain all capacity acquired during growth |
| Immediate | Return wholly empty added extents at legal safe points |
| Hysteresis | Return extents after an observed empty interval of 200 ms, retaining at most two added empty extents |

All policies use the same demand-driven growth path, source input, build, budget, and layout within each comparison. Each configuration has one observation; the ten operations inside each runtime fixture form one sample. The measured policy order remains in the run manifests.

The host is an AMD Ryzen 9 9950X3D running Linux 6.17.0-35-generic. B/C use QEMU/KVM with Linux 6.12, 2 GiB of RAM, and two guest vCPUs. Each Kernelet has one carrier and one vCPU. QEMU uses host CPUs 0/1 and observation uses CPU 4 on the shared host.

| Resource | B: policy replay | C: fixed-budget queue |
|---|---|---|
| Parent-cgroup budget | 768 MiB | 992 MiB |
| Grant budget | 256 MiB | 792 MiB globally |
| C instance limit | — | 1 GiB, also constrained by the global budget |
| Initial grant | 2 MiB | 2 MiB |
| Allocation/return unit | 2 MiB extent containing 4 KiB Frames | Same |

Memory cost is measured within the workload window, using actual timestamps and trapezoidal integration. Final instance destruction and artificial cleanup are excluded from return benefits. R2 retains its live objects through the end of that window. Parent-cgroup memory includes the measured instances and their associated management costs; grant accounting separately tracks physical blocks assigned to the Frame pool.

### 4. Testing fixed-budget completion

C uses two synthetic 45-second episodes that represent the R1 and R3 lifetime patterns. Both reach **474 MiB** during seconds 5–15. C-R1 then keeps only a sentinel page and its initial 2 MiB extent; C-R3 retains **118 MiB**. These amounts include the model's retained page.

The episodes run under a **792 MiB global grant budget** in three arrangements:

| Arrangement | Schedule | Minimum observation window |
|---|---|---:|
| Serial | Run C-R1, then C-R3 | 90 s |
| Staggered | Start C-R3 20 seconds after C-R1 | 65 s |
| Synchronized | Prepare both instances at low occupancy and release a common gate before their peak requests at second 5 | 45 s |

Three arrangements and three policies produce **nine configurations and 18 planned sessions**. We measure completed sessions, throughput, and memory-time cost per completion. A budget denial does not shorten the observation window. This tests whether returning unused memory helps later work enter a fixed-budget queue, and what happens when live peaks overlap.

### 5. Effects and scoring

The primary measure is **memory-time cost**: how much memory is held, and for how long. With memory measured in MiB,

$$
I=\int_{t_0}^{t_1}M(t)\,dt
\qquad\text{(MiB·s)}.
$$

We calculate it for both parent-cgroup memory (`I_parent`) and committed grant (`I_grant`). The former includes associated management costs; the latter isolates the Frame-pool capacity. Lower is better for the same completed workload.

For a paired policy comparison,

$$
\Delta I=\left(\frac{I_{policy}}{I_{grow}}-1\right)\times100\%.
$$

A negative change means lower cost. Equivalently, a **memory cost index** of $100I_{policy}/I_{grow}$ gives grow-only a score of 100; lower scores use less memory-time. We report completion and execution costs alongside this index rather than combine them into an arbitrary overall score.

| Metric | Calculation or measurement | Interpretation |
|---|---|---|
| Average and peak memory | $I/T$ and the largest sampled value | Separate sustained occupancy from peak demand and window length |
| Model utilization | Live-page memory-time / grant memory-time | Fraction of held grant used by the reconstructed live pages; higher indicates less unused capacity |
| Allocation turnover | Cumulative bytes allocated and freed | Reveals repeated allocation introduced by return; it is not peak memory saved |
| Execution cost | Parent CPU time and growth/trim stage time | Shows the processing cost accompanying memory savings |
| Queue completion and throughput | Completed/planned sessions; completed/observation seconds | Measures work served under the fixed budget |
| Cost per completion | Parent memory-time / completed sessions | Includes all queue attempts; read together with completion rate |

Live-page memory-time is calculated from the model's page counts, converted to MiB, and actual event times. It estimates replay utilization, not the source process's exact physical fragmentation. Growth and trim stage times include scheduling and Linux service time. Near-equal wall times can include different active CPU costs because the workload retains scheduled waits.

## Part II — Returnable Frame Grants: Design and Evaluation

### 6. Current allocation path

At the reference baseline, the Frame allocator receives one physically contiguous grant through `BootArgs`. A bitmap and Frame reference counts manage its 4 KiB pages. Dropping a Frame makes its page reusable within the allocator; returning that physical storage to Linux requires an additional ownership transition.

The experimental design replaces the single pool with a bounded registry of independently allocated **2 MiB extents**. An extent can be added when demand grows and returned once all of its Frames are free. The initial extent remains allocated until instance close. Live Frames never migrate.

The PoC changes 17 tracked files and adds 12 files relative to the reference baseline. This submission proposes the design and presents its evidence; it does not merge that implementation into the mainline. All performance comparisons switch policies within the same new PoC, rather than compare complete old and new repositories end to end.

### 7. Sources of memory cost

Three effects motivate the design and shape its tradeoffs:

1. **Retained peak capacity.** A pool that retains every acquired extent continues consuming memory after temporary demand ends. R1 isolates this case.
2. **Mixed lifetimes within an extent.** A few live Frames can prevent an otherwise mostly empty extent from being returned. R3's two layouts expose this constraint.
3. **Allocation and return turnover.** Returning memory can require new physical allocation, zeroing, and service calls when demand rises again. Immediate and delayed return trade this turnover against memory-time cost.

Persistent live memory, represented by R2, has no corresponding return opportunity. Idle execution alone does not make an extent reclaimable.

### 8. Proposed mechanism, implemented in the PoC

#### 8.1 Linux allocation and budget accounting

`linux/endovisor/grant.c` maintains committed and reserved bytes for both global and per-instance budgets. Under `budget_lock`, it checks that `committed + reserved + request <= limit`, reserves the request, and marks the extent `ALLOCATING`. It then allocates an order-9 physical block outside the lock using `GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NORETRY`.

On success, the extent becomes `LIVE` and reserved bytes become committed bytes. Allocation failure rolls back the reservation. Release checks the extent's `id + generation` and requires zero host mappings. It transitions through `LIVE → RETURNING → __free_pages → FREE`, removing committed accounting only after the actual free.

| State | Meaning | Transition |
|---|---|---|
| `FREE` | Slot owns no physical extent | Reserve budget and enter `ALLOCATING` |
| `ALLOCATING` | Budget is reserved while Linux allocation runs | Commit as `LIVE`, or roll back to `FREE` |
| `LIVE` | Extent owns an allocated physical block | After identity and mapping checks, enter `RETURNING` |
| `RETURNING` | Extent is being returned to Linux | Complete the physical free, remove accounting, and enter `FREE` |

Query 11 counts physical allocation failures; query 12 counts budget denials. These counters distinguish policy admission from inability to allocate physical RAM. A successful return demonstrates release to Linux; reuse of the same PFN by another instance is a separate observation.

#### 8.2 Rust registry and empty-extent ownership

Each entry in `vostd/src/mm/frame_allocator.rs` records its base, generation, initial flag, state, a bitmap for 512 pages, and reference counts. The last reference reaching zero and the corresponding bitmap bit being cleared occur under the same registry lock.

An extent is eligible for return only when its used count, bitmap, and reference counts are all zero, and it is not the initial extent. The allocator first removes it from the locally allocatable set under the lock, then calls the release service outside the lock. Success removes local ownership; failure restores availability. This prevents allocation from an extent while it is being returned and preserves live Frame PFNs.

#### 8.3 Explicit capacity operations and legal sleeping context

`mm/capacity.rs` exposes `configure`, `start`, `ensure`, `tool_start`, `tool_end`, `maintain`, and `stop`. Capacity operations require a current Task, no upcall context, `guard_count=0`, enabled IRQs, and `R2held=0`. `OP_BUSY` serializes operations. An interrupt-disabling spinlock is not held across a Linux service that may sleep.

Actual allocation demand triggers growth through `ensure` at a legal safe point. Ordinary Frame allocation does not implicitly perform blocking growth. `ensure_layout_prefix` supports the model layout used by replay; it is not an automatic application memory-scaling interface.

The maintenance task performs due returns when `active_tools=0`; it does not initiate demand-independent growth. A timer callback only marks work pending and wakes that task. Hysteresis measures 200 ms from the first observation of an empty extent and retains at most two added empty extents. Scheduling and legal-context checks can delay the return beyond the nominal timeout.

#### 8.4 Mapping references, page tables, and shutdown

In `space.c`, `grant_zap` calls `zap_vma_ptes` and completes synchronous TLB invalidation before reducing host mapping pins. Rust mappings separately hold Frame references, released after the PTE-removal, TLB, and RCU conditions are satisfied. Host pins and Rust references are checked independently; an extent becomes returnable only after the last relevant Frame reference is gone.

`stop` marks the controller as stopping, prevents new operations, wakes maintenance, and waits for `RUNNING=false` and the manager's weak-reference `strong_count=0`. It then returns eligible added extents. The initial 2 MiB remains until instance close; zero grant accounting is checked after close, not attributed to `capacity.stop` alone.

The 2 MiB extent is an allocation and return unit. `VmSpace` continues using 4 KiB mappings in the existing Linux page-table arrangement. Watch/mirror remains enabled. The evaluation does not change the syscall path, disable mirror, compare PVM, or measure a huge-page/TLB optimization.

The measured source tree is under `driver-engine/template/source/prototype/` in the experiment archive.

### 9. Results under identical configurations

G, I, and H denote grow-only, immediate, and hysteresis. Layout 0 is first-fit/tail-drop; layout 1 interleaves long- and short-lived pages in 64-page bands. All values below come from the retained rows of `B-business.csv`, selected by `(task_id, layout, policy)`.

#### 9.1 Memory-time cost, peak, and estimated utilization

| Input | Layout | Policy | Parent integral (MiB·s) | Grant integral (MiB·s) | Peak grant (MiB) | Estimated live/grant (%) |
|---|---:|:---:|---:|---:|---:|---:|
| R1 | 0 | G | 833.726 | 798.252 | 76 | 13.86 |
| R1 | 0 | I | 161.860 | 126.430 | 76 | 59.69 |
| R1 | 0 | H | 330.590 | 294.895 | 76 | 20.24 |
| R2 | 0 | G | 720.992 | 689.719 | 76 | 97.75 |
| R2 | 0 | I | 722.495 | 689.545 | 76 | 97.76 |
| R2 | 0 | H | 752.941 | 718.682 | 76 | 97.77 |
| R3 | 0 | G | 1714.965 | 1662.093 | 150 | 51.51 |
| R3 | 0 | I | 1009.027 | 956.202 | 150 | 94.34 |
| R3 | 0 | H | 1160.157 | 1108.298 | 150 | 77.04 |
| R3 | 1 | G | 1718.692 | 1665.360 | 150 | 51.34 |
| R3 | 1 | I | 1716.839 | 1663.437 | 150 | 51.42 |
| R3 | 1 | H | 1719.416 | 1665.462 | 150 | 51.35 |
| headless-terminal | 0 | G | 2764.495 | 2653.180 | 92 | 89.52 |
| headless-terminal | 0 | I | 2547.896 | 2439.809 | 92 | 97.21 |
| headless-terminal | 0 | H | 2649.906 | 2537.404 | 92 | 93.64 |
| openssl-selfsigned-cert | 0 | G | 342.611 | 328.472 | 88 | 91.96 |
| openssl-selfsigned-cert | 0 | I | 325.582 | 311.771 | 88 | 96.88 |
| openssl-selfsigned-cert | 0 | H | 337.874 | 323.084 | 88 | 93.48 |

#### 9.2 CPU, turnover, and synchronous stage times

Allocated/freed values cover the workload accounting window. Final cleanup frees are excluded from this table's return benefits.

| Input | Layout | Policy | Parent CPU (s) | Allocated/freed (MiB) | Synchronous growth (ms) | Synchronous trim (ms) |
|---|---:|:---:|---:|---:|---:|---:|
| R1 | 0 | G | 1.304262 | 76/0 | 19.248 | 0.072 |
| R1 | 0 | I | 1.575832 | 724/720 | 39.629 | 2.872 |
| R1 | 0 | H | 1.320783 | 688/612 | 620.968 | 8.034 |
| R2 | 0 | G | 0.267644 | 76/0 | 3.838 | 0.066 |
| R2 | 0 | I | 0.266517 | 76/0 | 5.317 | 1.647 |
| R2 | 0 | H | 0.913107 | 76/0 | 4.085 | 1.631 |
| R3 | 0 | G | 1.474079 | 150/0 | 439.032 | 0.090 |
| R3 | 0 | I | 1.497463 | 816/740 | 43.313 | 3.147 |
| R3 | 0 | H | 1.002445 | 780/700 | 46.525 | 9.110 |
| R3 | 1 | G | 1.481638 | 150/0 | 22.734 | 0.093 |
| R3 | 1 | I | 1.498553 | 150/0 | 23.126 | 2.198 |
| R3 | 1 | H | 0.953300 | 150/0 | 23.094 | 2.166 |
| headless-terminal | 0 | G | 1.660382 | 92/0 | 7.088 | 0.156 |
| headless-terminal | 0 | I | 2.106779 | 156/86 | 12.138 | 3.834 |
| headless-terminal | 0 | H | 2.281613 | 114/40 | 7.502 | 3.971 |
| openssl-selfsigned-cert | 0 | G | 0.162781 | 88/0 | 5.035 | 0.029 |
| openssl-selfsigned-cert | 0 | I | 0.148314 | 106/36 | 5.910 | 0.702 |
| openssl-selfsigned-cert | 0 | H | 0.163509 | 88/14 | 6.714 | 0.739 |

#### 9.3 Paired changes in the parent integral

| Input / layout | Immediate | Hysteresis |
|---|---:|---:|
| R1 / 0 | −80.585902% | −60.347870% |
| R2 / 0 | +0.208349% | +4.431237% |
| R3 / 0 | −41.163410% | −32.351009% |
| R3 / 1 | −0.107859% | +0.042095% |
| headless-terminal / 0 | −7.835012% | −4.145005% |
| openssl-selfsigned-cert / 0 | −4.970552% | −1.382523% |

These changes come from `B-effects.csv` at the original precision, rather than ratios recomputed from rounded display values.

### 10. Analysis and tradeoffs

#### 10.1 Burst release lowers memory-time cost at the price of turnover

For R1, immediate return reduces the parent integral by 80.59% while the peak grant remains 76 MiB. Cumulative allocation rises from 76 MiB to 724 MiB. Hysteresis lowers that turnover to 688 MiB, but retains more memory over time than immediate return. The mechanism removes capacity after demand falls; it does not reduce the peak required by the workload.

The real-task-derived replays also show lower parent integrals under immediate return: 7.84% for headless-terminal and 4.97% for openssl-selfsigned-cert. These observations apply to the reconstructed Frame lifetimes from their recorded source traces.

#### 10.2 Lifetime layout limits whole-extent return

For R3, immediate return reduces the parent integral by 41.16% with the primary layout and by only 0.108% with the interleaved layout. In the latter, all three policies allocate 150 MiB and perform no workload-period physical frees. Even with an estimated live/grant ratio near 51%, long-lived pages prevent extents from becoming wholly empty.

This result motivates lifetime-aware placement: free page count alone is insufficient when return requires an entirely empty extent.

#### 10.3 Persistent objects provide no workload-period return opportunity

R2 allocates 76 MiB and frees none during the workload under all three policies. Its hysteresis parent integral is 4.431% higher than grow-only. The measured hysteresis window is **9.497418 s**, compared with **9.116260 s** for grow-only, an increase of about **4.181%**. Average parent memory is **79.2785 versus 79.0886 MiB** (+0.240%), and average grant is **75.6713 versus 75.6582 MiB** (+0.017%).

The integral increase primarily accompanies the longer actual window. It is not evidence of allocating or retaining two additional spare extents. The causes of the window and CPU differences have not been profiled. These figures are calculations from the existing B rows, not additional experiments.

#### 10.4 Execution costs require separate interpretation

Headless-terminal's lower parent integral accompanies higher parent CPU time under both return policies in this run. The measured synchronous growth stages of **620.968 ms** for R1 hysteresis and **439.032 ms** for R3 grow-only are retained as observed.

Parent CPU includes management work; growth-stage measurements include scheduling and service time. One observation per configuration cannot establish a stable CPU ordering or attribute these differences to mirror, page tables, or the allocator in isolation. The results support a memory-time/turnover tradeoff rather than a general speedup claim.

### 11. Fixed-budget queue results

`C-points.csv`, keyed by `(arrangement, policy)`, supplies the following results. The parent-integral numerator includes all attempts in the fixed queue.

| Arrangement | Policy | Completed / 2 | Denied | Observation (s) | Parent integral per completion (MiB·s) | Completed types |
|---|:---:|---:|---:|---:|---:|---|
| serial | G | 2/2 | 0 | 90.107591 | 19766.612517 | C-R1; C-R3 |
| serial | I | 2/2 | 0 | 90.085400 | 7367.409843 | C-R1; C-R3 |
| serial | H | 2/2 | 0 | 90.093514 | 7578.154962 | C-R1; C-R3 |
| stagger | G | 1/2 | 1 | 65.001770 | 20129.675487 | C-R1 |
| stagger | I | 2/2 | 0 | 65.064801 | 7370.055091 | C-R1; C-R3 |
| stagger | H | 2/2 | 0 | 65.071701 | 7563.321804 | C-R1; C-R3 |
| sync | G | 1/2 | 1 | 45.110476 | 20121.340061 | C-R3 |
| sync | I | 1/2 | 1 | 45.105059 | 9485.774858 | C-R3 |
| sync | H | 1/2 | 1 | 45.111782 | 9659.915522 | C-R3 |

Across the nine configurations, **14 sessions complete and four receive normal budget denials**. Each denied attempt records `budget_denials=1` and `allocation_failures=0`.

With staggered peaks, returning capacity admits the later demand and increases completed sessions from one to two within an approximately 65-second observation window. With synchronized peaks, every policy completes C-R3 and denies C-R1. The two peak demands sum to **948 MiB**, exceeding the **792 MiB** grant budget. Returning unused memory cannot fit two simultaneously live peaks that exceed that budget.

The same completed type under synchronized execution still incurs less memory-time cost with return enabled. These fixed points demonstrate queue outcomes and a budget boundary, not the maximum number of AI sandboxes the system can host.

### 12. Design implications and future work

The measured mechanism provides a useful base: stable PFNs, explicit legal capacity operations, and physical return of wholly empty extents. Immediate return is effective when lifetimes create empty extents. The current hysteresis setting reduces some repeated allocation while consuming more memory-time; the data do not select a universally optimal policy.

The following candidates are **not implemented or measured** in this proposal:

1. **Lifetime-aware placement.** Separate persistent and temporary allocations to reduce the whole-extent fragmentation exposed by R3's interleaved layout.
2. **Empty-extent candidate sets and event-driven maintenance.** Replace unnecessary registry scans with targeted work. The current measurements do not identify scanning as the cause of R2's CPU increase.
3. **Pressure-aware spare retention and batched release.** Drain spare extents when budget is tight. Explore batching PTE removal, TLB invalidation, and RCU-related release while preserving reference and visibility ordering.

Transparent swap, live-page migration, and page-table-page reclamation remain outside the implemented mechanism. The next design decisions should address the measured lifetime and turnover constraints.

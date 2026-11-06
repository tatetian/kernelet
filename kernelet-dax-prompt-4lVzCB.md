# Host page-cache reuse and page metadata: Linux, microVMs, and kernelets

## Intent and deliverable

I want to understand whether kernelets can reuse the host page cache with fewer page faults and less page metadata than microVMs, and where any advantage survives improvements to Linux DAX.

Write an English article explaining how Linux reduces the physical memory consumed by `struct page`, distinguishing established mechanisms from ongoing work. Use that explanation to evaluate a possible kernelet design for sharing immutable image layers. Save the article as `better_dax.md` at the repository root.

Touch the output article MD file (`better_dax.md`) only. Do not modify any other files. Do not Git commit. Other work is ongoing in this repository; preserve all unrelated changes. These restrictions apply to review agents as well: they must report findings without editing files.

This is a research and analytical writing task. Do not implement the proposed device, run new benchmarks, or modify the book or prototype. Read the repository instructions, accepted design, and relevant prototype code to ground the analysis, but do not treat the current design as a constraint: major architectural revisions are welcome.

## Proposed kernelet scenario

Consider a hypothetical `klet-pmem` device attached to a kernelet. Its purpose is to let the kernelet maintain a page cache using references to host page-cache pages directly, and map those same pages into tenant userspace without copying their contents or creating another full set of page descriptors for the device's advertised capacity.

The desired access path explicitly acquires the backing pages and installs actual userspace page-table entries before userspace accesses the mapped range. For correctly permitted accesses to prepared mappings whose backing pages remain resident, the goal is zero mapping faults, including on first touch. Count the acquisition, I/O, metadata, synchronization, and page-table preparation costs separately; moving work out of a fault handler does not eliminate that work. Distinguish CPU page faults from explicit I/O, blocking, TLB misses, and hardware page-table walks.

Treat this mechanism as a design hypothesis to investigate, not an implemented feature or a conclusion to defend. Explain what sharing the host kernel address space enables and what still requires explicit tenant mappings, lifetime management, and host validation.

## Required technical coverage

Build from familiar concepts to abstractions and then mechanisms:

- Explain physical page frames, PFNs, `struct page`, compound pages, head and tail pages, folios, and vmemmap. Distinguish the data pages being described from the physical pages storing their descriptors.
- Explain how vmemmap preserves descriptor virtual addresses while changing their physical backing. Show why PFN conversion still works, what metadata can be shared, and how independent writable metadata must be restored when required. Use a worked 2 MiB example with 4 KiB base pages and 64-byte descriptors, labeling those assumptions and any version-dependent details.
- Distinguish huge page-table mappings, compound-page management, and physical vmemmap savings. Explain why a large folio or PMD mapping alone does not imply fewer descriptor bytes.
- Explain relevant established optimizations, including HugeTLB vmemmap optimization and Device DAX metadata deduplication. Establish their applicability and limitations for filesystem DAX, virtio-pmem, and virtio-fs rather than generalizing from their names.
- Examine relevant ongoing work on reducing or deferring FS-DAX metadata allocation. Distinguish actual physical-memory savings from moving metadata onto the device, reserving virtual space, or merely deferring initialization. Explain whether savings follow advertised capacity, previously accessed ranges, or the current live working set.
- Explain the virtio-fs shared-memory window and its capacity-versus-reclamation trade-off. Distinguish window allocation granularity from huge mappings, and contrast a reusable window with a virtio-pmem device address range.

Focus on mechanisms relevant to immutable image layers rather than exhaustively surveying every Linux memory optimization. Clarify which bytes are shared: host image-file bytes versus the contents of files within the image. Identify layout, alignment, or compression constraints that prevent direct reuse.

## Reclamation is an open design question

Host reclamation of shared image pages is a design goal. Explore whether co-designing vOSTD's mapping APIs with the host could support safe revocation, including removing tenant mappings and coordinating in-flight access before releasing backing pages. Do not assume that the current prototype already provides this protocol.

Investigate both policies without choosing one in advance:

1. **Refault after eviction:** prepared resident mappings are fault-free until revoked; subsequent access may fault to restore them.
2. **Restore before execution:** suspend affected execution and restore the necessary pages and mappings before resuming. Analyze whether this can preserve useful reclamation, bound the resident set, and avoid faults without excessive preparation or suspension costs.

Explain the limits of an absolute zero-fault guarantee under reclamation and unpredictable userspace access. Explicitly distinguish a revoked mapping from a prepared, valid mapping. If the goals conflict, state the conflict rather than assuming it away.

## Evidence and quantitative comparison

Use primary sources: Linux documentation and source, relevant patch discussions and commits, virtualization documentation, and the DeepSeek Elastic Compute (DSec) report. Verify the report's claims rather than treating this conversation as evidence. Verify the applicability of mechanisms described in the virtio-fs article at <https://developer.aliyun.com/article/863752>.

State the research date and pin important implementation claims to identifiable releases or commits. For ongoing work, report the latest verified revision and whether it is proposed, merged into a development tree, or released. Cite sources beside the claims they support. Clearly label established behavior, code-derived inference, proposed mechanisms, and expected benefits.

Compare kernelets with both established microVM DAX configurations and plausible optimized configurations, including applicable ongoing metadata work and eager mapping or prefaulting. Apply comparable preparation and residency assumptions; do not assume that fault-free prepared access is unique to kernelets.

Use analytical estimates only. Derive formulas and worked examples varying device or window capacity, accessed and live mapped bytes, and sandbox count. Keep the following costs separate:

- Shared data and remaining host page metadata.
- Per-guest device metadata and any guest page-cache duplication.
- Tenant page tables and, for microVMs, relevant guest and second-stage mappings.
- Kernelet cache indexes, references, ownership and revocation records, and other required bookkeeping.

Distinguish system-wide metadata already provisioned for host RAM from incremental metadata caused by another sandbox. Give conditional savings or bounds where possible; do not turn byte counts or fault counts into unsupported latency or throughput claims. Identify scenarios where sparse access, reclamation, or improved microVM mechanisms narrow or remove the advantage.

## Writing requirements

Write for a reader with operating-systems course knowledge and C/Rust familiarity, without assuming familiarity with Linux memory-management internals.

- Keep each section and paragraph focused on one topic, and make titles cover their sections.
- Begin paragraphs longer than three sentences with a summary sentence.
- Introduce required background locally, with links for deeper material. Follow applicable repository conventions for comments and background explanations.
- Move from familiar concepts to abstractions and then mechanisms. Break complex mechanisms into manageable steps.
- Use concrete examples, diagrams, and short interface snippets. Label proposed interfaces and pseudocode clearly, and distinguish them from existing implementation excerpts.
- Be concise and distinguish evidence from expectations.

Conclude with a verdict on whether kernelets have a chance to do better, how much, and where. Separate demonstrated architectural capabilities from proposed extensions and unresolved questions. State the conditions behind each claimed advantage, what cannot be eliminated, and the work required to realize the proposed design. A negative or conditional verdict is acceptable if supported by the analysis.

## Review and iteration

Use independent review agents to assess technical correctness and source/version accuracy, the fairness and feasibility of the kernelet-versus-microVM comparison, and clarity for the intended reader.

Have reviewers read the complete article and inspect the sources or repository code relevant to their findings. Revise the article and repeat review until no unresolved correctness issues or material clarity problems remain and the review agents are satisfied. Preserve and explain genuine uncertainties instead of resolving them through unsupported assertions. Report any remaining limitations when delivering the article.

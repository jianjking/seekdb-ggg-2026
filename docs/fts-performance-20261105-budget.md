# FTS performance budget and experiment gates

TASK_STATUS=INCOMPLETE

No official full-score result has been received. This is a recovery and research
record, not a claim that the competition target has been achieved.

## Protected state and attribution

Source baseline: `adc98d3a6b833df1f64f6d5e3775816adf3f59ac`.
Workspace and branch remain `/home/xiaob/seekdb-fts-linux`,
`competition/fts-opt-v3`. No push is authorized.

The complete pre-budget patch is saved outside the repository at
`/home/xiaob/fts-validation-20261105/checkpoint-before-budget-20261105.patch`.
No earlier files or experimental binaries were deleted.

## Current verified candidate versus original

The current source is adc plus the BMW correctness fix and a single-worker
FTS doc-word queue in mini mode. It contains none of the rejected sorting,
direct-merge, typed-property/galloping or document-batch experiments.
The general mini-mode DDL queue remains single-threaded; non-mini mode is unchanged.
The full scoped build linked successfully in **173.080 seconds**.
Frozen binary: `/home/xiaob/fts-validation-20261105/bin/seekdb-fts-aux-queue`.
SHA-256: `6e8f0f6c89da0c8a07facf95952a9da21061c2fb2672754006a34aae3f31e140`.

Four complete AB/BA pairs against the original, on CPUs 0-3 and with every
official parameter unchanged, produced:

| Official metric | Unit | Original mean | Current mean | Reduction |
| --- | --- | ---: | ---: | ---: |
| build_ik_all_sec | s | 5.838000 | 3.702000 | +36.59% |
| build_ik_content_sec | s | 4.383750 | 2.504000 | +42.88% |
| build_beng_en_sec | s | 2.502750 | 1.430500 | +42.84% |
| tokenize_ik_avg_ms | ms | 0.130800 | 0.129050 | +1.34% |
| tokenize_beng_avg_ms | ms | 0.096400 | 0.092875 | +3.66% |
| query_cn_avg_ms | ms | 1.192075 | 1.186075 | +0.50% |
| query_beng_avg_ms | ms | 1.744025 | 1.749050 | -0.29% |
| query_mixed_avg_ms | ms | 1.351775 | 1.386550 | -2.57% |
| query_limit_avg_ms | ms | 1.586900 | 1.576350 | +0.66% |

Build **40.77023%**, TOKENIZE **2.49728%**, query **-0.42313%**;
equal-category reduction **14.28146%**. The descriptive four-pair interval
is 13.48794%-14.98791%. This is a local measurement, not an official result.
The 50% local target is still **35.71854 percentage points** away.

Report: `/home/xiaob/fts-validation-20261105/original-fts-aux-queue-ab/`.
Both executables' unaffected oracles matched. The candidate passed strict
Token/TOKENIZE options, term counts, full MATCH IDs, independent BM25, sorting,
LIMIT prefixes and large generated-output boundary checks. The original's
two reproduced BMW 4016 failures are explicitly recorded as baseline defects.
Official file hashes and executable hashes were unchanged during testing.

Independent scoped-queue versus adc+BMW comparison:
`/home/xiaob/fts-validation-20261105/fts-aux-queue-isolated-ab/`.
Build reduction **37.76995%**, TOKENIZE **4.30638%**, query **1.55194%**,
overall **14.54276%**, descriptive interval 11.94518%-16.87782%.
Only the build contribution, **12.58998 points**, is causally assigned to
scheduling; TOKENIZE and query changes are not credited to this mechanism.

| Change | Classification | Evidence | Decision |
| --- | --- | --- | --- |
| BENG ownership and reusable document map | Committed performance work | Previous report; not an independent original-source comparison | Preserve |
| FTS-only mini-mode doc-word worker | Verified isolated structural optimization | Scoped isolated build gain 37.76995%; exact original/candidate overall 14.28146% | Preserve; static concurrency bound of two across the ordinary and FTS-only queues |
| Direct merge for <=8 dimensions | Rejected isolated experiment | Independent eight-round comparison: -3.18052% equal-category reduction | Removed; full patch and binaries preserved |
| Interval dictionary sort | Rejected isolated experiment | Eight complete rounds: -3.77315% equal-category reduction | Removed; full patch and binaries preserved |
| Non-root prefetched index-block exhaustion | Correctness fix | Strict Top-K oracle passes; original and adc reproduce error 4016 | Preserve separately; claim no performance gain |
| Typed properties plus galloping search | Rejected combined experiment | Eight rounds versus interval sort: -1.39371% | Both performance changes reverted; binaries and reports retained |
| FTS document-output batches | Rejected minimal experiment | Full link passed in 489.162 s; candidate disconnected on its first BENG index DDL in strict regression | Removed from candidate; preserve the 65-line experiment rather than expand an unproven path |
| Local regression / A/B harness | Validation support | Not part of official scoring inputs | Keep distinct from kernel changes |
| Temporary diagnostics | Diagnostic only | Frozen diagnostic binaries outside repository | Do not merge into candidate |

The initial new batch-boundary fixture incorrectly treated `and` as a stopword.
The built-in stopword list does not contain it. The reference version therefore
correctly used 2057 tokens, not 2056. The local oracle was corrected; no official
test, scoring file, SQL, timer, or workload size was changed.

The batch experiment patch is
`/home/xiaob/fts-validation-20261105/fts-batch-experiment.patch`. Its failing run
is `fts-batch-ab-v2/`. No nine-metric performance result exists for it.
After its removal, the safe source linked successfully in 181.463 s,
with build log `build-safe-structural.log`. The original/safe eight-round
comparison is `original-safe-structural-ab/`.

## Scoring and Amdahl model

### Historical combined candidate: not the current source

Both executables were frozen and SHA-256 checked. The original comes from
`5bce9e06d1281347ad61344e4b01fab9158c58a2`, with build adaptations only.
The safe candidate contains adc, direct merge, dictionary sorting and the
prefetch correctness fix. It contains neither the batch experiment nor the
reverted typed-property/galloping changes.

| Official metric | Unit | Original mean | Safe mean | Reduction |
| --- | --- | ---: | ---: | ---: |
| build_ik_all_sec | s | 6.678750 | 5.690000 | +14.80% |
| build_ik_content_sec | s | 4.633500 | 4.083500 | +11.87% |
| build_beng_en_sec | s | 2.557250 | 2.239750 | +12.42% |
| tokenize_ik_avg_ms | ms | 0.170650 | 0.148775 | +12.82% |
| tokenize_beng_avg_ms | ms | 0.111275 | 0.111600 | -0.29% |
| query_cn_avg_ms | ms | 1.222525 | 1.073800 | +12.17% |
| query_beng_avg_ms | ms | 1.796275 | 1.556725 | +13.34% |
| query_mixed_avg_ms | ms | 1.407225 | 1.215400 | +13.63% |
| query_limit_avg_ms | ms | 1.633625 | 1.609475 | +1.48% |

Category reductions: build **13.0301%**, TOKENIZE **6.2633%**, query **10.1528%**.
Equal-category reduction: **9.81537%**. This is local, not official scoring.
One original build-IK-all trial took 8.713 s, while the other original trials
were 5.952-6.093 s. All valid trials remain included; none were discarded.
The descriptive four-pair bootstrap interval is approximately 4.46-13.64%;
four pairs are too few to eliminate host drift or establish portability.

The candidate passed strict regression, including the 2051-distinct-term
document crossing batch boundaries, NULL/empty documents, independent BM25,
multi-term queries and exact ranked Top-K prefixes. The original's two known
BMW error-4016 failures are recorded separately, not declared passing.
The unaffected oracle JSONs match. All nine metrics and hit counts passed.

Raw data: `/home/xiaob/fts-validation-20261105/original-safe-structural-ab/`.
`summary.json` and `budget-summary.json` include means, ranges, weights,
individual weighted contributions and the complete calculation.

Let `T0_i` and `T1_i` be same-environment original and candidate mean elapsed times.
The elapsed-time reduction is `r_i = 1 - T1_i / T0_i`.
The official category weights of each metric are:

| Metrics | Count | Weight per metric |
| --- | ---: | ---: |
| Three build durations | 3 | 1/9 |
| Two TOKENIZE durations | 2 | 1/6 |
| Four MATCH durations | 4 | 1/12 |

The overall reduction is `R = sum(weight_i * r_i)`. Raw milliseconds across
categories must not be summed to calculate the score.

For an optimization affecting a measured critical-path elapsed fraction `p_i`,
with achievable component speedup `s_i`:

`r_i = p_i * (1 - 1/s_i)`, `speedup_i = 1 / (1 - r_i)`.

The component's infinite-speed ceiling is `p_i`; its overall contribution ceiling
is `sum(weight_i * p_i)`. Fractions of disjoint work may be added. Nested parent
and child fractions may not be added. Overlapping candidates require ablation
or a union-of-work estimate; multiplying or adding their standalone gains is
not automatically valid.

A complete category eliminated entirely contributes only 33.33 percentage points.
One MATCH metric contributes at most 8.33 points, and one TOKENIZE metric at most
16.67 points. These are scoring ceilings, not predictions.

### What existing sampling does and does not establish

The latest diagnostic profile ran the unchanged official workload on
`seekdb-typed-clean`. Its timing is excluded from all A/B tables.

| Sampled phase | Process CPU seconds | Phase wall seconds |
| --- | ---: | ---: |
| Build | 30.17 | 20.2397 |
| TOKENIZE | 1.87 | 2.6391 |
| Query | 3.77 | 4.9423 |

These are broad phase windows, not the nine scoring intervals. Build CPU time
exceeds wall time because workers overlap. Sampling is user-space CPU only.
The elapsed/CPU ratios cannot be distributed among the nine metrics without
additional measurement.

Build inclusive samples include single-row-to-batch adaptation at 67.04%,
segmentation at 21.16%, and sort at 10.51%. Adaptation contains child execution;
**67.04% is not removable adapter cost**. The child fractions must not be added
to it. Query inclusive samples include direct DAAT at 49.55%, advance at 9.29%,
and filtering at 7.87%. These likewise do not directly supply elapsed fractions.
No precise allocation or function-call count is claimed from sampled percentages.

Sources: `/home/xiaob/fts-validation-20261105/structural-typed-clean/`,
including `phases.json` and the per-phase perf recordings.

## Ranked hypotheses and gates

The following `p` values are **required thresholds**, not measured fractions or
fabricated forecasts. Until an elapsed fraction or a minimal ablation is
available, the defensible gain interval includes zero and possible regression.

| Priority / candidate | Affected scoring metrics | Infinite-speed scoring ceiling | Conditional achievable budget | Cost / risk / gate |
| --- | --- | --- | --- | --- |
| 1. Generated FTS rows returned in batches | All three build metrics | 33.33 points; actual ceiling is unknown `p_build/3` | If removable wall fraction is >=22.5% and that component becomes 3x faster: >=5 points | Minimal experiment failed correctness and was removed. Further work requires an independently demonstrated budget and a supported DAS batch boundary |
| 2. Reuse one tokenization result for inverted/doc-word construction | Three build metrics, potentially shared parsing work | 33.33 points; duplicate parsing only is much smaller | To contribute 5 points with a 2x component improvement requires >=30% build elapsed coverage | High coordination/storage cost. First prove duplicate parse count and critical-path time; otherwise do not implement |
| 3. Bulk posting merge / projection, preserving scores and Boolean logic | CN, BENG, mixed MATCH; possibly LIMIT | 25 points for first three, 33.33 with LIMIT | For 6 points over all four metrics at 4x component speed requires >=24% elapsed coverage | Medium/high risk. Count decoded postings, comparisons and projected rows; require a small algorithm experiment first |
| 4. Build scheduling / pipeline parallelism | Three build metrics | 33.33 points | For 10 points at 2x component speed requires >=60% parallelizable critical-path coverage | High risk/cost; CPU headroom is not proof. Measure task overlap and waiting before changing scheduling |
| 5. Stronger exact Top-K block bounds / early termination | LIMIT MATCH; possibly explicitly ranked queries outside scoring | 8.33 points | For 5 points at 4x component speed requires >=80% LIMIT elapsed coverage | Medium/high risk. Existing plan has implicit relevance DESC sorting. Do not remove ordering or return arbitrary hits |
| 6. Further interval/radix sorting | Three build metrics only | 33.33 points globally; residual sort coverage unknown | To add 5 points at 3x needs >=22.5% remaining build elapsed coverage, not supported by current 10.51% inclusive CPU evidence | Lower priority. Measure an independent ablation; do not extend to doc-word blindly |
| 7. More token-buffer/hash tweaks or filter-call tweaks | One or several metrics | Depends on actual covered work | No credible material overall budget established; last combined tweak regressed | Reject expansion without new evidence |

For small experiments, retain a performance candidate only after strict
correctness and a positive complete nine-metric equal-category A/B result.
Zero or negative results are not a reason to broaden the implementation.
A positive number within environment noise needs additional validation and
must not be promoted to a guaranteed gain.

## Combined target feasibility with the current candidate

No measured combination currently establishes a credible >=50% path.
The current candidate's original-source A/B measured 14.28146%, leaving
**35.71854 percentage points** to the 50% local target. A uniform reduction
of the remaining candidate times would have to be **41.66956%**.

If TOKENIZE remains at its current gain and build/query have equal target
reductions, build and query would each need 73.75136% reductions relative to
original to reach 50% overall. That requires another approximately 55.68336% of
current build time and 73.86196% of current query time. Small sorting, hash,
property or per-comparison changes do not establish such a path.

For perspective, a hypothetical combination of build 65%, TOKENIZE 35%, and
query 50% elapsed reductions yields exactly 50% overall. These are target
budgets, not measurements. They require large architecture changes or reduced
necessary work in all three categories. Current sorting, direct merge and
property parsing evidence does not justify those assumptions.

Even build 50% plus query 50% with TOKENIZE unchanged yields only 33.33%.
Therefore build-only or query-only research cannot by itself establish the
requested outcome. The next decision must use original A/B, an isolated batch
experiment, and metric-specific execution work, rather than the size of a
hotspot label.

## Independent dictionary-sort gate: rejected

Both executables contain the same direct merge and BMW correctness fix. Only
dictionary sorting differs. Both passed the strict regression oracle. The full
eight-round AB/BA comparison used unchanged official parameters and kept every
valid trial, including slow trials.

| Metric | Without dictionary sort | With dictionary sort | Reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 6.338750 | 7.287000 | -14.96% |
| build_ik_content_sec | 4.585500 | 4.438000 | +3.22% |
| build_beng_en_sec | 2.478750 | 2.355250 | +4.98% |
| tokenize_ik_avg_ms | 0.145775 | 0.158725 | -8.88% |
| tokenize_beng_avg_ms | 0.106500 | 0.112575 | -5.70% |
| query_cn_avg_ms | 1.118250 | 1.116325 | +0.17% |
| query_beng_avg_ms | 1.586600 | 1.627250 | -2.56% |
| query_mixed_avg_ms | 1.239925 | 1.274925 | -2.82% |
| query_limit_avg_ms | 1.631600 | 1.662200 | -1.88% |

Build: -2.25352%; TOKENIZE: -7.29389%; query: -1.77204%; equal-category:
**-3.77315%**. Descriptive four-pair interval: **-13.8998% to +2.31545%**.
Sorting does not execute in TOKENIZE or these read queries, so those changes
cannot be credited to sorting: they demonstrate host drift/noise. Even the
causally affected build category has no positive aggregate result. This is not
proof that sorting always regresses; it fails the retention gate. The 267-line
implementation is removed without expanding it.

Report: `/home/xiaob/fts-validation-20261105/dictionary-sort-isolated-ab/`.
Protected patch: `/home/xiaob/fts-validation-20261105/dictionary-sort-rejected.patch`.

## Independent direct-merge gate: rejected

Both executables include the BMW correctness fix and passed the strict oracle.
The only kernel difference is the <=8-dimension DAAT direct merge.

| Metric | Control | Direct merge | Reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 6.514750 | 6.492250 | +0.35% |
| build_ik_content_sec | 4.739250 | 5.016250 | -5.84% |
| build_beng_en_sec | 2.553750 | 2.584000 | -1.18% |
| tokenize_ik_avg_ms | 0.153000 | 0.170425 | -11.39% |
| tokenize_beng_avg_ms | 0.110450 | 0.117800 | -6.65% |
| query_cn_avg_ms | 1.285800 | 1.159375 | +9.83% |
| query_beng_avg_ms | 1.867525 | 1.762250 | +5.64% |
| query_mixed_avg_ms | 1.479825 | 1.441225 | +2.61% |
| query_limit_avg_ms | 1.678475 | 1.867225 | -11.25% |

Category reductions: build -2.22799%, TOKENIZE -9.02174%, query +1.70816%.
Equal-category reduction: **-3.18052%**; descriptive paired interval:
**-9.74899% to +2.38445%**. Direct merge does not execute in build, TOKENIZE,
or BMW LIMIT; those changes demonstrate noise, not a causal merge regression.
The complete result fails the retention gate, so the experiment is removed.

Report: `/home/xiaob/fts-validation-20261105/direct-merge-isolated-ab/`.
Patch: `/home/xiaob/fts-validation-20261105/direct-merge-experiment.patch`.
Current kernel source is adc plus the BMW correctness fix. The earlier 9.81537%
combined-candidate result above is historical and must not represent this source.

## Auxiliary-build overlap budget and minimal experiment

Eight task-interval observations in `direct-merge-isolated-ab` show zero overlap
between inverted and doc-word construction. These are task wall intervals, not
inclusive CPU samples. The two tasks cover 88.86%, 96.55%, and 93.20% of the
three respective outer DDL durations on average.

| Metric | Inverted s | Doc-word s | Ideal reduction | Assumed 70% of ideal |
| --- | ---: | ---: | ---: | ---: |
| build_ik_all_sec | 3.19255 | 2.58457 | 39.8077% | 27.8654% |
| build_ik_content_sec | 2.52833 | 2.18018 | 44.6750% | 31.2725% |
| build_beng_en_sec | 1.30988 | 1.08311 | 42.1667% | 29.5167% |

The ideal no-contention ceiling is 14.0722 equal-category points relative to
this control; the explicitly assumed achievable budget is 9.8505 points.
Concurrency can increase task durations through contention, so this is a
hypothesis requiring a real A/B, not a forecast or an original-source score.
Implementation/verification cost is initially one line plus full link, strict
oracle, and eight official-workload rounds. Risks include memory pressure,
DDL locking and CPU oversubscription.

The minimal diagnostic changes mini-mode generic DdlBuild worker count from
one to two, against the frozen adc+BMW control. This is not an approved final
generic-DDL behavior change. If the gate is passed, a production candidate must
be scoped to independent FTS auxiliary tasks with resource constraints.
No credible combined >=50% path is established by this build-only direction.

### Minimal two-worker experiment: positive

The one-line diagnostic fully linked in 58.407 seconds. Both strict regression
oracles passed and matched. The unchanged nine-metric AB/BA experiment completed
four pairs, preserving every trial, in `two-builders-isolated-ab/`.

| Metric | One worker | Two workers | Reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 6.395000 | 4.309750 | +32.61% |
| build_ik_content_sec | 4.992500 | 2.781750 | +44.28% |
| build_beng_en_sec | 2.686250 | 1.476250 | +45.04% |
| tokenize_ik_avg_ms | 0.164750 | 0.158400 | +3.85% |
| tokenize_beng_avg_ms | 0.113775 | 0.113750 | +0.02% |
| query_cn_avg_ms | 1.304550 | 1.327575 | -1.76% |
| query_beng_avg_ms | 1.921975 | 1.916875 | +0.27% |
| query_mixed_avg_ms | 1.544425 | 1.481775 | +4.06% |
| query_limit_avg_ms | 1.737325 | 1.708125 | +1.68% |

Build reduction 40.64438%, TOKENIZE 1.93815%, query 1.05941%; overall
**14.54731%** against adc+BMW, not against original. The descriptive paired
interval is 11.07315%-17.99454%. Build-only weighted contribution is 13.54813
points, close to the 14.07-point ideal budget. This comparison is across different
rounds, so 14.07 is not a strict bound on the measured 14.55 total. Changes in
TOKENIZE/query are not credited to DDL concurrency. Peak process RSS rose from
406068-431280 kB to 467808-539932 kB in this experiment.

The validated mechanism is now being narrowed to a dedicated single-worker
mini-mode doc-word queue. Other DDL keeps its original single worker; non-mini
mode keeps its original pool. The extra queue has complete init, failure cleanup,
stop, wait and destroy handling. Only FTS doc-word auxiliary tasks request it.
The scoped implementation subsequently passed the fresh link, strict oracle
and both independent and original-source A/B comparisons recorded at the top.

### Residual overlap gate: stop expanding worker count

In `two-builders-isolated-ab`, one-worker task intervals have zero overlap.
The two-worker intervals overlap for 3.14868 s (all-IK), 2.36614 s (content-IK),
and 1.11955 s (BENG). Content and BENG overlap the entire shorter task.
At fixed task durations, removing the remaining launch gap could contribute
only **0.29778 equal-category points**. More workers are not a credible next
optimization. The actual scoped queue remains one ordinary worker plus one
FTS doc-word worker.

## Current-candidate profiling and complete execution counts

New sampling: `metric-fts-aux-queue-v2/`, 999 Hz, current frozen binary.
These diagnostic times are excluded from scoring. The all-IK window is
contaminated by first-recording/first-touch overhead (13.27 user CPU seconds
and 18.82 system CPU seconds); it must not supply an elapsed forecast.
Other diagnostic CPU/wall windows:

| Window | User CPU s | System CPU s | Wall s |
| --- | ---: | ---: | ---: |
| content-IK build | 5.66 | 0.29 | 2.92113 |
| BENG build | 2.39 | 0.15 | 1.46303 |
| TOKENIZE IK | 0.74 | 0.30 | 1.41715 |
| TOKENIZE BENG | 0.31 | 0.34 | 0.83305 |
| CN query | 0.71 | 0.04 | 0.78548 |
| BENG query | 0.89 | 0.04 | 0.95092 |
| mixed query | 0.64 | 0.02 | 0.70909 |
| LIMIT query | 1.00 | 0.01 | 1.12792 |

The BENG query CPU sample tree has do_one_merge_round 80.71% inclusive,
fill_merge_heap 49.25% inclusive (inside that round), token get_next_row
23.80% inclusive (inside heap filling), and token get_next_batch 18.33%
inclusive (inside token get_next_row). These four fractions are nested and
**cannot be added**. Filtering is 13.54% inclusive; BM25 batch evaluation is
3.28% inclusive. These remain CPU shares, not removable wall fractions.

`controlled-fts-aux-queue-v3/` separately counts 50 statements per metric.
The perf counter is enabled and acknowledged before SQL starts and disabled
and acknowledged after SQL completes. All six counters report 100% enabled
runtime. Both TOKENIZE windows record exactly 50 segment entries, correcting
the old attach-gap method. Counts per statement:

| SQL | Segment | DAAT rounds | Row filters | Token batches | BMW pivots | Named malloc entry |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| TOKENIZE IK | 1 | 0 | 0 | 0 | 0 | 19.22 |
| TOKENIZE BENG | 1 | 0 | 0 | 0 | 0 | 12.24 |
| CN | 1 | 8003 | 8001 | 57 | 0 | 94.44 |
| BENG | 1 | 11002 | 11000 | 77 | 0 | 123.48 |
| mixed | 1 | 7334 | 7332 | 57 | 0 | 107.38 |
| LIMIT | 1 | 0 | 0 | 58 | 2186 | 133.52 |

Allocation counts cover only ObMallocAllocator::alloc(size,attr) across the
process, including background work. They are not all suballocations, nor are
they all attributable to FTS. Input data remains the unchanged 20000-document
official workload. Probe timing must not enter A/B. All temporary probe events
were removed and the diagnostic server exited. Failed v1/v2 protocol-check
runs remain archived and do not replace the successful six-window result.

### Next query gate

The largest remaining measured execution-scale discrepancy is thousands of
row-level merge/filter transitions versus tens of posting batches. A batch
merge or eligible aggregate path should first demonstrate reduced transitions
with exactly identical full MATCH, scores, ordering and LIMIT results.
Three count-query metrics have a combined infinite-speed scoring ceiling of
25 points. To contribute 10 points at a 3x component speedup requires at least
60% of their actual elapsed critical path; the present inclusive CPU profile
does not prove that elapsed coverage. LIMIT has a separate 8.33-point ceiling
and must retain implicit relevance ordering. No large implementation is
authorized by CPU percentage alone.

Only BM25 arithmetic has a low measured CPU share; even replacing 3.28% CPU
work with zero work does not establish material overall wall benefit.
Whole-query tokenization runs once per SQL, so eliminating repeated query
segmentation is not a valid high-yield hypothesis for these measurements.
The doc-word table contains per-word counts, not just one document-length row;
discarding word rows would change the index and is not an optimization.

## Remaining budget decisions

The following are CPU evidence and algebraic gates, not wall-time forecasts.
The metric profiler's first build window has incomplete sampling coverage,
and TOKENIZE has only 149 IK / 71 BENG samples. These are insufficient to assign
precise elapsed fractions or allocation/call counts.

* Sorting: 13.50% / 16.71% inclusive CPU in content-IK / BENG build windows,
  not removable elapsed fractions. Independent performance failed its gate.
* Isolated BM25 arithmetic: roughly 4.3-7.4% inclusive query CPU. Even if that
  were elapsed coverage in all four query metrics, eliminating it entirely
  would contribute only 1.43-2.47 points. Do not expand this direction alone.
* IK DAT lookup: 9.40% inclusive TOKENIZE-IK CPU and only part of build parsing.
  Optimizing its nested hash lookup cannot also claim the full segmentation
  percentage. A trie rewrite is deferred until a larger budget is demonstrated.
* FTS single-row adapters: the 82.86% / 83.03% inclusive build labels contain
  sorting, segmentation and downstream execution. `ObOperator::get_next_batch`
  self samples are 4.46% in content-IK; `get_next_row` self samples are 1.41%.
  These do not prove a 60% removable adapter fraction. The crashing batch
  experiment remains isolated, rather than undergoing a broad rewrite.
* Bulk DAAT: the 74.14-85.23% inclusive labels contain essential posting
  decoding, relevance, filtering and merge work. Require an isolated merge
  result and exact work counts before budgeting a broad bulk implementation.

The original/safe table above describes the frozen, now superseded combined
candidate only. It must not be relabeled as a result for the sort-free source.
No measured combination yet supports the 50% target.

## Next executable steps

1. Preserve the verified FTS-only queue and BMW fix; both original-source and
   isolated eight-round comparisons are complete.
2. Use `controlled-fts-aux-queue-v3/counts.json` and the current-candidate query
   CPU recordings for a minimal batch-merge/aggregate feasibility experiment.
   Its first gate is reduced row transitions and a measured elapsed budget,
   before broad C++ changes. Do not resume generic worker-count, sort, direct
   scalar merge or BM25-arithmetic micro-optimization.
3. Any accepted next candidate needs a new full link, strict oracle and exact
   original-versus-candidate nine-metric A/B. Current local reduction is 14.28146%.
4. Preserve all valid and rejected results, record the next command, and keep
   TASK_STATUS=INCOMPLETE until official full score is actually confirmed.

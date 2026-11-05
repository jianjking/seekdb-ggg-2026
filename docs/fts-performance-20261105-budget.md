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

## Current verified candidate: batch union, binary ID and COUNT witness

The current source preserves 40881ac2, batch union and binary-ID comparison,
and adds a narrowly gated positive-score witness for pure COUNT(*) with a
sole natural-language Boolean MATCH condition. Projected scores, explicit
thresholds, Boolean mode, boost, parameter text, sorting, LIMIT, HAVING,
non-scalar grouping, subqueries and function/index-merge lookups keep scoring.
The existing statistics definitions and DAAT selection remain intact.

Binary: `/home/xiaob/fts-validation-20261105/bin/seekdb-fts-count-witness-v2`.
SHA-256: `aa8c382f4e735efe91fb1172596384e66af967f759cd00a583bdfe51fdca983b`.
Full link: `build-fts-count-witness-v2.log`, **46.923 seconds**, success.
Source snapshot: `fts-count-witness-v2.source.diff`.
The kernel changes remain uncommitted at the time of this measurement.

Fresh four-pair original/candidate AB/BA, with all official parameters fixed:

| Official metric | Unit | Original mean | Candidate mean | Reduction |
| --- | --- | ---: | ---: | ---: |
| build_ik_all_sec | s | 6.215000 | 3.963250 | +36.23089% |
| build_ik_content_sec | s | 4.701250 | 2.935000 | +37.56980% |
| build_beng_en_sec | s | 2.555250 | 1.511250 | +40.85706% |
| tokenize_ik_avg_ms | ms | 0.149175 | 0.147825 | +0.90498% |
| tokenize_beng_avg_ms | ms | 0.123400 | 0.102000 | +17.34198% |
| query_cn_avg_ms | ms | 1.272600 | 0.768000 | +39.65111% |
| query_beng_avg_ms | ms | 1.934825 | 1.030025 | +46.76392% |
| query_mixed_avg_ms | ms | 1.441450 | 0.822600 | +42.93246% |
| query_limit_avg_ms | ms | 1.748025 | 1.526300 | +12.68432% |

Build **38.21925%**, TOKENIZE **9.12348%**, query **35.50795%**;
three-category equal-weight reduction **27.61689%**. Descriptive four-pair
bootstrap interval **24.86365%-30.08330%**. Local gap **22.38311 points**.
TOKENIZE has visible host drift, especially the original BENG 0.1006-0.1518 ms
range. All valid samples are retained; no TOKENIZE gain is causally assigned
to query-only changes. This is local A/B, not an official full-score result.

Report: `/home/xiaob/fts-validation-20261105/original-count-witness-ab-v2/`.
All nine metrics completed. The strict candidate oracle passed with zero
known-error allowances; original BMW failures are recorded separately.
The full unaffected oracles, including independent BM25 checks, match.
Official benchmark files and frozen executable hashes stayed unchanged.

The isolated COUNT-witness eight-round A/B is `count-witness-isolated-ab-v2/`:
build +1.15015%, TOKENIZE +3.02719%, query +17.05641%, overall +7.07792%
versus binary comparison, descriptive interval 5.22999%-8.84705%.
Only three COUNT queries are causally affected. Independent gains from
different A/B runs must not be added to obtain the original-source result.

Controlled ten-SQL windows (`count-witness-controlled-v2/`) show BM25 batch
calls per COUNT reduced from CN/BENG/mixed 53/73/52 to **0/0/0**. LIMIT still
uses 58 BM25 batches and 2186 BMW pivots. Posting traversal, merge windows,
and output cardinality are unchanged. The filter function still runs, but
its redundant Boolean expression evaluation is removed. Named process malloc
entries fell from 101.2/130.5/114.3 to 86.1/108.3/92.1; these counts include
background work and only one allocator overload.

The first witness prototype never activated: `!has_group_by()` also rejects
scalar COUNT. GDB confirmed the shape and NULL default parameter text.
V2 uses `is_scala_group_by()` instead. V1's eight-round -1.08301% is retained
as a non-activation result, not an estimate of the successful mechanism.

The aux-queue/batch-union/binary confirmation in
`aux-queue-batch-union-confirmation-ab/` also passed all eight rounds and strict
oracles: query +22.94811%, overall +4.34807%, with interval crossing zero
(-3.73074%-11.63806%) and unrelated build/TOKENIZE drift. This uncertainty is
retained alongside the positive fresh original-source comparison.

The extended DML oracle records indexed and PK-lookup scores separately and
compares each full path between binaries. It does not assert that the two
plans must produce identical scores after DML: that assumption fails on the
original itself, as documented in `dml-cross-plan-oracle-rejected.md`.
Independent formula tests and exact ranked-prefix assertions were not relaxed.

### Remaining Quantified Budget

The new acknowledged native profile `count-witness-long-cpu-v2/` has 30,000
SQLs each for SELECT 1 and TOKENIZE, 12,000 each MATCH, with zero lost samples.
Wall/user/system us per SQL: SELECT 1 101.45/22.67/46.67;
IK 160.95/89.33/38.00; BENG 113.80/50.33/34.33;
CN 828.08/725.00/58.33; BENG query 1061.12/958.33/61.67;
mixed 866.18/751.67/69.17; LIMIT 1561.92/1450.00/61.67.
These are instrumented diagnostic windows, not scored intervals.

BENG merge-window inclusive user CPU 59.10% includes posting reads 30.40%
and comparison. Projection 8.14%, caching 5.06%, and the no-op filter 1.67%
are separate children outside that window. Their nested descendants must not
be counted again. A projection-only rewrite has no demonstrated large wall
budget: even p_wall=0.15 and infinite speed supplies only about 2.13 original
points; 2x component speed supplies 1.07. Defer that broad rewrite.

TOKENIZE parse_param is 8.99% IK / 11.63% BENG user CPU; segmentation is
39.63% / 14.44%. These are CPU fractions, not elapsed savings.
Cross-SQL IK result caching has no proved dictionary invalidation contract
and is not implemented. Unchanged SQL-loop fixed costs cannot be attributed
to parser work by subtracting two noisy windows.

Two minimal storage counterfactual gates have now completed without native
edits. Integer IDs (`integer-id-gate-v1/`) match all full ID/BM25 results but
reduce the four-query class only about 8.8%, below the 15% expansion gate,
with a LIMIT regression. Reject explicit signed-PK migration.

Flat storage (`row-format-gate-v1/`) also matches all full ID/BM25 results,
but encoded/flat means in ms are CN 0.72475/1.59377,
BENG 0.96763/2.17504, mixed 0.76013/1.59054 and
LIMIT 1.40704/2.36285. Reject format replacement: all four queries regress.
These are isolated diagnostic schema variants, never official scores.
Both servers exited. Do not expand these weak directions.

Even zero query time with current build/TOKENIZE would yield only
**49.11424%** overall. If TOKENIZE is frozen, equally improved build/query
would each need **70.43826%** original-source reductions. Those require
another **52.15057%** of current build time and **54.16220%** of current query
time. No measured combination yet demonstrates a credible path to 50%.

## Historical batch-union and binary-ID candidate

The new frozen candidate contains the preserved 40881ac2 queue/BMW work,
watermark-bounded pairwise posting-batch union, and fixed-width binary-ID
comparison. It has not yet been committed. It contains no internal-PX-degree
experiment or exhaustive Top-K selection experiment.

Binary: `/home/xiaob/fts-validation-20261105/bin/seekdb-fts-binary-compare-v1`.
SHA-256: `d3f1c7037cf632f4ece94428c022316373fe3f7a6d9d9a34d965248a35d35b8e`.
Full link: `build-fts-binary-compare-v1.log`, **37.581 seconds**, success.
Frozen source: `fts-binary-compare-v1.source.diff`.

Four complete original/candidate AB/BA pairs, all official parameters unchanged:

| Official metric | Unit | Original mean | Candidate mean | Reduction |
| --- | --- | ---: | ---: | ---: |
| build_ik_all_sec | s | 6.232250 | 3.861500 | +38.04003% |
| build_ik_content_sec | s | 4.537000 | 2.754750 | +39.28257% |
| build_beng_en_sec | s | 2.577000 | 1.426000 | +44.66434% |
| tokenize_ik_avg_ms | ms | 0.151750 | 0.139125 | +8.31960% |
| tokenize_beng_avg_ms | ms | 0.108975 | 0.097250 | +10.75935% |
| query_cn_avg_ms | ms | 1.276050 | 0.949350 | +25.60245% |
| query_beng_avg_ms | ms | 1.843900 | 1.302475 | +29.36303% |
| query_mixed_avg_ms | ms | 1.397200 | 1.058300 | +24.25565% |
| query_limit_avg_ms | ms | 1.650725 | 1.488875 | +9.80478% |

Build **40.66231%**, TOKENIZE **9.53948%**, query **22.25648%**;
three-category equal-weight reduction **24.15276%**. Descriptive four-pair
interval: **22.00571%-26.00223%**. Local gap to 50%: **25.84724 points**.
These are local results, not an official score or a claim of completion.
TOKENIZE source was not changed by these query mechanisms; its movement is
reported but is not causally credited to the new query code.

Report: `/home/xiaob/fts-validation-20261105/original-binary-compare-ab-v1/`.
Both strict oracles match except separately recorded original BMW failures.
The new boundary fixture compares all 5000 IDs and independent BM25 scores
(including the existing average-length floor of 10), exhausted/absent streams,
COUNT/SUM, OFFSET/LIMIT, and prepared-statement parameter changes.
Candidate has zero known-error allowances. Binary and official file hashes
are checked by the harness before and after all eight rounds.

### Independent Mechanism Gates

| Mechanism | Evidence and budget | Actual result / decision |
| --- | --- | --- |
| Internal FTS SQL PX degree 2 | Seven-line isolated experiment; conditional 4.94-6.91 overall points at p_build=0.50-0.70 and 2x execution | Eight rounds: build -0.97574%, overall -5.94846%; removed, source/logs retained externally |
| Unique-leading posting spans | Exact hit sets have longest single-dimension runs only 1/1/2 in primary-key order; physical ID order not proven equivalent | No credible long-span budget; rejected before kernel implementation |
| Watermark-bounded posting-batch union | Simplified independent-heap prototype 2.43-3.55x; conditional ~7.4 overall points, not an elapsed forecast | Isolated eight rounds versus aux-queue: query +11.71413%, overall +7.88375%, overall interval crosses zero; strict oracle passes |
| Same union versus original | Full original A/B, all valid outliers retained | Build +32.79103%, TOKENIZE -11.27410%, query +9.24588%, overall +10.25427%; did not supersede historical best alone |
| Fixed-width binary-ID comparisons | Long native profile: ~19%-23% inclusive user CPU in ID comparison for the three COUNT queries; not wall coverage. Conditional p=20%, component 4x: 5 overall points | Isolated eight rounds versus batch union: query +12.83474%, causal weighted query contribution +4.27825 points; overall +2.81814%, interval crosses zero. Combined original gate above passes |
| Exhaustive batch retrieval before existing exact Top-K sort | Only LIMIT affected; absolute ceiling 8.33 points. Conditional p=0.35-0.50 at 2-3x: 1.46-2.78 points | Linked in 49.408 s; strict oracles match. Eight rounds: LIMIT +5.15599% (+0.42967 weighted points), overall -2.51708%; rejected and removed |

The union's watermark is the minimum buffered last-ID among live lists.
Every posting through that ID is already loaded, so pairwise union can sum
all contributions without per-document heap operations. Scratch storage is
bounded by term count times batch size. Buffered ID pointers remain valid
until that output window is consumed. Boolean, minimum_should_match > 1,
function lookup and BMW retain their prior algorithm.

Binary-ID comparison is selected only for Varchar + binary collation at
iterator initialization. Non-null, non-extended 16-byte IDs use byte comparison;
all other lengths, NULL and sentinels retain the generic comparison. It does
not reinterpret little-endian document-ID fields as numeric sort order.

`window-merge-long-cpu-v1/` has seven acknowledged diagnostic windows:
20,000 statements for SELECT 1 / each TOKENIZE, 6000 for each MATCH. No lost
samples were reported. Diagnostic wall/user/system microseconds per SQL:
SELECT 1 72.98/18.50/30.00; IK 141.42/73.50/36.50;
BENG 109.70/41.00/38.00. IK parse_param is 10.32% and segment 42.09%
inclusive user CPU; BENG 12.17% and 13.63%. These sibling fractions are not
elapsed ablations. Parser-property or JSON-output work alone has no demonstrated
large budget, so no further speculative rewrite was made.

Controlled counters in `controlled-window-merge-v1/` show per SQL:
CN 54 merge windows / 8001 filters / 55 reads; BENG 67 / 11000 / 76;
mixed 52 / 7332 / 55. Their old heap merge rounds are now zero. LIMIT is
unchanged at 2186 BMW pivots / 58 reads. Process-wide entries to one malloc
overload were 19.26/12.16 for IK/BENG, not counts of all FTS suballocations.

The exhaustive Top-K selection was actually executed, not an ineligible
no-op: `controlled-exhaustive-topk-v2/` records per LIMIT SQL zero BMW pivots,
57 merge windows, 9333 filters and 61 posting reads. Replacing pivot work
introduced full-result filtering and more reads. The measured gain did not
justify expanding the prototype. Its isolated patch is retained externally as
`exhaustive-topk-rejected.patch`; the query-planning file has no remaining diff.
The initial non-root probe attempt lacked tracefs permission and has no valid
counts. The root retry removed all temporary events and exited its instance.

No validated combination yet establishes a credible >=50% path. Even zero
query time with the latest observed build/TOKENIZE means would yield only
50.06726% overall, so query-only development would require effectively
eliminating the entire query category. New build/TOKENIZE architecture work
still needs independently measured budgets. No push or official test was made.

## Historical Verified Candidate: 40881ac2

The 40881ac2 source is adc plus the BMW correctness fix and a single-worker
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

1. Preserve the verified queue, BMW fix, batch union, binary-ID comparison and
   COUNT witness. Current full original-source A/B is **27.61689%** overall.
2. Integer-ID and flat-storage gates are rejected. Evaluate a count-only
   execution path's actual data dependencies and work scale before expanding
   any document-materialization or aggregate prototype. TOKENIZE/build still
   require independently demonstrated mechanisms.
3. Any retained kernel change needs full link, strict complete oracles, an
   isolated nine-metric A/B and a fresh original/candidate comparison.
4. Preserve every experiment and an exact recovery command; official >=50%
   full-score confirmation is still required. TASK_STATUS=INCOMPLETE.

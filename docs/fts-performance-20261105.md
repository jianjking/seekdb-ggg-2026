# FTS performance validation: 2026-11-05

## Result and Versions

The candidate linked successfully and passed isolated SQL correctness tests, concurrent
evaluation tests, and eight complete runs of the unchanged official nine-metric benchmark.
The controlled-run equal-category mean reduction was **2.21%** against the previous
runnable version. This is a modest, environment-sensitive measured improvement.

- Workspace: `/home/xiaob/seekdb-fts-linux`, branch `competition/fts-opt-v3`.
- Before: `f4e13c27c8e7c22cb3f29cc3ce2b59e758c8cd41`, including the BENG ownership
  optimization and committed BUILD dependency fixes, with the broken row-map reuse reverted.
- After: the FTS source changes committed with this report.
- Before binary SHA-256: `eaa186f75f42c8e7399b6e3f5f5ae8229654f05cc576cac482cd1401b7570983`.
- After binary SHA-256: `4028aa844eb1d9359cad8301d008296ca582f80a5c0af171b664b7ae5945cdd9`.
- Frozen binaries: `/home/xiaob/fts-validation-20261105/bin/seekdb-previous` and
  `/home/xiaob/fts-validation-20261105/bin/seekdb-candidate`.

The official reporter reads the checkout's HEAD, rather than the executable's source
identity. Both sets of raw reports consequently name `f4e13c27c` and a dirty checkout.
The binary hashes, saved source diff, and manifest identify the actual A/B executables.

## Nine Official Metrics

Each number below averages four fresh-instance trials. The execution order was
A/B/B/A/A/B/B/A. Both versions inherited the same CPU affinity, logical CPUs 0-3.
Positive reduction means lower elapsed time; negative means a higher measured mean.

| Official metric | Unit | Before | After | Reduction | Before trial range | After trial range |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| build_ik_all_sec | s | 6.80775 | 6.47475 | +4.89% | 6.2670-7.9740 | 6.0650-6.8070 |
| build_ik_content_sec | s | 4.78525 | 4.71475 | +1.47% | 4.6340-4.9330 | 4.4420-5.0410 |
| build_beng_en_sec | s | 2.69175 | 2.50400 | +6.98% | 2.6260-2.7520 | 2.3230-2.6270 |
| tokenize_ik_avg_ms | ms | 0.14975 | 0.15093 | -0.78% | 0.1429-0.1594 | 0.1341-0.1689 |
| tokenize_beng_avg_ms | ms | 0.10850 | 0.10500 | +3.23% | 0.1030-0.1172 | 0.0954-0.1158 |
| query_cn_avg_ms | ms | 1.31793 | 1.27153 | +3.52% | 1.2568-1.4107 | 1.2066-1.3210 |
| query_beng_avg_ms | ms | 1.90943 | 1.87800 | +1.65% | 1.8369-1.9517 | 1.7801-1.9613 |
| query_mixed_avg_ms | ms | 1.42820 | 1.41810 | +0.71% | 1.4079-1.4365 | 1.3712-1.4907 |
| query_limit_avg_ms | ms | 1.70130 | 1.73545 | -2.01% | 1.6019-1.8596 | 1.5835-1.9758 |

The three build improvements are averaged first, as are the two tokenize and four query
improvements, and then those three category means are averaged. All valid trials are
included. The official configuration and scoring implementation were unchanged.

Every trial returned the required hit counts: CN **8001**, BENG **11000**, mixed **7332**,
and LIMIT **20**. Every trial also passed the official configuration check.

Configuration: `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`.
The eight raw reports and their per-sample console logs are in
[`fts-performance-20261105/`](fts-performance-20261105/).

## Follow-up on the Two Slower Means

The small IK and LIMIT regressions in the main table were investigated with a separate
long-batch A/B/B/A experiment, using the same frozen binaries and generated 20,000-row
databases. This experiment used CPU 0 for both versions, 12 samples per phase per trial,
10,000 IK calls per sample and 1,000 LIMIT queries per sample. There were two trials
per binary. Wall time used a monotonic clock; server CPU time came from `/proc/PID/stat`.

| Supplementary metric | Before ms/call | After ms/call | Reduction |
| --- | ---: | ---: | ---: |
| IK wall time | 0.0725761 | 0.0665728 | +8.27% |
| IK server CPU time | 0.0644583 | 0.0586667 | +8.99% |
| LIMIT wall time | 1.7603269 | 1.6964184 | +3.63% |
| LIMIT server CPU time | 1.7133333 | 1.6679167 | +2.65% |

The supplementary runs did not reproduce a stable regression. Their different affinity
and sampling parameters mean they are kept separate from the official nine-metric
results; they do not replace either negative entry in the main table. Full samples are
saved in `focused-results.json`, with aggregates in `focused-summary.json`. The exact
supplementary harness is `/home/xiaob/fts-validation-20261105/focused_latency.py`.

The host showed appreciable run-to-run variance. Preliminary unpinned trials did not
establish a consistent gain. The controlled 2.21% result is not a guaranteed improvement
on a different machine or scheduling configuration.

## Correctness and Source Changes

The previous `abb15c77f` implementation put the reusable-map case in a terminal
`else if` branch, skipping segmentation. Its archived report contained zero hits for
all four MATCH workloads, so its very short timings were invalid. This candidate
reintroduces map reuse while always executing segmentation.

The retained BENG ownership optimization copies a borrowed token when it first becomes
a retained word-map key. The row cache clears old keys before resetting the document
arena. Bucket allocation follows the existing document-length heuristic, grows as
needed under the existing bucket-count bound, and avoids requesting 997 buckets for short documents.
Empty maps skip redundant clearing.

`ObFTWordMap` uses the existing `NoPthreadDefendMode`: its instances are local to an
expression evaluation, query-token extraction, or one non-copyable index-row cache.
Three query-token extraction sites now use that alias consistently. The shared
stopword set and shared dictionaries retain their existing synchronization.

`unittest/sql/fts/test_fts_regression.py` checks:

- English case folding, Chinese tokenization, empty text, stopword-only text, and long text.
- Exact frequencies for repeated words, including 4096 repeats followed by another token.
- Index creation across documents of different lengths, including empty documents.
- MATCH membership, absent words, and token ownership after subsequent token processing.
- BM25 scores against an independent formula, with relative tolerance `1e-12`.
- BM25 ordering, LIMIT 0/1/2/4/20, and LIMIT with an offset.
- INSERT, UPDATE, DELETE, and rollback with an existing fulltext index.
- Four concurrent clients running 32 tokenization/ranking checks.

Before and after correctness JSON outputs were identical.

## Profiling and Rejected Variants

Software CPU-clock sampling at 499 Hz collected approximately 12,000 samples without
lost samples in the corrected variant with a fixed bucket-count request of 997. `ObLatch::unlock` accounted
for 10.90% and `wrlock` for 10.23%; call stacks showed two bucket-clearing passes per
document. This variant was rejected.

After adaptive buckets and avoiding empty-map clearing, the sampled unlock share fell
to 0.74%. The locked adaptive version completed a preliminary full A/B, but its
equal-category mean was 0.32% slower, so it was not selected. The final candidate also
removes locking from the owned word maps and passed the concurrent regression tests.

Profiling runs are excluded from the official timing table. Full recordings and
call-stack reports remain under `/home/xiaob/fts-validation-20261105/profile2/` and
`/home/xiaob/fts-validation-20261105/profile-adaptive/`.

## Build, Timing Controls, and Reproduction

The workspace already contained dependency repair commit `88cf5b65d`: the config target
declares `:local_storage_interface`, and generated headers declare `local_table_schema.h`.
Both the previous binary and the candidate passed the complete Bazel target. No global
header-injection workaround was required. The successful final build used:

```sh
export PATH=/home/xiaob/.cargo/bin:/usr/bin:/bin
/home/xiaob/tools/bazel-8.2.1/bazel \
  --output_base=/home/xiaob/.cache/bazel-fts/efad048bc8f08cb615d31510f9b260d6 \
  build //src/observer:seekdb --jobs=4 --compilation_mode=opt \
  --force_pic --spawn_strategy=standalone
```

Host: Intel Core i9-13980HX, WSL2 Ubuntu, 32 visible logical CPUs, approximately 7.6 GiB
RAM. Accepted runs used `hyperv_clocksource_tsc_page`, with `systemd-timesyncd` temporarily
stopped. Earlier trials with detected wall-clock steps were excluded and retained in
the external experiment directory. Every accepted official trial had an empty
clock-jump list, with a 5 ms detection threshold. The original `tsc` clocksource and
active time-sync service were restored after all experiments.

The benchmark shell script, generator, schema SQL, baseline JSON, and scorer match
their original Git contents and the manifest hashes. Instances were isolated; benchmark
servers were stopped after each trial. AI Functions, FileSQL, and AI Column workspaces
were not modified, and no remote push was performed.

The reusable driver creates a new output directory and does not remove existing data:

```sh
cd /home/xiaob/seekdb-fts-linux
PYTHONDONTWRITEBYTECODE=1 python3 unittest/sql/fts/run_fts_ab.py \
  --before /home/xiaob/fts-validation-20261105/bin/seekdb-previous \
  --after /home/xiaob/fts-validation-20261105/bin/seekdb-candidate \
  --output /home/xiaob/fts-validation-20261105/reproduction-new \
  --pairs 4 --cpus 0,1,2,3
```

The driver rejects wrong hit counts, configuration mismatches, non-positive durations,
wall-clock discontinuities, modified benchmark files, and binaries changed during a run.
It invokes the unchanged official scoring functions on each report. See `manifest.json`,
`ab-results.json`, `summary.json`, both correctness JSON files, and `build-tail.txt` in
the evidence directory for machine-readable provenance and results.

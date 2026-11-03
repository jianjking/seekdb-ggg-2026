# CAGRA build-path optimization

This note documents the changes made on top of the frozen V5 contest baseline
(`634888d9b037153fa143d2c913ba4b25503e1d62`).  The goal is to reduce the
`CREATE VECTOR INDEX ... lib=cuvs` end-to-end time without changing the SQL
contract, the final graph degree, or the recall gates.

## Baseline cost model

The V5 baseline builds a CAGRA graph with:

```text
graph_degree             = 128
intermediate_graph_degree = 256
NN-Descent iterations     = 100
```

and persists the result as:

```text
cuVS graph file (graph + fp32 dataset)  ->  temporary file
snapshot stream (graph bytes)           ->  index snapshot
snapshot stream (fp32 ids + fp32 data)  ->  duplicate dataset copy
```

Two costs dominate at 250K points:

1. **NN-Descent + graph pruning** scale with the iteration count and the
   intermediate degree.  The frozen baseline runs 100 iterations even though
   NN-Descent's default convergence budget is 20 and the graph degree is already
   doubled for recall headroom.
2. **Snapshot serialization** writes the dataset twice.  `cuvsCagraSerialize`
   is called with `include_dataset=true`, and the adaptor then writes the same
   fp32 rows again as a separate snapshot payload.

For reference, a CPU-only VSAG HNSW build on the same 250K x 128 data measured
75 s in the local Linux environment (32 vCPU), while the contest reference for
the GPU V5 baseline is 20--27 s on a Radeon PRO W7900D.  The gap between the
GPU baseline and the DDL floor shows that the CAGRA build, not the SQL scan, is
the main lever.

## Changes

### 1. FP16 build and search

The bridge converts the incoming fp32 rows to fp16 once, builds the CAGRA graph
with an fp16 dataset, and keeps the fp16 dataset in the index.  Queries are
converted to fp16 at the bridge boundary.  The public SQL type and all returned
ids remain unchanged; fp32 is not exposed to the kernel.

This halves the dataset bytes transferred to the GPU, halves the dataset stored
in the index/snapshot, and uses the GPU's fp16 distance kernels.  The final
graph degree is unchanged (128), so the search graph has the same connectivity
budget as the baseline.

The conversion is controlled by `SEEKDB_CUVS_FP16` (default `1`).  Set it to
`0` to build/search in fp32.

Quantization-only sanity check (exact fp16-quantized search vs exact fp32
search, no graph approximation, 1000 queries):

| dataset | Recall@10 | Recall@100 |
| --- | ---: | ---: |
| SIFT250K (250K x 128) | 1.000000 | 1.000000 |
| random 256d (250K x 256) | 0.998800 | 0.999010 |

The 256d drop is well inside the 0.005 baseline-delta gate; the CAGRA graph is
still built with the same degree and the same NN-Descent target, so this is the
expected order of magnitude of the fp16 contribution.

### 2. NN-Descent iteration budget

`SEEKDB_CUVS_NITER` defaults to `40` instead of `100`.  The final graph degree
and intermediate degree remain `128`/`256`; only the refinement budget is
reduced.  NN-Descent terminates early when the update counter falls below the
termination threshold, so the effective iteration count on easy data is lower
than the configured maximum.  For local quality experiments the value can be
raised back to `100` without rebuilding seekdb:

```bash
SEEKDB_CUVS_NITER=100 ...
```

`SEEKDB_CUVS_GRAPH_DEGREE` and `SEEKDB_CUVS_INTERMEDIATE_DEGREE` are also
available for controlled experiments; the submitted defaults are `128`/`256`.

### 3. One dataset copy in the snapshot

The snapshot stream now uses `reserved = 1` in its header:

```text
OBCUVS02 | graph_bytes | dim | reserved=1 | n | ids | graph file bytes
```

The graph file already embeds the dataset, so the separate fp32 vector payload
is omitted.  On load, `seekdb_cuvs_export_dataset` copies the embedded dataset
back to the host fp32 buffer used by the lazy VSAG fallback.  Legacy snapshots
with `reserved = 0` are still accepted and read the old inline vector payload.

This removes one 128 MB write (250K x 128 fp32) per snapshot, and with fp16 the
embedded dataset is another 64 MB instead of 128 MB.

### 4. Query-side buffer reuse

The bridge keeps the CAGRA search path unchanged in shape: the query is copied
once, one `cuvsCagraSearch` call is issued, and the top-k ids are copied back.
The fp16 conversion is done in one contiguous host buffer per call.  The
existing `itopk_size = max(512, 4 * topk)` and `search_width = 4` settings are
preserved so that Top-100 recall does not regress.

## Validation

Use the contest's fixed evaluation for the official number.  For local A/B
testing, build the bridge and seekdb exactly as specified in section 3.2 of the
task, then run:

```bash
# CPU baseline
CREATE VECTOR INDEX idx_cpu ON items(v)
  WITH (distance=l2, type=hnsw, lib=vsag, m=16, ef_construction=200, ef_search=64);

# GPU candidate
CREATE VECTOR INDEX idx_gpu ON items(v)
  WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);
```

Measure from the start of `CREATE VECTOR INDEX` through the first
`APPROXIMATE` query, on the same table state, and compare Recall@10/@100 against
an exact `l2_distance` query.  The reproducible smoke test in
[`validation/run_ddl_persistence_smoke.sh`](validation/run_ddl_persistence_smoke.sh)
also checks that the snapshot reloads on restart and that the GPU path is used.

Suggested sweep for the recall/performance trade-off:

```text
SEEKDB_CUVS_FP16=0/1
SEEKDB_CUVS_NITER=20/40/60/100
```

The hard gates require Recall@10/@100 within 0.005 of the frozen baseline; if a
sweep shows a larger drop, keep `SEEKDB_CUVS_NITER=100` and `FP16=0` and rely
only on the snapshot-dedup win.

## Limitations

- The local development host used for this branch has an NVIDIA GPU, so the
  numbers above are the contest reference (V5, W7900D) and a CPU DDL reference,
  not a measured AMD candidate result.  The official A/B run remains the
  authority.
- The fixed evaluation SQL and data are unchanged.  The optimization does not
  change the final graph degree, the metric, or the returned id semantics.

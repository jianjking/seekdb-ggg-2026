# AI COLUMN P0/P1 Handoff (2026-11-06)

Historical record. See [2026-11-07 status](ai-column-status-20261107.md) for
the current build entry points, component results and remaining feature gaps.

## Source And Isolation

- Branch: `feat/ai-column-20261106`; base: `c86a417a401db0d8b6036b172af26e16e1b04164`.
- Independent shallow clone of `origin/fix/ai-baseline`, without object alternates.
- Original staged AI patch was applied before the P1 corrections. Source, index,
  and working copies remain under `F:\OceanBase-2026\ai-column-p0-20261106`.
- No commit, push, official evaluation, or change to the frozen FTS checkout.

## Metadata Contract

The in-memory type array retains its original meaning. A `VECTOR(4)` column
still has exactly one type string. Ordinary columns retain their existing
`extended_type_info` blob representation.

For an AI column only, the persisted blob appends an OB serialization trailer
after that array: magic `0x4149434F4C`, version `1`, sync mode, retry limit,
function type, state/request/result-spec column IDs, model name, expression SQL,
and normalized expression. Readers reject an invalid or incomplete trailer.
Full column UNIS transport carries the same AI fields separately; decoded
strings are copied into the schema allocator. A legacy image without AI fields
clears an older definition when an object is reused. Once the AI flag is true,
incomplete or invalid metadata is rejected and AI state is cleared on failure.
UNIS permits all three companion IDs to be unallocated for the ALTER ADD RPC;
persisted blobs require three valid, distinct IDs. Companion IDs follow column-ID
remapping.

Normalized JSON version 1 contains `func`, `endpoint`, `expr_sql`, `sql_mode`,
`system`, and `deps`. `expr_sql` is SQL, not an already evaluated prompt.
Dependencies are read with the JSON parser, not by scanning text. No Worker
consumes this representation yet. No migration from the earlier experimental
`__AI_COLUMN__` string-array encoding is implemented; use fresh private test data.

## DDL Boundary

- CREATE and ALTER ADD retain ordinary nullable result columns and three hidden
  integer companion columns; ALTER persistence uses the existing DDL transaction.
- Prompt/config expressions use existing name resolution and type inference.
  Missing, self, chained AI, and hidden dependencies are rejected.
- JSON/integer prompts retain the supplied DDL test's exact strings. Decimal and
  floating results use a numeric prompt rather than an integer prompt.
- Modification, rename, or drop of AI columns, their companions, or their input
  columns is rejected until task invalidation and companion DDL are implemented.
- This work does not implement task creation, Worker, RETRY, or Strict Read.

## Validation And Remaining Gates

- Passed: Git diff whitespace check, independent object connectivity, Bison 3.8.2
  generation with zero conflicts, and inner-table schema generation (ID 12565,
  13 columns). Generated artifacts and logs are in `.p1-validation/`.
- Passed: seven original checkout HEAD/status/recorded-file hash comparisons,
  plus working-source backup comparisons and staged blob checks after Git's
  configured line-ending normalization.
- Clang 17 with C++20 and the declared Bazel feature macros compiles the column
  schema and unit-case sources to objects. The historical GCC/C++17 attempt used
  the wrong compiler contract, omitted `_NO_EXCEPTION`, and imposed an unsuitable
  512 MiB address-space limit. It was not repeated.
- DDL/virtual-table source checks use a diagnostic adapter for an older generated
  Rust config bridge, not a freshly built current-crate export. They do not
  establish a complete production build. Schema SQL standalone checks must retain
  the Unity prefix declared in `share_source_inventory.bzl`.
- Not run: the supplied five mysqltests, eleven new `AIColumnSchema` unit cases,
  and the new `ai_column_ddl_regression` mysqltest. A ten-case intermediate link
  probe with the available Linux gtest archives fails on missing OBLib/Schema
  implementation symbols. The final eleven-case sources compile to objects;
  linking was not repeated after memory preflight refused further checks.
  There is no server built from this source, and no
  unittest BUILD/CMake target. The original DDL test/result are unchanged and
  contain ALTER ADD assertions, not ALTER MODIFY success assertions.
- Still missing: the three official unit tests, evaluation gateway, and scoring
  specification. The supplied mysqltest Mock script is present.
- Detailed compile statuses, resource guards, and link diagnostics are recorded
  in `.p1-validation/RESULTS.md` and `.p1-validation/clang/`.
- P2 integration gate: successful affected-source compilation, original DDL and
  added regression passes, schema roundtrip tests, and reload/restart validation.
  AI vector-index behavior also remains unverified.

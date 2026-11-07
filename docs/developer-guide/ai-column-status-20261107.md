# AI COLUMN status: 2026-11-07

This is an incomplete implementation, not a passing competition submission.
It supersedes the build-status statements in the historical P1 handoff.
Branch: `feat/ai-column-20261106`.
Base HEAD: `c86a417a401db0d8b6036b172af26e16e1b04164`.
Any local checkpoint containing this file is WIP, not an accepted submission.
The independent checkout has no Git object alternates. Other checkouts and the
frozen FTS version were not modified or retested.

## Current production integration

The following code is now connected, but database acceptance is NOT established.

- DDL/Schema: CREATE, ALTER ADD, SHOW AI COLUMNS, typed prompts, dependency
  validation, three companions, metadata trailer/UNIS, deep copy and ID remapping.
  The original VECTOR type string remains separate from restored AI metadata.
- DML: INSERT/UPDATE/DELETE after-row hooks persist tasks through the existing
  external-session nested connection and business transaction. Dependency or
  primary-key changes invalidate results and replace requests; unrelated updates
  retain results. Batched rows restore their own old/new expressions.
- Worker: two server-lifecycle threads use persistent CAS claims, durable attempt
  budgets, expiring leases and owner/epoch fencing. Existing AI functions call
  model endpoints after the claim transaction commits. Terminal group members
  publish through one ordinary typed UPDATE plus task deletion in one transaction.
  The business-row lock precedes task locks; request/spec predicates reject stale
  results. Session capabilities/prompts/modes are restored before pool release.
- RETRY: grammar lowers to the normal UPDATE pipeline. Unquoted STATUS becomes
  the target AI column's state; quoted `status` remains a business column. READY
  rows can be retried. Mixed ALTER actions and non-AI targets are rejected.
- Strict Read: session variable and serialized physical-plan column references
  feed a pre-execution completeness scan using the statement's snapshot.
- Vector: invalidation writes NULL and publication uses ordinary DML/index
  maintenance. HNSW/VSAG/rollback/ANN behavior still needs a running database.
- ALTER ADD recovery: one Worker scans uninitialized companion specs in batches
  of 32 and invokes the transactional RETRY path. NULL specs are durable progress;
  the per-schema in-memory cache only suppresses completed scans.

Latest correctness repairs:

1. Resolve dependency IDs from persisted normalized JSON against the current
   table schema, rather than the unused generated-column dependency bitmap.
2. Read unsigned task table/column IDs with get_uint; use the native LOB reader
   for task expression/result fields and preserve NULL versus empty strings.
3. Respect explicit HTTP timeout caps without changing ordinary AI SQL defaults;
   do not sleep after the final HTTP attempt. Persisted retries own the budget.
4. Decode JSON integer/unsigned nodes by their actual type.
5. Validate saved AI expressions under their persisted SQL mode, restoring the
   ALTER session mode on both success and failure.
6. Project generated and ON UPDATE timestamp dependencies; suppress no-op
   generations. Publication and RETRY preserve business timestamps.
7. Partitioned heaps include the physical Tablet in the canonical task key.
   Hidden-key/VID reassignment relocates pending generations in UPDATE/UPSERT
   while retaining unchanged READY/FAILED results, including nonpartitioned heaps.
   Worker reads/locks/publishes through the captured partition. Deleted Tablets
   discard old tasks without cross-partition hidden-key aliasing.
8. ALTER seeding includes partition inputs and uses NULL-safe comparisons.
   A nullable partition key must not leave old rows permanently uninitialized.
9. Reject AI partition keys and generated-column indirection into AI inputs.
   CREATE resolves native generated dependencies in check_default_value before
   check_ai_column_dependencies, so this check sees populated dependency IDs.

There is no database runtime pass for these changes. No scoring claim is made.

## Supported Boundary

MySQL and async only. AI definition/dependency MODIFY, DROP and RENAME remain
rejected; the supplied DDL test does not require a successful MODIFY. Canonical
binary task keys are limited to 2048 bytes. SQL group publication and snapshot
completeness are implemented but still need real concurrency/rollback/restart
acceptance. Protocol tests alone do not establish those database invariants.

## Build repairs and evidence

- Added the missing Bazel/CMake share-test entry points, using the repository's
  module inventory and normal `oceanbase` implementation library.
- Passed the configured SDK path into SQL and PL parser generation, retaining
  the original path when no override is supplied.
- Corrected the Ubuntu libaio SONAME fallback from `lib:libaio.so.1t64`
  (which CMake emitted as `-llib:...`) to `-l:libaio.so.1t64`.
- GCC discovery still needs the explicit `-DGCC9:PATH=<SDK>/devtools` override
  in this checkout. Without it the compiler receives `GCC9-NOTFOUND`.

Local evidence is under `.p1-validation/20261107/` (ignored by Git). No old
database implementation library was linked into these checks.

| Check | Actual outcome | Evidence |
| --- | --- | --- |
| Current config Rust bridge | Offline package build, exit 0; peak 463380 KiB | `config-bridge.status`, `config-bridge.artifacts.jsonl` |
| Protocol component unit tests | 16 executed, 16 passed | `task_protocol.status`, `task_protocol.xml` |
| CMake configure/generate | Exit 0 after SDK/PL fixes | `cmake-gccsdk-final.status`, corresponding log |
| Configure after libaio flag correction | Exit 0, peak 35292 KiB | `cmake-gccsdk-verified.status`, corresponding log |
| Current CMake share-tests Unity object | Exit 0, peak 246656 KiB | `cmake-unit-object.status`, `cmake-unit-command.json` |
| Production Schema Unity object (30 sources) | Exit 0, peak 591476 KiB | `cmake-schema-object.status`, `cmake-schema-command.json` |
| Current DML Unity object | Exit 0, peak 676868 KiB; final hidden-key/VID relocation included | `cmake-dml-object.status`, `cmake-dml-command.json` |
| Current Worker Unity object | Exit 0, peak 586536 KiB; partition routing and NULL seeding included | `cmake-worker-object.status`, `cmake-worker-command.json` |
| SQL common Unity object | Exit 143; host memory guard terminated the owned compiler, sampled peak 608304 KiB | `cmake-common-object.status`, `cmake-common-command.json` |
| Task SQL service object | Exit 0, peak 175224 KiB | `clang/ai_column_task_sql_service.object.status` |
| Resolver and virtual-table factory checks | Syntax only, exit 0 | `clang/*.status` |
| SDK Bison 2.4.1 generation | Exit 0 | `bison-production.log`, generated production parser |
| Schema unit tests | 0 executed; 13 authored cases compile in the current test object | No runtime pass |
| Supplied mysqltests / added regressions | 0 executed out of 5 supplied / 3 added | No database built from this snapshot |
| Recovery, concurrency, ANN, performance | Not run | No acceptance claim |

The latest DDL indirect-dependency/partition guards and the final resolver heap
projection change have not been recompiled. SQL common Unity (including Strict
Read and transaction snapshot integration) was stopped by the host memory guard.
Its shell status says stopped=0 because termination came from PowerShell, not the
WSL-local guard. No source diagnostic was emitted before the interruption.
Older per-source status files are not current acceptance evidence for those edits.

Reproduction (PowerShell from the AI checkout):

```powershell
& ./.p1-validation/20261107/compile_cmake_unit.ps1 -Mode dml
& ./.p1-validation/20261107/compile_cmake_unit.ps1 -Mode worker
```

The scripts record the full generated compile command and exit status, use one
compiler, and guard host free memory/Commit plus WSL available memory and process
group RSS. These are objects, not test executables. DML/Worker keep production
definitions/includes/warnings, with O0/g0 and isolated object outputs.
The configure script does not start the production build.

## Remaining verification blockers

`share_tests` links the complete current `liboceanbase.so` and Rust runtime, not
just the column object. No such library/server is available from this snapshot.
The generated graph contains 374 translation units; a successful small object
does not establish safe memory use for the complete cold build or final link.
Ubuntu lacks `libaio1t64` and `libaio-dev`; fixing the link flag does not supply
those libraries. No package was installed or system configuration changed.

The Share header-ownership checker also fails on existing unrelated inventory
entries (`resource/ob_server_resource.h`, `ob_tenant_role_transition_service.h`).
The task SQL header is now public and its protocol header is interface closure;
the remaining failures do not name either task header. This failure was not bypassed.
Three official unit-test sources, the evaluation gateway and full scoring
materials remain absent.

Current per-source object evidence (exit 0, O0/current headers; NOT a link):
`clang/ai_column_worker.object.status`, `clang/ob_ai_func_client.object.status`,
`clang/ai_column_dml.object.status`, `clang/ob_del_upd_resolver.object.status`,
`clang/ob_optimizer.object.status`, `clang/ob_result_set.object.status`,
`clang/ob_column_schema.object.status`, `clang/test_ai_column_schema.object.status`.
Commands use `verify_current_clang.ps1 -Source <file> -Mode object`. This helper
suppresses warnings and does not replace the complete production build.

The current Worker and DML Unity checks passed; an earlier Worker launch was
refused by preflight. The latest common Unity attempt started only after available
physical memory recovered to 2851804 KiB / Commit 76%, then was terminated by the
host guard as conditions worsened. No further compiler was started. No full
link, forced WSL restart, process termination outside the owned compiler, or
system memory configuration change was attempted.

Added integration assertions cover ALTER existing rows, a 16 KiB result roundtrip,
transaction rollback/no requests, binary composite keys, primary-key changes,
generated/timestamp dependencies, partitioned heap movement, HNSW heap-VID task
relocation without unnecessary model calls, and nullable seed keys.
The DDL regression also switches ANSI_QUOTES between CREATE and ALTER. These are
authored expected results, not recorded successful executions.

No authorized remote builder or usable remote cache has been established.
The existing main CI uses OceanBase-only runners, private images and NFS.
The lowest incremental-cost verification path is an already authorized Linux
builder with the matching SDK and useful caches: build the full current snapshot,
run the 13 Schema cases and all 5 supplied / 3 added mysqltests on a fresh isolated
database. Run restart/late-result/group publication/ANN rollback assertions before
treating the connected production implementation as accepted.

No push, official submission, model request, benchmark, pagefile change, WSL
restart, or unrelated process termination was performed. This snapshot is not
ready for official evaluation.

# AI COLUMN: First Official Failure Investigation

Baseline: `e6a58db30dc0a76ed9eae3927fb02ec6072cf4ee`.
Repair branch: `fix/ai-column-official-20261107`.
Production repair: `b13779470d8c7fa1c3baad7d28706a3e8630c62d`.
Status: repaired executable linked; nine SQL suites, 29 unit tests and the
same-data crash-recovery fixtures passed locally on November 7, 2026.

## Official Evidence Gap

The available feedback lists five failed subcases, without the first failing SQL,
error number, `.reject`, database log, build log, startup command, or client
version. No official detailed log or evaluation gateway is present in this
checkout. Public GitHub check/status/workflow queries returned HTTP 403 rate
limit errors. The cause of the official five failures is therefore unconfirmed.

## Controlled Reproductions

All runs used the existing e6a58db server binary (SHA256
`833ee9966798148105d0d1691d4a8f5c7b4761ae0b3ed5ce14f55e5e79a22e4e`),
fresh private databases, the repository `run_case()` implementation, admin/admin,
and repository initialization settings. No private mysqltest prologue was used.

| Client and test source | Actual result | Evidence |
| --- | --- | --- |
| MariaDB 10.11, original five tests | 0/5 | Every reject differs only by an extra first line `connection default;`; all remaining lines match |
| MariaDB 10.11, explicit connection-log control | 8/9 | Five main tests pass; integration fails ALTER ADD initialization |
| Repository-declared obclient 2.2.3, original e6a58db tests | 5/5 | Original tests were extracted from the exact Git commit, including original expected results |

The obclient package was downloaded from the OceanBase mirror declared in
`deps/init/oceanbase.el9.x86_64.deps` and extracted privately, not installed.
Its mysqltest reports `Ver 3.5 Distrib 10.4.18-MariaDB`; package SHA256:
`35e38b580996fc583b0eaa961bd78911feff08981624154851db51d7e5aa935d`.
For this run, obclient executed unmodified `init.sql` and `init_user.sql`.
The earlier MariaDB adapter changed only client meta-command syntax.

The connection-log discrepancy proves a portability/evidence gap in the old
local harness. Because original tests pass with obclient, it does NOT prove
the cause of the official failure. Tests now declare `disable_connect_log`
before switching connections; SQL, assertions, and result files are preserved.

## Confirmed Production Defect

Standard `tools/deploy/init.sql` enables the recycle bin; the old local runner
did not execute that initialization. Dropped AI tables remain visible to the
runtime Schema enumeration. `seed_uninitialized_rows()` attempted to initialize
such a table and propagated `OB_SCHEMA_ERROR` (-4029), aborting the rest of
the scan. Live ALTER ADD rows stayed NULL, including the nullable partition
fixture. The integration reject and server log reproduce this failure.

The Worker now excludes recycled tables and records a failed table as incomplete
while continuing with other tables. Failed transactions never become completed
progress. The integration test explicitly retires an AI table before testing
ALTER ADD, preserving its original expected output and restoring session state.
This is a repair to a reproduced defect, not an attribution of all official FAILs.

## Current Repair Validation

The changed Worker Unity source and `liboceanbase_static.a` compiled successfully
with the existing Clang 17 CMake configuration (`-O0 -g0`, single concurrency).
The original LLD attempt was stopped by the Windows memory guard (exit 143).
A bounded BFD fallback failed on the generated empty linker script and its
adapted retry was refused before launch. Neither attempt produced a candidate.
After host memory recovered to about 3.1 GiB free and 64% Commit, the existing
CMake link command succeeded with the matching SDK LLD and one linker thread.
The validation link additionally strips SDK DWARF, preserving runtime sections
and the normal symbol table. Link exit: 0; linker maximum RSS: 2,003,992 KiB.
The successful link and runtime runs did not trigger the Windows memory guard.

New executable:
`/home/xiaob/ai-column-validation-f9f9266-20261107/build/src/observer/seekdb.official-lld-validation`.
SHA256: `03e78d4b6e69bc7298b5543b9a3b0f14db359ba3b78c0769be790c6fa0518989`.
The native Worker source is byte-identical to the repair checkout; SHA256:
`9f1c468714ceef830caed86ea3ab0668e7f4f2fe3b1e5fdadbc6670539ac35cf`.

| Current run | Actual result | Checks |
| --- | --- | --- |
| Five main SQL suites, new executable | 5/5, each exit 0 | DDL, DML/rollback, Worker/retry/stale results/group publication, vector index, RETRY/Strict Read |
| Additional SQL suites, new executable | 4/4, each exit 0 | DDL regression, result write protection, integration including retired-table ALTER ADD, transport faults |
| Schema and task protocol unit tests | 29/29, exit 0 | Schema 13; task protocol 16; fresh execution at 19:50:52, unchanged unit/Schema sources since baseline |
| Same-data crash-recovery fixtures, new executable | Seed 0; crash -9; verify 0 | Durable RUNNING claim, real lease expiry, atomic publication, Schema/ordinary VECTOR reload, task cleanup, Strict Read, ANN and post-restart regeneration |

The nine SQL suites used raw repository `init.sql` and `init_user.sql`,
admin/admin, the repository `discover_cases()` and `run_case()`, obclient 2.2.3,
and no prologue. Each passed on its first attempt; no whitespace override was
used. Existing `.result` files are unchanged. Recovery used a fresh private
database, the same initialization and client, and only killed its own child
server. Neither lease duration nor Mock computations were bypassed.

Reproduction commands, from this checkout in PowerShell:

```powershell
& ./.p1-validation/official-20261107/run_guarded.ps1 -Mode link
& ./.p1-validation/official-20261107/run_guarded.ps1 -Mode obclient-fixed
& ./.p1-validation/20261107/compile_cmake_unit.ps1 -Mode native-schema
& ./.p1-validation/official-20261107/run_guarded.ps1 -Mode obclient-recovery
```

The link adapter refuses to overwrite an existing candidate; inspect and reuse
the recorded binary rather than repeating that step. Commands, exits, original
rejects and hashes are under `.p1-validation/official-20261107/`, including
`link.log`, `link-command.json`, `obclient-fixed.log`, `obclient-fixed.status`,
`obclient-recovery.log`, `obclient-recovery.status`, `native-schema.xml`,
`native-schema.log` and `RESULTS.txt`. Original failure evidence remains in
`baseline.log`, `baseline-ai_column_*.reject`, `fixed.log`,
`admin-integration.reject` and `obclient-baseline.log`. Current runtime paths:

- `/home/xiaob/ai-column-validation-f9f9266-20261107/official-parity-20261107-194819-obclient-fixed`
- `/home/xiaob/ai-column-validation-f9f9266-20261107/official-parity-20261107-195009-obclient-recovery`

## Remaining Official Evidence

The repair is a locally validated candidate. The official all-five failure is
still unexplained: the original baseline also passes all five repository suites
with the repository-declared client. The essential next evidence is one official
first failure (SQL/error/reject), plus the tested revision, build/startup commands
and client version. Local passes cannot establish an official score or prove
parity with undisclosed tests or configuration.

No push, official resubmission, paid model, system configuration change, or
unrelated process termination was performed. Private server/Mock processes were
stopped after verification; original workspaces and the frozen FTS version remain
untouched.

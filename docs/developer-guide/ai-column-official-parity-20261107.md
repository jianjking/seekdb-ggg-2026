# AI COLUMN: First Official Failure Investigation

Baseline: `e6a58db30dc0a76ed9eae3927fb02ec6072cf4ee`.
Repair branch: `fix/ai-column-official-20261107`.
Status: confirmed defects repaired; final Worker runtime validation is pending.

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

## Build And Remaining Validation

The changed Worker Unity source and `liboceanbase_static.a` compiled successfully
with the existing Clang 17 CMake configuration. Final LLD linking was terminated
by the Windows memory guard (exit 143); no final runtime pass is claimed.
The status file's stopped=0 belongs to the inner Linux guard; the outer Windows
guard reports the actual stop. A BFD fallback failed with `PHDR segment not
covered by LOAD segment`: CMake supplies an empty `rpm/ld.lds`, which BFD treats
as a replacement layout. The local adapter now omits that empty script for BFD,
but its corrected retry was refused by memory preflight before launch.
No valid new server binary was produced. The prior 29 unit passes are historical,
not a fresh validation of the Worker repair.

Local commands, original exits, rejects, source and package hashes, and private
runner scripts are under `.p1-validation/official-20261107/`. In particular:
`baseline.log`, `baseline-ai_column_*.reject`, `fixed.log`,
`admin-integration.reject`, `obclient-baseline.log`, `server-link-host-guard.log`,
`link-bfd-empty-script.log`, and `link-command.json`. Runtime directories:

- `/home/xiaob/ai-column-validation-f9f9266-20261107/official-parity-20261107-192711-baseline`
- `/home/xiaob/ai-column-validation-f9f9266-20261107/official-parity-20261107-192846-fixed`
- `/home/xiaob/ai-column-validation-f9f9266-20261107/official-parity-20261107-193255-obclient-baseline`

Next: obtain one official failing SQL/reject with build/startup/client details;
with adequate host headroom, link the compiled repair and run the nine suites
using obclient, raw repository initialization, no prologue, and then recovery.
No push, official resubmission, paid model, system configuration change, or
unrelated process termination was performed. This is not a validated submission.

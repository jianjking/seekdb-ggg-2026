# FileSQL SQL Acceptance

These are self-built tests, not the organizer's scoring runner. Every query
executes in the real seekdb server through the MySQL text protocol. PyArrow,
when available, only creates Parquet fixtures.

Use a disposable test instance with an empty-password local root account.
The SQL port defaults to 12881. Set `secure_file_priv` to a directory containing
the fixture directory and keep `enable_file_sql` globally OFF before running.
The security suite verifies that TCP cannot change `secure_file_priv`. Its
`--empty-only` mode requires a separate instance initialized with
`--variable secure_file_priv=`; it never bypasses the local-connection gate.
Run suites serially.

## Windows Commands

From the repository root:

```powershell
$env:TEMP = 'F:/oceanbase-temp'
$env:TMP = $env:TEMP
$env:TMPDIR = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:PYTHONIOENCODING = 'utf-8'

python -B unittest/sql/engine/file_sql/file_sql_acceptance.py `
  --port 12881 --data-dir F:/OceanBase-2026/run/seekdb/files/takeover `
  --output F:/OceanBase-2026/run/takeover_final.json --large-mib 1025

python -B unittest/sql/engine/file_sql/file_sql_security.py `
  --port 12881 --root F:/OceanBase-2026/run `
  --output F:/OceanBase-2026/run/takeover_security.json

python -B unittest/sql/engine/file_sql/file_sql_security.py `
  --port 12882 --root F:/OceanBase-2026/run --empty-only `
  --output F:/OceanBase-2026/run/takeover_security_empty.json

python -B unittest/sql/engine/file_sql/file_sql_regression.py `
  --port 12881 --data-dir F:/OceanBase-2026/run/seekdb/files/takeover `
  --output F:/OceanBase-2026/run/takeover_regression.json
```

Omit `--large-mib` for a quick run. The large scan writes a file larger than
1 GiB and verifies the SQL count; its JSON records size and query duration.
Windows security fixtures use directory junctions. POSIX fixtures use symlinks.
Fixture files and links are retained. The ordinary SQL suite uses a unique
database and drops only that database on completion.

## Coverage

| Requirement | Tests |
| --- | --- |
| SQL interfaces | Five file functions, aliases, named parameters, DESCRIBE |
| CSV | Quoting, escapes, multiline fields, UTF-8/BOM, headers, NULL/empty, size bounds |
| JSONL | Union schema, missing fields, trailing newline, strict strings/numbers |
| Parquet | V1/V2, dictionary/plain, Snappy/NONE, row groups, NULL, date/time, projection |
| Type inference | Full scan, scalar type merging, fractional time, all-NULL columns |
| Native SQL | Filter, aggregate, HAVING, order/limit, derived queries, CTE, windows |
| JOIN | INNER/LEFT and CSV/JSONL/Parquet/physical-table combinations |
| Security/stability | Disabled default, path bounds, junction/symlink, empty privilege, concurrency, mutation during scan, large scan |
| COPY | Query/table, header/empty results, NULL/empty, special strings, readback, no overwrite |
| Regression | Ordinary table queries, DDL/DML, transactions, views, OUTFILE, AI_PROMPT |

`official_test_exact_replay` replays statements from the committed
`file_sql_basic.test`, checks expected errors, and compares the captured output
byte-for-byte with the committed `.result`. It is not an invocation of the
mysqltest program or a substitute for the full competition evaluation.

JSON entries with `passed: true/false` represent actual executions.
`status: NOT_RUN` records unavailable optional fixtures. Build success and
static checks are separate evidence and do not count as SQL test passes.

The mutation test waits for result metadata, rewrites a file of the same size
while a slow SQL scan is active, and accepts either the original stable result
or an explicit data error. A successful mixture of old and new rows fails.
The reader checks size and modification time before and after buffered reads.
This detects ordinary concurrent writes; it is not a snapshot or a guarantee
against a writer that deliberately restores timestamps.

The currently implemented formats are CSV, top-level JSONL objects, and flat
Parquet columns using PLAIN/dictionary encoding with NONE/Snappy compression.
Nested JSON values, nested Parquet schemas, INT96 and other compression codecs
are explicitly unsupported. Positive `sample_size` values are rejected; `-1`
performs full-file inference.

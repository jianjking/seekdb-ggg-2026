"""Self-built FileSQL SQL acceptance, not the organizer's scoring runner.

Run against a local seekdb with secure_file_priv containing --data-dir.
The client uses Python's standard library. Optional pyarrow generates Parquet
fixtures and is never used to evaluate SQL or read the database's results.
"""
import argparse
import base64
import concurrent.futures
import datetime
import json
from pathlib import Path
import sys
import threading
import time

sys.dont_write_bytecode = True


def literal(text):
    return "'" + str(text).replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--large-mib", type=int, default=0)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    from mysql_client import Connection

    def connect():
        return Connection(args.port)
    args.data_dir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    results = []
    conn = connect()

    def fixture(name, text):
        path = args.data_dir / name
        path.write_bytes(text.encode("utf8"))
        return literal(path.as_posix())

    def check(name, sql, rows=None, error=None):
        try:
            actual = conn.query(sql)
            if error is not None:
                passed = actual["kind"] == "ERROR" and actual["code"] in error
            elif rows is None:
                passed = actual["kind"] != "ERROR"
            else:
                passed = actual["kind"] == "RS" and actual["rows"] == rows
            results.append({"name": name, "passed": passed, "sql": sql,
                            "expected_rows": rows, "expected_errors": error,
                            "actual": actual})
        except Exception as exc:
            results.append({"name": name, "passed": False, "sql": sql,
                            "exception": repr(exc)})
        print(("PASS " if results[-1]["passed"] else "FAIL ") + name, flush=True)
        args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf8")

    # Copy official fixtures into the explicitly authorized data directory.
    official = root / "tools/deploy/mysql_test/test_suite/file_sql/data"
    for name in ("basic.csv", "basic.jsonl", "invalid.txt"):
        (args.data_dir / name).write_bytes((official / name).read_bytes())
    (args.data_dir / "simple.parquet").write_bytes(
        base64.b64decode((official / "simple.parquet.b64").read_bytes()))
    basic = literal((args.data_dir / "basic.csv").as_posix())
    jsonl = literal((args.data_dir / "basic.jsonl").as_posix())
    parquet = literal((args.data_dir / "simple.parquet").as_posix())
    check("default_off", "select @@session.enable_file_sql", [["0"]])
    check("disabled_read", f"select count(*) from read_csv({basic})", error=[1235])
    disabled_copy = args.data_dir / ("disabled_" + datetime.datetime.now().strftime("%H%M%S%f") + ".csv")
    check("disabled_copy", f"copy (select 1) to {literal(disabled_copy.as_posix())}", error=[1235])
    check("enable", "set session enable_file_sql=on")
    check("official_csv", f"select id,name,score,active,day,created,"
          f"replace(note,char(10),'<NL>'),id__2 from read_csv({basic}) order by id",
          [["1", "Alice", "98.5", "1", "2026-08-01", "2026-08-01 10:11:12",
            "hello,<NL>world", "101"],
           ["2", "Bob", "87", "0", None, None, "plain", "102"],
           ["3", "Chen", "90.25", "1", "2026-08-03", "2026-08-03 12:13:14",
            'say "hi"', "103"]])
    check("official_csv_aggregate", f"select count(*),sum(id),round(avg(score),2),"
          f"sum(active) from read_csv({basic},header=true,sample_size=-1)",
          [["3", "6", "91.92", "2"]])
    check("official_json", f"select id,name,score,active,day,note from "
          f"read_json({jsonl},format='newline_delimited') order by id",
          [["1", "Alice", "98.5", "1", None, None],
           ["2", "Bob", None, "0", "2026-08-02", None],
           ["3", "Chen", "90", "1", None, "ok"]])
    check("official_parquet", f"select count(*),min(c1),max(c1),sum(c1)"
          f" from read_parquet({parquet})", [["7", "0", "6", "21"]])
    check("invalid_format", f"select * from read_csv({basic},'xml')", error=[1064])
    check("invalid_named_format", f"select * from read_csv({basic},format='xml')", error=[1064])
    check("sample_size", f"select * from read_csv({basic},sample_size=10)", error=[1235])
    check("filter_order_limit", f"select id from read_csv({basic}) where score>88 "
          "order by score desc limit 2", [["1"], ["3"]])
    check("group_having", f"select active,count(*) from read_csv({basic}) "
          "group by active having count(*)>1", [["1", "2"]])
    check("derived_query", f"select name from (select id,name from read_csv({basic})) t "
          "where id=2", [["Bob"]])
    check("cte_query", f"with f as (select id,name from read_csv({basic})) "
          "select name from f where id=3", [["Chen"]])
    check("window_query", f"select id,row_number() over(order by score desc) "
          f"from read_csv({basic}) order by id", [["1", "1"], ["2", "3"], ["3", "2"]])
    check("json_parquet_join", f"select j.id,p.c1 from read_ndjson({jsonl}) j "
          f"left join read_parquet({parquet}) p on j.id=p.c1 order by j.id",
          [["1", "1"], ["2", "2"], ["3", "3"]])
    check("inner_join", f"select c.id,j.name from read_csv({basic}) c join "
          f"read_ndjson({jsonl}) j on c.id=j.id order by c.id",
          [["1", "Alice"], ["2", "Bob"], ["3", "Chen"]])
    check("left_join", f"select c.id,j.name from read_csv({basic}) c left join "
          f"read_ndjson({jsonl}) j on c.id=j.id and j.id>1 order by c.id",
          [["1", None], ["2", "Bob"], ["3", "Chen"]])
    check("csv_parquet_join", f"select c.id,p.c1 from read_csv({basic}) c join "
          f"read_parquet({parquet}) p on c.id=p.c1 order by c.id",
          [["1", "1"], ["2", "2"], ["3", "3"]])
    no_lf = fixture("no_lf.jsonl", '{"id":1,"a":""}\n{"id":2,"last":"yes"}')
    check("json_no_final_newline", f"select count(*),sum(id) from read_ndjson({no_lf})",
          [["2", "3"]])
    check("json_empty_string", f"select length(a),a is null from read_ndjson({no_lf}) "
          "where id=1", [["0", "0"]])
    check("json_nullable_union", f"describe select * from read_ndjson({no_lf})",
          [["id", "BIGINT", "NO", "NULL", "NULL", "NULL"],
           ["a", "VARCHAR", "YES", "NULL", "NULL", "NULL"],
           ["last", "VARCHAR", "YES", "NULL", "NULL", "NULL"]])
    all_null = fixture("all_null.jsonl", '{"id":1,"n":null}\n{"id":2}\n')
    check("json_null_type", f"describe select * from read_ndjson({all_null})",
          [["id", "BIGINT", "NO", "NULL", "NULL", "NULL"],
           ["n", "NULL", "YES", "NULL", "NULL", "NULL"]])
    check("json_null_scan", f"select id,n is null from read_ndjson({all_null}) order by id",
          [["1", "1"], ["2", "1"]])
    csv_all_null = fixture("all_null.csv", "id,n\n1,\n2,\n")
    check("csv_null_type", f"describe select * from read_csv({csv_all_null},header=true)",
          [["id", "BIGINT", "NO", "NULL", "NULL", "NULL"],
           ["n", "NULL", "YES", "NULL", "NULL", "NULL"]])
    check("csv_null_scan", f"select id,n is null from read_csv({csv_all_null},header=true) order by id",
          [["1", "1"], ["2", "1"]])
    weird_header = fixture("weird_header.csv", "a\\b,quote'col\n1,2\n")
    check("describe_header_escaping", f"describe select * from read_csv({weird_header},header=true)",
          [["a\\b", "BIGINT", "NO", "NULL", "NULL", "NULL"],
           ["quote'col", "BIGINT", "NO", "NULL", "NULL", "NULL"]])
    check("describe_projection", f"describe select id as identifier from read_csv({basic})",
          [["identifier", "BIGINT", "NO", "NULL", "NULL", "NULL"]])
    mixed = fixture("mixed.jsonl", '{"a":true,"d":"2026-01-02"}\n'
                    '{"a":2,"d":"2026-01-03 01:02:03.123456"}\n'
                    '{"a":1.5,"d":"text"}\n')
    check("json_date_text_merge", f"select a,d from read_ndjson({mixed})",
          [["1", "2026-01-02"], ["2", "2026-01-03 01:02:03.123456"], ["1.5", "text"]])
    quoted = fixture("quoted.csv", 'id,s\n1,""\n2,\n3,"NULL"\n')
    check("csv_quoted_empty", f"select id,length(s),s is null from read_csv({quoted},header=true)"
          " order by id", [["1", "0", "0"], ["2", None, "1"], ["3", "4", "0"]])
    numeric = fixture("numeric.csv", "a,d\ntrue,2026-01-02\n2,2026-01-03 01:02:03.123456\n")
    check("csv_boolean_numeric_merge", f"select a,d from read_csv({numeric},header=true)",
          [["1", "2026-01-02 00:00:00.000000"], ["2", "2026-01-03 01:02:03.123456"]])
    escaped = fixture("escaped.csv", 'id;s\n1;"hello;世界"\n2;"say ""hi"""\n3;"a\r\nb"\n')
    check("csv_escapes_utf8_crlf", f"select id,s from read_csv({escaped},header=true,delimiter=';')",
          [["1", "hello;世界"], ["2", 'say "hi"'], ["3", "a\nb"]])
    nulls = fixture("nulls.csv", "id,s\n1,MISSING\n2,NULL\n3,\n")
    check("csv_nullstr", f"select id,s,s is null from read_csv({nulls},header=true,nullstr='MISSING')",
          [["1", None, "1"], ["2", "NULL", "0"], ["3", "", "0"]])
    only_empty = fixture("empty_custom_null.csv", "id,s\n1,\n2,\n")
    check("csv_empty_custom_null", f"select id,length(s),s is null from read_csv("
          f"{only_empty},header=true,nullstr='MISSING') order by id",
          [["1", "0", "0"], ["2", "0", "0"]])
    backslash = fixture("backslash_escape.csv", 'id,s\n1,"say \\"hi\\""\n')
    check("csv_custom_escape", f"select id,s from read_csv({backslash},header=true,escape="
          + literal("\\") + ")", [["1", 'say "hi"']])
    bom = fixture("bom.csv", "\ufeffid,s\n1,中文\n")
    check("csv_utf8_bom", f"select id,s from read_csv({bom},header=true)", [["1", "中文"]])
    skipped = fixture("skip.csv", "metadata\nid,s\n1,value\n")
    check("csv_skip_before_header", f"select id,s from read_csv({skipped},header=true,skip_rows=1)",
          [["1", "value"]])
    eof_csv = fixture("eof.csv", "id,s\n1,")
    check("csv_eof_trailing_null", f"select id,s is null from read_csv("
          f"{eof_csv},header=true)", [["1", "1"]])
    extremes = fixture("integer_extremes.csv", "n\n-9223372036854775808\n9223372036854775807\n")
    check("csv_integer_extremes", f"select n from read_csv({extremes},header=true) order by n",
          [["-9223372036854775808"], ["9223372036854775807"]])
    unicode_path = fixture("中文路径.csv", "id,s\n1,文件内容\n")
    check("csv_unicode_filename", f"select s from read_csv({unicode_path},header=true)",
          [["文件内容"]])
    unicode_json = fixture("escaped_unicode.jsonl",
                           '{"id":1,"s":"\\ud83d\\ude00"}\n{"id":2,"s":"\\u4e2d\\u6587"}\n')
    check("json_unicode_escapes", f"select id,s from read_ndjson({unicode_json}) order by id",
          [["1", "\U0001f600"], ["2", "中文"]])
    duplicate_json = fixture("duplicate.jsonl", '{"id":0,"id":2,"s":"x"}\n')
    check("json_duplicate_key_last_value", f"select id,s from read_ndjson({duplicate_json})",
          [["2", "x"]])
    widened_json = fixture("widened.jsonl", '{"a":true}\n{"a":1.5}\n{"a":"text"}\n')
    check("json_scalar_varchar_merge", f"select a from read_ndjson({widened_json})",
          [["true"], ["1.5"], ["text"]])
    ragged_csv = fixture("ragged.csv", "id,s\n1,a,extra\n")
    check("csv_wrong_field_count", f"select * from read_csv({ragged_csv},header=true)", error=[4070])
    check("csv_size_limit", f"select * from read_csv({basic},max_object_size=8)", error=[4019])
    invalid_utf8 = args.data_dir / "invalid_utf8.csv"
    invalid_utf8.write_bytes(b"id,s\n1,\xc0\x80\n")
    check("csv_invalid_utf8", f"select * from read_csv({literal(invalid_utf8.as_posix())},header=true)",
          error=[4070])
    malformed = fixture("malformed.csv", 'id,s\n1,"unterminated')
    check("csv_malformed", f"select * from read_csv({malformed},header=true)", error=[4070])
    for label, text in [("leading_zero", '{"a":01}\n'),
                        ("missing_fraction", '{"a":1.}\n'),
                        ("missing_exponent", '{"a":1e}\n'),
                        ("surrogate", '{"a":"\\ud800"}\n'),
                        ("control", '{"a":"bad\ttext"}\n')]:
        path = fixture(label + ".jsonl", text)
        check("json_invalid_" + label, f"select * from read_ndjson({path})", error=[4070])
    for suffix in ("missing.csv", "../outside.csv"):
        path = literal((args.data_dir / suffix).as_posix())
        if suffix.startswith(".."):
            # Parent still lives inside secure_file_priv on normal runs.
            continue
        check("missing_file", f"select * from read_csv({path})", error=[1017])
    check("outside_read", "select * from read_csv('C:/Windows/win.ini')", error=[1227, 1017])
    check("traversal_read", f"select * from read_csv("
          f"{literal(args.data_dir.as_posix() + '/../../../../../../Windows/win.ini')})",
          error=[1227, 1017])
    check("ordinary_sql", "select sum(n) from (select 1 n union all select 2) t", [["3"]])
    check("glob_count_without_column", f"select count(*) from glob("
          f"{literal(args.data_dir.as_posix() + '/basic.csv')})", [["1"]])
    bad_footer = args.data_dir / "bad_footer.parquet"
    footer = b"\x1b\xff\xff\xff\xff\xff\xff\xff\xff\x7f\x11\x00"
    bad_footer.write_bytes(b"PAR1" + footer + len(footer).to_bytes(4, "little") + b"PAR1")
    check("parquet_bad_footer_bound", f"select * from read_parquet({literal(bad_footer.as_posix())})",
          error=[4070])
    check("ai_prompt", "select json_extract(ai_prompt('{0}', 'hello'),'$.args[0]')", [['"hello"']])
    export = args.data_dir / ("export_" + datetime.datetime.now().strftime("%H%M%S%f") + ".csv")
    check("copy_csv", f"copy (select id,name,score from read_csv({basic}) order by id)"
          f" to {literal(export.as_posix())} (format csv,header,nullstr 'NULL')")
    check("copy_roundtrip", f"select id,name,score from read_csv({literal(export.as_posix())})",
          [["1", "Alice", "98.5"], ["2", "Bob", "87"], ["3", "Chen", "90.25"]])
    check("copy_no_overwrite", f"copy (select 1) to {literal(export.as_posix())} (format csv)",
          error=[1086])
    escaped_export = args.data_dir / ("escaped_export_" + datetime.datetime.now().strftime("%H%M%S%f") + ".csv")
    check("copy_special_values", "copy (select 'a,b' as name,'say \"hi\"' as note,"
          f"cast(null as char) as absent,'' as empty) to {literal(escaped_export.as_posix())}"
          " (format csv,header,nullstr 'NULL')")
    check("copy_special_roundtrip", f"select name,note,absent,empty from read_csv("
          f"{literal(escaped_export.as_posix())},header=true,nullstr='NULL')",
          [["a,b", 'say "hi"', None, ""]])
    extra_export = args.data_dir / ("copy_extra_" + datetime.datetime.now().strftime("%H%M%S%f") + ".csv")
    check("copy_default_no_header", "copy (select concat('a',char(10),'b') as x,"
          "'NULL' as y,cast(null as char) as z,'' as e)"
          f" to {literal(extra_export.as_posix())}")
    check("copy_default_roundtrip", f"select * from read_csv({literal(extra_export.as_posix())},"
          "header=false)", [["a\nb", "NULL", None, ""]])
    empty_export = args.data_dir / ("copy_empty_" + datetime.datetime.now().strftime("%H%M%S%f") + ".csv")
    check("copy_empty_header", f"copy (select 1 as id where false) to "
          f"{literal(empty_export.as_posix())} (format csv,header)")
    check("copy_empty_roundtrip", f"select count(*) from read_csv("
          f"{literal(empty_export.as_posix())},header=true)", [["0"]])
    check("copy_outside", "copy (select 1) to 'C:/Windows/filesql_disallowed.csv' (format csv)",
          error=[9731, 1227])
    changing = args.data_dir / "changing.csv"
    same_sql = f"select sum(id) from read_csv({literal(changing.as_posix())},header=true)"
    changing.write_bytes(b"id\n1\n")
    check("file_change_before", same_sql, [["1"]])
    changing.write_bytes(b"id\n2\n3\n")
    check("file_change_after", same_sql, [["5"]])
    active_change = args.data_dir / "active_change.csv"
    payload = "x" * 1024
    original_rows = "".join(f"{i:04d},{payload}\n" for i in range(1, 601))
    updated_rows = "".join(f"{i + 6000:04d},{payload}\n" for i in range(1, 601))
    active_change.write_text("id,payload\n" + original_rows, encoding="utf8", newline="")
    columns_ready = threading.Event()

    def scan_during_change():
        active_conn = connect()
        try:
            active_conn.query("set session enable_file_sql=on")
            return active_conn.query(f"select id,payload,sleep(0.002) from read_csv("
                                     f"{literal(active_change.as_posix())},header=true)",
                                     on_columns=columns_ready.set)
        finally:
            active_conn.s.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        pending = pool.submit(scan_during_change)
        ready = columns_ready.wait(timeout=10)
        if ready:
            time.sleep(0.05)
            active_change.write_text("id,payload\n" + updated_rows, encoding="utf8", newline="")
        mutation_result = pending.result(timeout=60)
    ids = [row[0] for row in mutation_result.get("rows", [])]
    stable_snapshot = ids == [str(i) for i in range(1, 601)]
    rejected = mutation_result.get("kind") == "ERROR" and mutation_result.get("code") == 4070
    results.append({"name": "file_change_during_scan", "passed": ready and (rejected or stable_snapshot),
                    "actual": {"kind": mutation_result.get("kind"), "code": mutation_result.get("code"),
                               "row_count": len(ids), "first_ids": ids[:3], "last_ids": ids[-3:],
                               "partial_rows": len(mutation_result.get("partial_rows", []))},
                    "expected": "stable original snapshot or explicit data error"})
    print(("PASS " if results[-1]["passed"] else "FAIL ") + "file_change_during_scan", flush=True)
    official_dir = args.data_dir / ("official_" + datetime.datetime.now().strftime("%H%M%S%f"))
    official_dir.mkdir()
    for name in ("basic.csv", "basic.jsonl", "invalid.txt"):
        (official_dir / name).write_bytes((official / name).read_bytes())
    (official_dir / "simple.parquet").write_bytes(
        base64.b64decode((official / "simple.parquet.b64").read_bytes()))
    official_conn = connect()
    transcript, official_errors, statement = [], [], []
    capture = True
    expected_error = None
    for line in (official.parent / "t/file_sql_basic.test").read_text(encoding="utf8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or stripped.startswith("let "):
            continue
        if stripped.startswith("--"):
            directive = stripped[2:].split(" ", 1)
            if directive[0] == "echo":
                transcript.append(directive[1])
            elif directive[0] == "disable_result_log":
                capture = False
            elif directive[0] == "enable_result_log":
                capture = True
            elif directive[0] == "error":
                expected_error = [int(v) for v in directive[1].split(",")]
            continue
        statement.append(line)
        if stripped.endswith(";"):
            sql = "\n".join(statement).strip().removesuffix(";")
            if sql.startswith("eval "):
                sql = sql[5:]
            sql = sql.replace("$FILE_SQL_DATA_DIR", official_dir.as_posix())
            actual = official_conn.query(sql)
            if expected_error is not None:
                if actual["kind"] != "ERROR" or actual["code"] not in expected_error:
                    official_errors.append({"sql": sql, "actual": actual})
            elif actual["kind"] == "ERROR":
                official_errors.append({"sql": sql, "actual": actual})
            elif capture and actual["kind"] == "RS":
                transcript.append("\t".join(actual["columns"]))
                transcript.extend("\t".join("NULL" if v is None else v for v in row)
                                  for row in actual["rows"])
            statement, expected_error = [], None
    official_conn.s.close()
    expected = (official.parent / "r/mysql/file_sql_basic.result").read_text(encoding="utf8")
    official_actual = "\n".join(transcript) + "\n"
    results.append({"name": "official_test_exact_replay", "passed": (
        official_actual == expected and not official_errors), "errors": official_errors,
        "actual_transcript": official_actual, "expected_transcript": expected,
        "runner": "equivalent SQL replay; mysqltest framework is not installed"})
    print(("PASS " if results[-1]["passed"] else "FAIL ") + "official_test_exact_replay", flush=True)
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
        table = pa.table({"id": pa.array([1, 2, 3, 4, 5], pa.int64()),
                          "name": ["alpha", None, "", "中文", "alpha"],
                          "active": [True, False, None, True, False],
                          "score": [1.5, None, 3.25, 4.0, 5.0],
                          "day": pa.array([datetime.date(2026, 1, 2)] * 5, pa.date32()),
                          "created": pa.array([datetime.datetime(2026, 1, 2, 1, 2, 3, 123456)] * 5,
                                              pa.timestamp("us"))})
        for version in ("1.0", "2.0"):
            for dictionary in (False, True):
                for compression in ("NONE", "snappy"):
                    name = f"multi_{version}_{dictionary}_{compression}.parquet"
                    path = args.data_dir / name
                    pq.write_table(table, path, compression=compression,
                                   use_dictionary=dictionary, data_page_version=version,
                                   row_group_size=2)
                    source = f"read_parquet({literal(path.as_posix())})"
                    check(name, f"select id,name,active,score,day,created from {source} order by id",
                          [[str(i + 1), table["name"][i].as_py(),
                            [ "1", "0", None, "1", "0"][i],
                            ["1.5", None, "3.25", "4", "5"][i],
                            "2026-01-02", "2026-01-02 01:02:03.123456"] for i in range(5)])
                    check(name + "_projection", f"select sum(id) from {source}", [["15"]])
        empty = args.data_dir / "empty.parquet"
        pq.write_table(table.slice(0, 0), empty)
        check("empty_parquet", f"select count(*) from read_parquet({literal(empty.as_posix())})", [["0"]])
        projection = args.data_dir / "projection.parquet"
        pq.write_table(pa.table({"id": [1, 2, 3], "unused": ["a", "b", "c"]}),
                       projection, compression={"id": "NONE", "unused": "gzip"})
        check("parquet_skips_unused_codec", f"select sum(id) from read_parquet("
              f"{literal(projection.as_posix())})", [["6"]])
        check("parquet_projection_predicate", f"select id from read_parquet("
              f"{literal(projection.as_posix())}) where unused='a'", error=[1235])
        check("parquet_count_without_decode", f"select count(*) from read_parquet("
              f"{literal(projection.as_posix())})", [["3"]])
        primitive = args.data_dir / "primitive.parquet"
        primitive_table = pa.table({
            "id": pa.array([-2147483648, None, 2147483647], pa.int32()),
            "f": pa.array([1.25, None, -2.5], pa.float32()),
            "s": pa.array([b"abcd", None, b"wxyz"], pa.binary(4)),
            "ts": pa.array([datetime.datetime(2026, 1, 2, 1, 2, 3, 456000),
                            None, datetime.datetime(1969, 12, 31, 23, 59, 59, 123000)],
                           pa.timestamp("ms"))})
        pq.write_table(primitive_table, primitive, compression="snappy",
                       use_dictionary=False, row_group_size=2)
        check("parquet_primitive_types", f"select id,f,s,ts from read_parquet("
              f"{literal(primitive.as_posix())})",
              [["-2147483648", "1.25", "abcd", "2026-01-02 01:02:03.456"],
               [None, None, None, None],
               ["2147483647", "-2.5", "wxyz", "1969-12-31 23:59:59.123"]])
        check("parquet_unused_codec_rejected", f"select * from read_parquet("
              f"{literal(projection.as_posix())})", error=[1235])
        check("parquet_size_limit", f"select * from read_parquet({parquet},max_object_size=32)",
              error=[4019, 4070])
    except ImportError:
        results.append({"name": "pyarrow_fixtures", "status": "NOT_RUN",
                        "reason": "pyarrow is unavailable"})

    def parallel_scan(_):
        c = None
        try:
            c = connect()
            c.query("set session enable_file_sql=on")
            return c.query(f"select sum(id) from read_csv({basic})")
        except Exception as exc:
            return {"exception": repr(exc)}
        finally:
            if c is None:
                pass
            elif hasattr(c, "s"):
                c.s.close()
            else:
                c.conn.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        scans = list(pool.map(parallel_scan, range(12)))
    results.append({"name": "concurrent_scans", "passed": all(
        r.get("rows") == [["6"]] for r in scans), "actual": scans})
    if args.large_mib:
        conn.s.settimeout(660)
        check("large_timeout", "set ob_query_timeout=600000000")
        large = args.data_dir / ("large_" + str(args.large_mib) + ".csv")
        block = b"x" * (1024 * 1024 - 1) + b"\n"
        with large.open("wb") as output:
            output.write(b"payload\n")
            for _ in range(args.large_mib):
                output.write(block)
        started = time.monotonic()
        check("large_streaming_scan", f"select count(*) from read_csv("
              f"{literal(large.as_posix())},header=true)", [[str(args.large_mib)]])
        results[-1]["file_size_bytes"] = large.stat().st_size
        results[-1]["elapsed_seconds"] = time.monotonic() - started
    check("server_alive", "select 1", [["1"]])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf8")
    failures = [r["name"] for r in results if r.get("passed") is False]
    print(json.dumps({"passed": sum(r.get("passed") is True for r in results),
                      "failed": failures, "not_run": [r["name"] for r in results
                                                     if r.get("status") == "NOT_RUN"]}))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())

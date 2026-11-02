"""Self-built path-security checks against a real seekdb SQL server.

Creates only uniquely named fixtures below --root (use an F-drive directory).
Directory links and all generated files are retained as reproducible evidence.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

from mysql_client import Connection


def literal(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=12881)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--empty-only", action="store_true",
                        help="Test an isolated instance initialized with --variable secure_file_priv=")
    args = parser.parse_args()
    results = []
    conn = Connection(args.port)
    priv = conn.query("select @@global.secure_file_priv")
    if priv.get("kind") != "RS":
        raise RuntimeError(priv)
    original = priv["rows"][0][0]
    token = datetime.datetime.now().strftime("%Y%m%d_%H%M%S%f")
    secure = Path(original) if original else args.root
    inside = secure / ("security_" + token)
    outside = args.root / ("outside_" + token)
    inside.mkdir(parents=True)
    outside.mkdir(parents=True)
    (outside / "secret.csv").write_bytes(b"id\n42\n")
    (inside / "allowed.csv").write_bytes(b"id\n7\n")
    link = inside / "escape"

    def record(name, actual, passed):
        results.append({"name": name, "passed": passed, "actual": actual})
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf8")
        print(("PASS " if passed else "FAIL ") + name, flush=True)

    def check(name, sql, rows=None, denied=False):
        actual = conn.query(sql)
        passed = (actual.get("kind") == "ERROR" and actual.get("code") in (1227, 9731)) if denied else (
            actual.get("kind") != "ERROR" if rows is None else actual.get("rows") == rows)
        record(name, {"sql": sql, "result": actual}, passed)

    if args.empty_only:
        record("empty_priv_configured", priv, original == "")
        if original != "":
            conn.s.close()
            return 1
        check("enable", "set session enable_file_sql=on")
        check("empty_priv_read", f"select * from read_csv("
              f"{literal((inside / 'allowed.csv').as_posix())})", denied=True)
        check("empty_priv_glob", f"select * from glob("
              f"{literal(inside.as_posix() + '/*')})", denied=True)
        check("empty_priv_copy", f"copy (select 1) to "
              f"{literal((inside / 'denied.csv').as_posix())}", denied=True)
        record("empty_priv_copy_no_file", {"target": str(inside / "denied.csv")},
               not (inside / "denied.csv").exists())
        check("server_alive", "select 1", [["1"]])
        conn.s.close()
        failed = [r["name"] for r in results if not r["passed"]]
        print(json.dumps({"passed": len(results) - len(failed), "failed": failed}), flush=True)
        return bool(failed)

    if os.name == "nt":
        created = subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(outside)],
                                 capture_output=True, text=True)
        record("junction_fixture", {"exit_code": created.returncode,
               "stdout": created.stdout, "stderr": created.stderr},
               created.returncode == 0 and (link / "secret.csv").is_file())
    else:
        link.symlink_to(outside, target_is_directory=True)
    check("enable", "set session enable_file_sql=on")
    check("allowed", f"select id from read_csv({literal((inside / 'allowed.csv').as_posix())})",
          [["7"]])
    check("junction_read", f"select * from read_csv({literal((link / 'secret.csv').as_posix())})",
          denied=True)
    check("junction_glob", f"select * from glob({literal(link.as_posix() + '/*')})", denied=True)
    check("junction_copy", f"copy (select 'protected') to {literal((link / 'export.csv').as_posix())}",
          denied=True)
    record("junction_copy_no_file", {"outside_target": str(outside / "export.csv")},
           not (outside / "export.csv").exists())
    check("backslash_read", f"select * from read_csv({literal(str(link / 'secret.csv'))})",
          denied=True)
    check("backslash_copy", f"copy (select 1) to {literal(str(link / 'backslash.csv'))}", denied=True)
    record("backslash_copy_no_file", {"outside_target": str(outside / "backslash.csv")},
           not (outside / "backslash.csv").exists())
    sibling = secure.parent / (secure.name + "_sibling_" + token)
    sibling.mkdir()
    (sibling / "data.csv").write_bytes(b"id\n9\n")
    check("sibling_prefix_read", f"select * from read_csv("
          f"{literal((sibling / 'data.csv').as_posix())})", denied=True)
    check("traversal_read", f"select * from read_csv("
          f"{literal(inside.as_posix() + '/../../' + sibling.name + '/data.csv')})", denied=True)
    actual = conn.query("set global secure_file_priv=''")
    record("tcp_priv_change_rejected", actual,
           actual.get("kind") == "ERROR" and actual.get("code") == 1235)
    check("priv_unchanged", "select @@global.secure_file_priv", [[original]])
    check("server_alive", "select 1", [["1"]])
    conn.s.close()
    failed = [r["name"] for r in results if not r["passed"]]
    print(json.dumps({"passed": len(results) - len(failed), "failed": failed}), flush=True)
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())

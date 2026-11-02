"""Self-built ordinary SQL and mixed file/physical-table regression."""
import argparse
import datetime
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from mysql_client import Connection


def literal(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=12881)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    conn = Connection(args.port)
    results = []
    token = datetime.datetime.now().strftime("%Y%m%d%H%M%S%f")
    database = "filesql_reg_" + token
    args.data_dir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def check(name, sql, rows=None):
        actual = conn.query(sql)
        passed = actual.get("kind") != "ERROR" and (
            rows is None or actual.get("rows") == rows)
        results.append({"name": name, "passed": passed, "sql": sql, "actual": actual,
                        "expected": rows})
        args.output.write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf8")
        print(("PASS " if passed else "FAIL ") + name, flush=True)

    check("create_database", "create database " + database)
    check("use_database", "use " + database)
    try:
        check("create_t1", "create table t1(id int primary key,v varchar(32),n double,d date)")
        check("insert_t1", "insert into t1 values (1,'a',1.5,'2026-01-01'),"
              "(2,'b',2.5,'2026-01-02'),(3,'c',3.5,'2026-01-03')")
        check("create_t2", "create table t2(id int,tag varchar(16))")
        check("insert_t2", "insert into t2 values (1,'x'),(2,'y'),(4,'z')")
        check("count", "select count(*) from t1", [["3"]])
        check("describe", "describe t1")
        check("desc", "desc t1")
        check("explain", "explain select * from t1 where id>1")
        check("inner_join", "select a.id,b.tag from t1 a join t2 b on a.id=b.id order by a.id",
              [["1", "x"], ["2", "y"]])
        check("left_join", "select a.id,b.tag from t1 a left join t2 b on a.id=b.id order by a.id",
              [["1", "x"], ["2", "y"], ["3", None]])
        check("union", "select id from t1 union select id from t2 order by id",
              [["1"], ["2"], ["3"], ["4"]])
        check("cte", "with c as(select id,v from t1 where id>1) select * from c order by id",
              [["2", "b"], ["3", "c"]])
        check("window", "select id,row_number() over(order by n desc) from t1 order by id",
              [["1", "3"], ["2", "2"], ["3", "1"]])
        check("group_having", "select tag,count(*) from t2 group by tag having count(*)=1 order by tag",
              [["x", "1"], ["y", "1"], ["z", "1"]])
        check("json", "select json_extract(json_object('k',1),'$.k')", [["1"]])
        check("derived", "select * from(select id,v from t1)s where id=2", [["2", "b"]])
        check("begin", "begin")
        check("update", "update t1 set v='changed' where id=1")
        check("updated_value", "select v from t1 where id=1", [["changed"]])
        check("rollback", "rollback")
        check("rollback_value", "select v from t1 where id=1", [["a"]])
        check("create_view", "create view v1 as select id,v from t1")
        check("view", "select count(*) from v1", [["3"]])
        check("ordinary_outfile", "select id,v from t1 order by id into outfile "
              + literal((args.data_dir / ("ordinary_" + token + ".txt")).as_posix()))
        ordinary = args.data_dir / ("ordinary_" + token + ".txt")
        results.append({"name": "ordinary_outfile_bytes", "passed":
                        ordinary.read_bytes() == b"1\ta\n2\tb\n3\tc\n",
                        "actual": ordinary.read_text(encoding="utf8")})
        check("enable", "set session enable_file_sql=on")
        copy = args.data_dir / ("table_copy_" + token + ".csv")
        check("copy_table", f"copy t1 to {literal(copy.as_posix())} (format csv,header)")
        check("copy_table_read", f"select id,v,n,d from read_csv({literal(copy.as_posix())}) order by id",
              [["1", "a", "1.5", "2026-01-01"], ["2", "b", "2.5", "2026-01-02"],
               ["3", "c", "3.5", "2026-01-03"]])
        check("mixed_physical_join", f"select t.id,f.v from t2 t left join "
              f"read_csv({literal(copy.as_posix())}) f on t.id=f.id order by t.id",
              [["1", "a"], ["2", "b"], ["4", None]])
        check("disable", "set session enable_file_sql=off")
        check("physical_after_disable", "select sum(id) from t1", [["6"]])
        check("ai_prompt", "select json_extract(ai_prompt('{0}-{1}','a','b'),'$.template')",
              [['"{0}-{1}"']])
    finally:
        check("drop_own_database", "drop database " + database)
    check("server_alive", "select 1", [["1"]])
    args.output.write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf8")
    conn.s.close()
    failures = [r["name"] for r in results if not r["passed"]]
    print(json.dumps({"passed": len(results) - len(failures), "failed": failures}), flush=True)
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Exercise FTS token ownership, row reuse, ranking, and DML on a test instance."""

import argparse
import json
import math
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = {}

    def sql(statement):
        process = subprocess.run(
            [args.client, "-h127.0.0.1", "-P" + str(args.port), "-uroot",
             "-N", "-s", "--default-character-set=utf8mb4",
             "-e", "SET NAMES utf8mb4; USE fts_regression; " + statement],
            capture_output=True, text=True,
        )
        if process.returncode:
            raise RuntimeError(process.stderr)
        return process.stdout.strip()

    subprocess.run(
        [args.client, "-h127.0.0.1", "-P" + str(args.port), "-uroot",
         "-e", "CREATE DATABASE fts_regression"],
        check=True, capture_output=True,
    )
    for text, expected in [
        ("alpha alpha beta ALPHA gamma beta", ["alpha", "beta", "gamma"]),
        ("the or", []),
        ("", []),
        ("echo " * 4096 + "tailmarker", ["echo", "tailmarker"]),
    ]:
        tokens = json.loads(sql("SELECT tokenize('%s', 'beng')" % text))
        assert sorted(tokens) == expected, (text[:80], tokens, expected)
        results["tokens_" + str(len(results))] = sorted(tokens)
    chinese = sql("SELECT tokenize('全文索引 分词器 数据库 OceanBase', 'ik')")
    results["tokens_ik"] = sorted(json.loads(chinese))
    assert "全文索引" in results["tokens_ik"]
    for text, expected in [
        ("alpha alpha beta ALPHA gamma beta", {"alpha": 3, "beta": 2, "gamma": 1}),
        ("echo " * 4096 + "tailmarker", {"echo": 4096, "tailmarker": 1}),
    ]:
        detail = json.loads(sql(
            "SELECT tokenize('%s', 'beng', '[{\"output\":\"all\"}]')" % text))
        counts = {word: count for token in detail["tokens"]
                  for word, count in token.items()}
        assert counts == expected, counts
        assert detail["doc_len"] == sum(expected.values()), detail
        results["frequencies_" + str(len(results))] = counts

    sql("CREATE TABLE docs(id INT PRIMARY KEY, content TEXT)")
    documents = [
        (1, "alpha alpha alpha beta"), (2, "alpha beta beta beta"),
        (3, "gamma delta"), (4, ""), (5, "the and or"),
        (6, "alpha"), (7, "zeta " * 2048 + "tailmarker"),
        (8, "beta"), (9, "alpha beta"), (10, "   "),
    ]
    sql("INSERT INTO docs VALUES " + ",".join(
        "(%d,'%s')" % row for row in documents))
    sql("ALTER TABLE docs ADD FULLTEXT INDEX ft(content) WITH PARSER beng")

    def matched(query):
        return sql("SELECT id FROM docs WHERE MATCH(content) AGAINST('%s') "
                   "ORDER BY id" % query)

    assert matched("alpha") == "1\n2\n6\n9"
    assert matched("beta") == "1\n2\n8\n9"
    assert matched("gamma") == "3"
    assert matched("tailmarker") == "7"
    assert matched("nonexistent") == ""
    results["match_alpha"] = matched("alpha")

    def ranking():
        output = sql(
            "SELECT id, MATCH(content) AGAINST('alpha') AS score FROM docs "
            "WHERE MATCH(content) AGAINST('alpha') ORDER BY score DESC,id")
        return [[int(line.split("\t")[0]), float(line.split("\t")[1])]
                for line in output.splitlines()]

    ranks = ranking()
    assert len(ranks) == 4
    assert all(math.isfinite(score) and score > 0 for _, score in ranks)
    scores = dict(ranks)
    lengths = {1: 4, 2: 4, 6: 1, 9: 2}
    frequencies = {1: 3, 2: 1, 6: 1, 9: 1}
    # Ten documents, 2064 retained tokens, four documents containing alpha.
    idf = max(0.25, math.log((10 - 4 + 0.5) / (4 + 0.5)))
    for doc_id, score in ranks:
        tf = frequencies[doc_id]
        expected = idf * 2.2 * tf / (
            tf + 1.2 * (0.25 + 0.75 * lengths[doc_id] / (2064 / 10)))
        assert math.isclose(score, expected, rel_tol=1e-12), (doc_id, score, expected)
    # Equal document length: the document with three occurrences must rank higher.
    assert scores[1] > scores[2], ranks
    # Equal term frequency: the shorter document must rank higher.
    assert scores[6] > scores[9] > scores[2], ranks
    results["bm25"] = ranks
    for limit in (0, 1, 2, 4, 20):
        output = sql(
            "SELECT id FROM docs WHERE MATCH(content) AGAINST('alpha') "
            "ORDER BY MATCH(content) AGAINST('alpha') DESC,id LIMIT %d" % limit)
        assert output == "\n".join(str(row[0]) for row in ranks[:limit])
    results["limits"] = "passed"
    offset = sql(
        "SELECT id FROM docs WHERE MATCH(content) AGAINST('alpha') "
        "ORDER BY MATCH(content) AGAINST('alpha') DESC,id LIMIT 1,2")
    assert offset == "\n".join(str(row[0]) for row in ranks[1:3])
    def concurrent_check(_):
        detail = json.loads(sql(
            "SELECT tokenize('alpha alpha beta ALPHA gamma beta', 'beng', "
            "'[{\"output\":\"all\"}]')"))
        counts = {word: count for token in detail["tokens"]
                  for word, count in token.items()}
        assert counts == {"alpha": 3, "beta": 2, "gamma": 1}
        assert ranking() == ranks

    with ThreadPoolExecutor(max_workers=4) as workers:
        list(workers.map(concurrent_check, range(32)))
    results["concurrent"] = "passed"

    sql("INSERT INTO docs VALUES (11,'freshmarker alpha alpha'),"
        "(12,''),(13,'secondmarker beta beta'),(14,'the and or')")
    assert matched("freshmarker") == "11"
    assert matched("secondmarker") == "13"
    sql("UPDATE docs SET content='replacementmarker beta' WHERE id=11")
    assert matched("freshmarker") == ""
    assert matched("replacementmarker") == "11"
    sql("DELETE FROM docs WHERE id=13")
    assert matched("secondmarker") == ""
    sql("START TRANSACTION; INSERT INTO docs VALUES (15,'rollbackmarker'); ROLLBACK")
    assert matched("rollbackmarker") == ""
    results["dml"] = "passed"
    sql("CREATE TABLE ikdocs(id INT PRIMARY KEY, content TEXT)")
    sql("INSERT INTO ikdocs VALUES (1,'全文索引 分词器 数据库'),"
        "(2,''),(3,'分词器 分词器'),(4,'无关内容')")
    sql("ALTER TABLE ikdocs ADD FULLTEXT INDEX ft(content) WITH PARSER ik")
    results["match_ik"] = sql(
        "SELECT id, MATCH(content) AGAINST('分词器') AS score FROM ikdocs "
        "WHERE MATCH(content) AGAINST('分词器') ORDER BY score DESC,id")
    assert len(results["match_ik"].splitlines()) == 2
    args.output.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": "passed", "output": str(args.output)}), flush=True)


if __name__ == "__main__":
    main()

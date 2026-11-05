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
    parser.add_argument("--allow-known-baseline-topk-error", action="store_true",
                        help="Record the reproduced original BMW 4016 defect; never use for candidates")
    args = parser.parse_args()
    results = {}
    known_errors = []

    def sql(statement):
        process = subprocess.run(
            [args.client, "-h127.0.0.1", "-P" + str(args.port), "-uroot",
             "-N", "-s", "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; USE fts_regression; " + statement + ";\n",
            capture_output=True, text=True,
        )
        if process.returncode:
            raise RuntimeError(process.stderr + "\nSQL: " + statement)
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
    dynamic = sql(
        "SELECT tokenize(txt, parser, opts) FROM ("
        "SELECT 1 id, 'alpha alpha beta' txt, 'beng' parser, '[]' opts UNION ALL "
        "SELECT 2, 'alpha alpha beta', 'beng', '[{\"output\":\"all\"}]' UNION ALL "
        "SELECT 3, 'gamma gamma', 'space', '[]') t ORDER BY id")
    dynamic_rows = [json.loads(row) for row in dynamic.splitlines()]
    assert sorted(dynamic_rows[0]) == ["alpha", "beta"]
    assert dynamic_rows[1]["doc_len"] == 3
    assert dynamic_rows[2] == ["gamma"]
    prepared = sql(
        "PREPARE tokenize_stmt FROM 'SELECT tokenize(?, ?, ?)'; "
        "SET @txt='alpha alpha beta', @parser='beng', @opts='[]'; "
        "EXECUTE tokenize_stmt USING @txt, @parser, @opts; "
        "SET @txt='gamma gamma', @parser='space', @opts='[{\"output\":\"all\"}]'; "
        "EXECUTE tokenize_stmt USING @txt, @parser, @opts; "
        "DEALLOCATE PREPARE tokenize_stmt")
    prepared_rows = [json.loads(row) for row in prepared.splitlines()]
    assert sorted(prepared_rows[0]) == ["alpha", "beta"]
    assert prepared_rows[1]["doc_len"] == 2
    assert prepared_rows[1]["tokens"] == [{"gamma": 2}]
    constant_config = sql(
        "SELECT tokenize(txt, 'beng', '[{\"output\":\"all\"}]') FROM ("
        "SELECT 1 id, 'alpha alpha beta' txt UNION ALL SELECT 2, 'gamma gamma' "
        "UNION ALL SELECT 3, '' UNION ALL SELECT 4, NULL) t ORDER BY id")
    config_rows = [json.loads(row) for row in constant_config.splitlines()]
    assert [row["doc_len"] for row in config_rows] == [3, 2, 0, 0]
    assert config_rows[1]["tokens"] == [{"gamma": 2}]
    results["tokenize_dynamic_and_prepared"] = "passed"
    for parser_name, text, options, expected in [
        ("space", "hello world", "[]", ["hello", "world"]),
        ("ngram", "abcd", "[]", ["ab", "bc", "cd"]),
        ("ngram2", "abcd", "[]", ["ab", "abc", "bc", "bcd", "cd"]),
        ("beng", "alpha beta gamma",
         '[{"additional_args":[{"min_token_size":5},{"max_token_size":10}]}]', ["alpha", "gamma"]),
        ("ngram", "abcd", '[{"additional_args":[{"ngram_token_size":3}]}]', ["abc", "bcd"]),
    ]:
        tokens = json.loads(sql("SELECT tokenize('%s', '%s', '%s')"
                                % (text, parser_name, options)))
        assert sorted(tokens) == expected, (parser_name, options, tokens)
        results["parser_" + parser_name + "_" + options] = sorted(tokens)
    for options in (
        '[{"additional_args":[{"min_token_size":8},{"max_token_size":2}]}]',
        '[{"additional_args":[{"unknown_option":1}]}]',
        '[{"output":"invalid"}]',
    ):
        try:
            sql("SELECT tokenize('alpha beta', 'beng', '%s')" % options)
        except RuntimeError as error:
            assert "ERROR " in str(error)
        else:
            raise AssertionError("Invalid parser options accepted: " + options)
    results["parser_invalid_options"] = "rejected"
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
    term_counts = {
        doc_id: {term: text.lower().split().count(term)
                 for term in ("alpha", "beta", "gamma", "delta", "zeta", "tailmarker")}
        for doc_id, text in documents
    }
    doc_lengths = {doc_id: sum(counts.values()) for doc_id, counts in term_counts.items()}
    for query in (
        "alpha beta", "beta alpha", "alpha beta gamma",
        "alpha beta gamma delta", "alpha beta gamma delta zeta tailmarker",
        "alpha missingword", "missingword alpha", "missingword othermissing",
        "alpha beta gamma delta zeta tailmarker missingword othermissing ninthterm",
    ):
        expected_scores = {}
        for doc_id, counts in term_counts.items():
            expected = 0.0
            for term in query.split():
                tf = counts.get(term, 0)
                if tf:
                    df = sum(row.get(term, 0) > 0 for row in term_counts.values())
                    term_idf = max(0.25, math.log((10 - df + 0.5) / (df + 0.5)))
                    expected += term_idf * 2.2 * tf / (
                        tf + 1.2 * (0.25 + 0.75 * doc_lengths[doc_id] / (2064 / 10)))
            if expected:
                expected_scores[doc_id] = expected
        output = sql(
            "SELECT id, MATCH(content) AGAINST('%s') AS score FROM docs "
            "WHERE MATCH(content) AGAINST('%s') ORDER BY id" % (query, query))
        actual = {int(line.split("\t")[0]): float(line.split("\t")[1])
                  for line in output.splitlines()}
        assert actual.keys() == expected_scores.keys(), (query, actual, expected_scores)
        for doc_id, expected in expected_scores.items():
            assert math.isclose(actual[doc_id], expected, rel_tol=1e-12), (
                query, doc_id, actual[doc_id], expected)
        results["multi_" + query] = actual
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
    # Cross a generated-output batch boundary inside one document, with empty
    # documents on either side, and compare the retained term count via BM25.
    batch_words = ["batchword" + chr(ord("a") + i // (26 * 26))
                   + chr(ord("a") + i // 26 % 26) + chr(ord("a") + i % 26)
                   for i in range(2051)]
    sql("CREATE TABLE batchdocs(id INT PRIMARY KEY, content TEXT)")
    sql("INSERT INTO batchdocs VALUES (1,NULL),(2,''),(3,'the and'),"
        "(4,'alpha alpha beta'),(5,'%s tailmarker'),(6,'omega')"
        % " ".join(batch_words))
    sql("ALTER TABLE batchdocs ADD FULLTEXT INDEX ft(content) WITH PARSER beng")
    for word in (batch_words[0], batch_words[1023], batch_words[1024], batch_words[-1],
                 "tailmarker"):
        assert sql("SELECT id FROM batchdocs WHERE MATCH(content) AGAINST('%s')" % word) == "5"
    assert sql("SELECT id FROM batchdocs WHERE MATCH(content) AGAINST('alpha')") == "4"
    assert sql("SELECT id FROM batchdocs WHERE MATCH(content) AGAINST('omega')") == "6"
    score = float(sql("SELECT MATCH(content) AGAINST('tailmarker') FROM batchdocs "
                      "WHERE id=5"))
    # "and" is retained by the built-in stopword list: 1 + 3 + 2052 + 1 tokens.
    expected = math.log(5.5 / 1.5) * 2.2 / (
        1 + 1.2 * (0.25 + 0.75 * 2052 / (2057 / 6)))
    assert math.isclose(score, expected, rel_tol=1e-12), (score, expected)
    results["generated_batch_boundaries"] = score
    for table in ("many_repeated", "many_unique", "many_interval"):
        high_cardinality = table != "many_repeated"
        sql("CREATE TABLE %s(id INT PRIMARY KEY, content TEXT)" % table)
        for start in range(1, 5001, 250):
            rows = []
            for doc_id in range(start, start + 250):
                value = doc_id if high_cardinality else doc_id % 10
                suffix = ""
                for _ in range(4):
                    suffix += chr(ord("a") + value % 26)
                    value //= 26
                content = "commonmarker category%s cafe caf\u00e9" % suffix
                if table == "many_interval":
                    content += " " + " ".join("keyword" + chr(ord("a") + i // 26)
                                               + chr(ord("a") + i % 26) for i in range(48))
                if doc_id % 2 == 0:
                    content += " evenmarker"
                rows.append("(%d,'%s')" % (doc_id, content))
            sql("INSERT INTO %s VALUES %s" % (table, ",".join(rows)))
        sql("ALTER TABLE %s ADD FULLTEXT INDEX ft(content) WITH PARSER beng" % table)
        output = sql("SELECT id FROM %s WHERE MATCH(content) AGAINST('evenmarker') "
                     "ORDER BY id" % table)
        assert [int(value) for value in output.splitlines()] == list(range(2, 5001, 2))
        assert sql("SELECT COUNT(*) FROM %s WHERE "
                   "MATCH(content) AGAINST('commonmarker')" % table) == "5000"
        results[table] = sql(
            "SELECT COUNT(*), SUM(id) FROM %s WHERE "
            "MATCH(content) AGAINST('cafe caf\u00e9 categoryaaaa')" % table)
        if high_cardinality:
            rare_terms = []
            for doc_id in (1, 1250, 2500, 3750, 5000):
                value = doc_id
                suffix = ""
                for _ in range(4):
                    suffix += chr(ord("a") + value % 26)
                    value //= 26
                assert sql("SELECT id FROM %s WHERE MATCH(content) "
                           "AGAINST('category%s')" % (table, suffix)) == str(doc_id)
                rare_terms.append("category" + suffix)
            query = "evenmarker " + " ".join(rare_terms)
            select = ("SELECT MATCH(content) AGAINST('%s') AS score FROM %s "
                      "WHERE MATCH(content) AGAINST('%s') ORDER BY score DESC"
                      % (query, table, query))
            all_scores = [float(value) for value in sql(select).splitlines()]
            assert len(all_scores) == 2501
            for limit in (1, 20, 257):
                try:
                    top_scores = [float(value) for value in sql(
                        select + " LIMIT %d" % limit).splitlines()]
                except RuntimeError as error:
                    if not args.allow_known_baseline_topk_error or "ERROR 4016 (HY000)" not in str(error):
                        raise
                    known_errors.append({"table": table, "limit": limit, "error": str(error)})
                    continue
                assert top_scores == all_scores[:limit], (table, limit, top_scores[:3])
            results[table + "_topk"] = all_scores[:20]
    args.output.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
    status = "passed"
    if known_errors:
        status = "known_baseline_failures"
        args.output.with_name("known-topk-errors.json").write_text(
            json.dumps(known_errors, indent=2) + "\n")
    print(json.dumps({"status": status, "known_errors": len(known_errors),
                      "output": str(args.output)}), flush=True)


if __name__ == "__main__":
    main()

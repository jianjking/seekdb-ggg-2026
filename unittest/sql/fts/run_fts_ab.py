#!/usr/bin/env python3
"""Compare two frozen binaries using the unchanged official FTS benchmark."""

import argparse
import hashlib
import importlib.util
import json
import os
import socket
import subprocess
import threading
import time
from pathlib import Path


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--port", type=int, default=2941)
    parser.add_argument("--pairs", type=int, default=2)
    parser.add_argument("--cpus", help="Optional comma-separated CPU IDs")
    parser.add_argument("--before-known-topk-error", action="store_true",
                        help="Record original BMW errors explicitly; candidate regressions stay strict")
    parser.add_argument("--correctness-only", action="store_true",
                        help="Run both regression oracles and hashes without benchmark timing")
    args = parser.parse_args()
    if args.pairs < 2 or args.pairs % 2:
        parser.error("--pairs must be a positive multiple of two")
    if args.cpus:
        os.sched_setaffinity(0, {int(cpu) for cpu in args.cpus.split(",")})
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[3]
    benchmark = repo / "tools/benchmark"
    client = repo / "deps/3rd/u01/obclient/bin/obclient"
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    deps = repo / "deps/3rd"
    env = dict(os.environ, LD_LIBRARY_PATH=":".join(str(deps / path) for path in [
        "usr/local/oceanbase/deps/devel/lib", "usr/local/oceanbase/devtools/lib",
        "usr/local/oceanbase/devtools/lib64", "u01/obclient/lib"]))
    spec = importlib.util.spec_from_file_location(
        "official_fts_score", benchmark / "fts_large_bench_score.py")
    scoring = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(scoring)
    baseline = scoring.load_baseline(benchmark / "fts_large_bench_baseline.json")
    expected_hits = {key: value for key, value in baseline["baseline_metrics"].items()
                     if key.endswith("_hits")}
    manifest = {
        "binaries": {key: {"path": str(path), "sha256": digest(path)}
                     for key, path in binaries.items()},
        "source_head": subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "official_files": {path.name: digest(path)
                           for path in sorted(benchmark.glob("fts_large_bench*"))},
        "config": baseline["benchmark_config"],
        "affinity": sorted(os.sched_getaffinity(0)),
        "cpu": subprocess.check_output(["lscpu"], text=True),
        "clocksource": Path(
            "/sys/devices/system/clocksource/clocksource0/current_clocksource").read_text().strip(),
        "timesyncd_active": subprocess.run(
            ["systemctl", "is-active", "systemd-timesyncd"],
            capture_output=True, text=True).stdout.strip(),
        "before_known_topk_error": args.before_known_topk_error,
        "correctness_only": args.correctness_only,
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (root / "source.diff").write_bytes(subprocess.check_output(
        ["git", "-C", str(repo), "diff", "--", "src"]))

    def run(label, version, port, correctness=False):
        directory = root / label
        directory.mkdir()
        with socket.socket() as sock:
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                raise RuntimeError("Port already in use: " + str(port))
        with (directory / "server.log").open("w") as log:
            server = subprocess.Popen(
                [str(binaries[version]), "--nodaemon", "--base-dir",
                 str(directory / "instance"), "--port", str(port)],
                env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(120):
                    if server.poll() is not None:
                        raise RuntimeError("Server exited: " + label)
                    ready = subprocess.run(
                        [str(client), "-h127.0.0.1", "-P" + str(port), "-uroot",
                         "-N", "-s", "-e", "SELECT 1"], env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    if ready.returncode == 0:
                        break
                    time.sleep(1)
                else:
                    raise RuntimeError("Server start timeout: " + label)
                print("START " + label, flush=True)
                if correctness:
                    baseline_options = []
                    if version == "before" and args.before_known_topk_error:
                        baseline_options = ["--allow-known-baseline-topk-error"]
                    subprocess.run(
                        ["python3", str(Path(__file__).with_name("test_fts_regression.py")),
                         "--client", str(client), "--port", str(port),
                         "--output", str(directory / "correctness.json")] + baseline_options,
                        env=env, check=True)
                    return json.loads((directory / "correctness.json").read_text())
                command = f"{client} -h127.0.0.1 -P{port} -uroot --default-character-set=utf8mb4"
                run_env = dict(env, MYSQL=command + " -N -s", MYSQL_VERBOSE=command,
                               LABEL=label, OUTPUT=str(directory / "report.txt"))
                for key, value in baseline["benchmark_config"].items():
                    run_env[key.upper()] = str(value)
                clock_jumps = []
                stop = threading.Event()

                def monitor_clock():
                    previous = time.time_ns() - time.monotonic_ns()
                    while not stop.wait(0.02):
                        current = time.time_ns() - time.monotonic_ns()
                        if abs(current - previous) > 5_000_000:
                            clock_jumps.append((current - previous) / 1e9)
                        previous = current

                monitor = threading.Thread(target=monitor_clock, daemon=True)
                monitor.start()
                try:
                    with (directory / "console.txt").open("w") as console:
                        subprocess.run(
                            ["bash", str(benchmark / "fts_large_bench.sh")],
                            env=run_env, stdout=console, stderr=subprocess.STDOUT,
                            check=True)
                finally:
                    stop.set()
                    monitor.join()
                (directory / "clock-jumps.json").write_text(
                    json.dumps(clock_jumps) + "\n")
                if clock_jumps:
                    raise RuntimeError("Invalid wall-clock sample: " + str(clock_jumps))
                metrics = scoring.load_report(directory / "report.txt")
                scoring.check_config(metrics, baseline, False)
                for key, expected in expected_hits.items():
                    if metrics[key] != expected:
                        raise RuntimeError(f"{label}: {key}={metrics[key]}, expected {expected}")
                for names in baseline["score_categories"].values():
                    if any(metrics[name] <= 0 for name in names):
                        raise RuntimeError("Non-positive benchmark duration: " + label)
                (directory / "score.json").write_text(json.dumps(
                    scoring.calculate_score(metrics, baseline), indent=2) + "\n")
                (directory / "memory.txt").write_text(
                    Path(f"/proc/{server.pid}/status").read_text())
                print("PASS " + label + " " + json.dumps({
                    name: metrics[name] for names in baseline["score_categories"].values()
                    for name in names}), flush=True)
                return metrics
            finally:
                server.terminate()
                try:
                    server.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()

    before = run("correctness_before", "before", args.port, True)
    after = run("correctness_after", "after", args.port + 1, True)
    if before != after:
        raise RuntimeError("Correctness A/B mismatch")
    print("CORRECTNESS ORACLES A/B IDENTICAL", flush=True)
    if (root / "correctness_before/known-topk-errors.json").exists():
        print("BASELINE HAS RECORDED BMW FAILURES; CANDIDATE PASSED STRICT TOP-K CHECKS", flush=True)
    reports = {}
    versions = [] if args.correctness_only else ["before", "after", "after", "before"] * (args.pairs // 2)
    for index, version in enumerate(versions):
        label = version + "_r" + str(index + 1)
        reports[label] = run(label, version, args.port + 2 + index)
        (root / "ab-results.json").write_text(json.dumps(reports, indent=2) + "\n")
    for version, binary in binaries.items():
        if digest(binary) != manifest["binaries"][version]["sha256"]:
            raise RuntimeError("Binary changed during benchmark: " + version)
    if any(digest(benchmark / name) != value
           for name, value in manifest["official_files"].items()):
        raise RuntimeError("Official benchmark file changed during validation")
    if args.correctness_only:
        (root / "CORRECTNESS_SUCCESS").write_text(
            "Candidate strict correctness passed; unaffected A/B oracles match.\n"
            "No benchmark timing was collected by this regression-only run.\n")
    else:
        (root / "SUCCESS").write_text(
            "Candidate strict correctness and all nine benchmark metrics passed.\n"
            "Any original BMW failures are recorded separately in correctness_before.\n")


if __name__ == "__main__":
    main()

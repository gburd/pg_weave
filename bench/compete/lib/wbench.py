#!/usr/bin/env python3
"""
wbench -- the competitive benchmark instrument for pg_weave.

One tool, three jobs, deliberately kept in one file so the version that produced a
number is unambiguous:

    wbench measure   run queries, emit RAW timing samples as JSONL
    wbench stats     compute statistics from raw JSONL
    wbench gate      run a correctness gate and emit its verdict

The separation is the point.  `measure` never computes a statistic and `stats`
never touches a database.  A pg_fts sub-agent once reported a median of 6.86 ms
where hand measurement gave 2.13 ms -- a 3.2x error that flattered pg_fts
(pg_fts/bench/RESULTS_5WAY_159b:15-31).  The fix is structural: per-engine hosts
emit samples, the coordinator derives every statistic centrally from those
samples, and the raw series ships with the result so anyone can recompute it.

Dependency-light on purpose: stdlib plus a `psql` on PATH.  A fresh AL2023 host
has no psycopg2 and adding a pip step is another thing that can differ between
engine hosts.

See bench/compete/STRATEGY.md for why each rule exists.
"""

import argparse
import json
import math
import os
import random
import re
import statistics
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Defaults.  These are the floor, not a suggestion.  pg_fts's rigor regressed
# from N=200 with bootstrap CIs to bare medians of 5 over one month; naming the
# floor here makes a regression visible in a diff.
# ---------------------------------------------------------------------------
WARMUP = 10
SAMPLES = 200
BOOTSTRAP = 10000

EXEC_TIME_RE = re.compile(r"^\s*Execution Time:\s*([0-9.]+)\s*ms\s*$", re.M)
PLAN_RE = re.compile(r"^\s*(?:->\s*)?(.+?)(?:\s+\(cost=|\s*$)", re.M)


def psql(dsn, sql, timeout=1800):
    """Run SQL through psql -X, return (stdout, stderr, rc).

    -X matters: a developer's ~/.psqlrc can set timing, pager, or a search_path
    and silently change what is measured.
    """
    cmd = ["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1", "-t", "-A", "-d", dsn, "-f", "-"]
    p = subprocess.run(cmd, input=sql, capture_output=True, text=True, timeout=timeout)
    return p.stdout, p.stderr, p.returncode


def psql_scalar(dsn, sql):
    out, err, rc = psql(dsn, sql)
    if rc != 0:
        raise RuntimeError(f"psql failed: {err.strip()}\nSQL: {sql}")
    return out.strip()


# ---------------------------------------------------------------------------
# measure
# ---------------------------------------------------------------------------

def assert_plan(dsn, query, must_match, label):
    """Capture the plan and assert the expected access path appears in it.

    Non-negotiable.  pg_fts published a 24 s "ranked latency" that was a seq scan
    caused by a missing WHERE clause, and pg_weave's own first six competitive
    runs measured a seq scan for the same reason (task L7).  A latency benchmark
    that does not verify the access path is measuring an unknown plan.

    `must_match` is a regex.  Returns (ok, plan_text).
    """
    out, err, rc = psql(dsn, f"EXPLAIN (COSTS OFF) {query};")
    if rc != 0:
        return False, f"EXPLAIN failed: {err.strip()}"
    ok = re.search(must_match, out, re.I | re.M) is not None
    return ok, out.strip()


def measure_one(dsn, query, samples=SAMPLES, warmup=WARMUP, setup=""):
    """Return a raw list of per-execution server-side milliseconds.

    All repetitions run in ONE psql session.  Using one invocation per repetition
    makes every sample a first-scan-in-a-fresh-backend, which on pg_weave produced
    a latency flat at ~87 ms across every selectivity band -- 32 matching
    documents costing the same as 179,262.  That is backend setup, not query work.

    TIMING OFF, SUMMARY ON: per-node instrumentation costs real time on a
    sub-millisecond query, and `Execution Time` is the number we want.
    """
    n = warmup + samples
    stmts = [setup] if setup else []
    stmts += [f"EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) {query};"] * n
    out, err, rc = psql(dsn, "\n".join(stmts))
    if rc != 0:
        raise RuntimeError(f"measure failed: {err.strip()}")
    vals = [float(m) for m in EXEC_TIME_RE.findall(out)]
    if len(vals) < n:
        raise RuntimeError(
            f"expected {n} Execution Time lines, parsed {len(vals)} -- "
            "psql output shape changed or a statement errored"
        )
    return vals[warmup:]


def cmd_measure(args):
    dsn = args.dsn
    spec = json.load(open(args.spec))
    out = open(args.out, "w")

    meta = {
        "kind": "meta",
        "engine": spec["engine"],
        "engine_version": psql_scalar(dsn, spec.get("version_sql", "SELECT 'unknown'")),
        "pg_version": psql_scalar(dsn, "SHOW server_version"),
        "run": args.run,
        "host": os.uname().nodename,
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "samples": args.samples,
        "warmup": args.warmup,
        # Every non-default GUC.  pg_turbovec carried a 450x latency tax for ~20
        # releases because every benchmark set tuning GUCs and no run ever
        # recorded which, so nobody noticed the defaults were catastrophic.
        "settings": json.loads(psql_scalar(dsn, """
            SELECT coalesce(json_agg(json_build_object('n',name,'v',setting))::text,'[]')
              FROM pg_settings WHERE source NOT IN ('default','override')""")),
        # Corpus identity.  The single most expensive historical error was two
        # hosts indexing different columns; this is how the analyzer detects it.
        "corpus_fingerprint": psql_scalar(dsn, spec["fingerprint_sql"]),
        "index_size_bytes": psql_scalar(dsn, spec.get("index_size_sql", "SELECT 0")),
        "build_seconds": spec.get("build_seconds"),
    }
    out.write(json.dumps(meta) + "\n")
    out.flush()

    for q in spec["queries"]:
        label = q["label"]
        query = q["sql"]
        expect = q.get("expect_plan")
        setup = q.get("setup", "")

        rec = {"kind": "query", "label": label, "sql": query, "engine": spec["engine"]}

        if expect:
            ok, plan = assert_plan(dsn, query, expect, label)
            rec["plan_ok"] = ok
            rec["plan"] = plan
            if not ok:
                # Record and continue.  A wrong plan is a RESULT -- it is how
                # task L7 was found -- so it must appear in the output rather
                # than aborting the run.
                rec["error"] = f"plan does not match /{expect}/"
                out.write(json.dumps(rec) + "\n")
                out.flush()
                print(f"  {label}: PLAN MISMATCH (recorded)", file=sys.stderr)
                continue

        # Row count, for the cross-engine match-count column that must appear in
        # the same table as latency.  Analyzer parity with Tantivy is
        # unachievable (it does not stem), so a common-term latency quoted
        # without its match count is meaningless -- permanently.
        if q.get("count_sql"):
            try:
                rec["nrows"] = int(psql_scalar(dsn, q["count_sql"]))
            except Exception as e:
                rec["nrows_error"] = str(e)

        try:
            rec["samples_ms"] = measure_one(dsn, query, args.samples, args.warmup, setup)
        except Exception as e:
            rec["error"] = str(e)
            print(f"  {label}: ERROR {e}", file=sys.stderr)
        else:
            print(f"  {label}: n={len(rec['samples_ms'])} "
                  f"p50~{statistics.median(rec['samples_ms']):.3f}ms", file=sys.stderr)

        out.write(json.dumps(rec) + "\n")
        out.flush()

    out.close()


# ---------------------------------------------------------------------------
# stats
# ---------------------------------------------------------------------------

def pct(sorted_vals, p):
    """Nearest-rank percentile on an already-sorted list.

    Deliberately not linear interpolation: with N=200 the difference is under a
    sample width, and nearest-rank has the property that every reported value is
    an actually-observed measurement.
    """
    if not sorted_vals:
        return None
    k = max(0, min(len(sorted_vals) - 1, int(math.ceil(p / 100.0 * len(sorted_vals))) - 1))
    return sorted_vals[k]


def bootstrap_ci_median(vals, resamples=BOOTSTRAP, alpha=0.05, seed=20260907):
    """Percentile bootstrap CI for the median.

    A point estimate with no dispersion is not a measurement.  Seeded so the
    interval is reproducible from the same raw series.
    """
    if len(vals) < 8:
        return (None, None)
    rng = random.Random(seed)
    n = len(vals)
    meds = []
    for _ in range(resamples):
        meds.append(statistics.median(rng.choices(vals, k=n)))
    meds.sort()
    lo = pct(meds, 100 * alpha / 2)
    hi = pct(meds, 100 * (1 - alpha / 2))
    return (lo, hi)


def summarize(vals):
    s = sorted(vals)
    q1, q3 = pct(s, 25), pct(s, 75)
    lo, hi = bootstrap_ci_median(vals)
    return {
        "n": len(s),
        "min": s[0], "p50": pct(s, 50), "p90": pct(s, 90),
        "p95": pct(s, 95), "p99": pct(s, 99), "max": s[-1],
        "iqr": (q3 - q1) if (q1 is not None and q3 is not None) else None,
        "ci95_lo": lo, "ci95_hi": hi,
        "mean": statistics.fmean(s),
        # Relative dispersion, so a noisy host is visible at a glance.  pg_tre
        # chased a "+18% regression" that was pure host noise.
        "cv": (statistics.pstdev(s) / statistics.fmean(s)) if statistics.fmean(s) else None,
    }


def load_runs(paths):
    engines = {}
    for p in paths:
        meta = None
        for line in open(p):
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("kind") == "meta":
                meta = r
                engines.setdefault(r["engine"], {"meta": r, "queries": {}, "files": []})
                engines[r["engine"]]["files"].append(p)
            elif r.get("kind") == "query":
                eng = r.get("engine") or (meta or {}).get("engine")
                engines.setdefault(eng, {"meta": meta, "queries": {}, "files": [p]})
                engines[eng]["queries"].setdefault(r["label"], []).append(r)
    return engines


def cmd_stats(args):
    engines = load_runs(args.raw)

    # --- corpus identity gate ------------------------------------------------
    # The #1 historical error, mechanically detected.  pg_fts retracted an entire
    # 5-way because two engines indexed title+body and two indexed body alone,
    # noticed only when match counts diverged by 48%.
    fps = {e: d["meta"].get("corpus_fingerprint") for e, d in engines.items() if d.get("meta")}
    distinct = set(v for v in fps.values() if v)
    corpus_ok = len(distinct) <= 1
    report = []
    report.append("## Corpus identity gate\n")
    if corpus_ok:
        report.append(f"PASS -- all {len(fps)} engines report fingerprint `{next(iter(distinct), 'n/a')}`.\n")
    else:
        report.append("**FAIL -- engines indexed different data. Every cross-engine "
                      "comparison below is INVALID.**\n")
        for e, f in sorted(fps.items()):
            report.append(f"- {e}: `{f}`")
        report.append("")

    # --- plan gate -----------------------------------------------------------
    bad_plans = []
    for e, d in engines.items():
        for label, recs in d["queries"].items():
            for r in recs:
                if r.get("plan_ok") is False:
                    bad_plans.append((e, label))
    report.append("## Access-path gate\n")
    if bad_plans:
        report.append("**FAIL -- these measurements did not use the expected index and are "
                      "excluded from the tables below:**\n")
        for e, l in sorted(set(bad_plans)):
            report.append(f"- {e} / {l}")
        report.append("")
    else:
        report.append("PASS -- every measured query used its expected access path.\n")

    # --- per-query tables ----------------------------------------------------
    labels = []
    for d in engines.values():
        for l in d["queries"]:
            if l not in labels:
                labels.append(l)
    englist = sorted(engines)

    report.append("## Latency, milliseconds (server-side Execution Time)\n")
    report.append("`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read "
                  "alongside latency -- engines with different analyzers match different "
                  "numbers of documents.\n")
    header = "| query | " + " | ".join(englist) + " |"
    report.append(header)
    report.append("|" + "---|" * (len(englist) + 1))

    table = {}
    for label in labels:
        cells = []
        for e in englist:
            recs = engines[e]["queries"].get(label, [])
            vals = []
            rows = None
            skipped = False
            for r in recs:
                if r.get("plan_ok") is False:
                    skipped = True
                    continue
                vals.extend(r.get("samples_ms", []))
                if r.get("nrows") is not None:
                    rows = r["nrows"]
            if not vals:
                cells.append("PLAN FAIL" if skipped else "—")
                continue
            st = summarize(vals)
            table.setdefault(label, {})[e] = st
            ci = (f"[{st['ci95_lo']:.3f}–{st['ci95_hi']:.3f}]"
                  if st["ci95_lo"] is not None else "")
            rowstr = f" rows={rows}" if rows is not None else ""
            cells.append(f"{st['p50']:.3f} {ci} p99 {st['p99']:.3f} "
                         f"(n={st['n']}, cv={st['cv']:.2f}){rowstr}")
        report.append(f"| {label} | " + " | ".join(cells) + " |")
    report.append("")

    # --- who wins, and by how much, with the significance caveat ------------
    report.append("## Winner per query, and whether the margin is supported\n")
    report.append("A margin is only claimed when the two medians' bootstrap 95% CIs do "
                  "not overlap. Overlapping CIs are reported as a tie regardless of the "
                  "point estimates.\n")
    report.append("| query | best | second | ratio | CIs disjoint? |")
    report.append("|---|---|---|---|---|")
    for label in labels:
        row = table.get(label, {})
        if len(row) < 2:
            continue
        order = sorted(row.items(), key=lambda kv: kv[1]["p50"])
        (e1, s1), (e2, s2) = order[0], order[1]
        disjoint = (s1["ci95_hi"] is not None and s2["ci95_lo"] is not None
                    and s1["ci95_hi"] < s2["ci95_lo"])
        ratio = s2["p50"] / s1["p50"] if s1["p50"] else float("inf")
        report.append(f"| {label} | {e1} ({s1['p50']:.3f}) | {e2} ({s2['p50']:.3f}) | "
                      f"{ratio:.2f}x | {'yes' if disjoint else '**NO — tie**'} |")
    report.append("")

    # --- build/size ---------------------------------------------------------
    report.append("## Build time and index size\n")
    report.append("| engine | version | build s | index size | non-default GUCs |")
    report.append("|---|---|---:|---:|---:|")
    for e in englist:
        m = engines[e].get("meta") or {}
        sz = m.get("index_size_bytes")
        try:
            szs = f"{int(sz)/1048576:.0f} MB"
        except Exception:
            szs = "—"
        bs = m.get("build_seconds")
        report.append(f"| {e} | {m.get('engine_version','?')} | "
                      f"{bs if bs is not None else '—'} | {szs} | "
                      f"{len(m.get('settings') or [])} |")
    report.append("")

    txt = "\n".join(report)
    if args.out:
        open(args.out, "w").write(txt + "\n")
    print(txt)

    # Non-zero exit on a failed gate, so an orchestrator cannot publish a run
    # whose comparisons are invalid.
    if not corpus_ok:
        return 2
    return 0


# ---------------------------------------------------------------------------
# gate: correctness, run BEFORE timing is trusted
# ---------------------------------------------------------------------------

def cmd_gate(args):
    """Run a correctness gate defined in JSON and emit a verdict.

    Two shapes:
      set_equality  -- symmetric EXCEPT between an index query and a reference
                       must be empty in both directions (pg_tre's approach, and
                       the reason its correctness claims hold)
      topk_parity   -- the index's top-k ids, scored by an oracle, must not be
                       worse than the oracle's k-th score by more than tol
      recall        -- recall@k against a ground-truth table; ALSO fails when
                       recall is 0, because pg_turbovec once recorded a whole
                       latency frontier for a configuration whose recall was 0
    """
    dsn = args.dsn
    spec = json.load(open(args.spec))
    results = []
    worst = 0

    for g in spec["gates"]:
        name, typ = g["name"], g["type"]
        rec = {"gate": name, "type": typ}
        try:
            if typ == "set_equality":
                a = int(psql_scalar(dsn, g["missing_sql"]))
                b = int(psql_scalar(dsn, g["extra_sql"]))
                rec.update(missing=a, extra=b, passed=(a == 0 and b == 0))
            elif typ == "topk_parity":
                miss = int(psql_scalar(dsn, g["violations_sql"]))
                rec.update(violations=miss, passed=(miss == 0))
            elif typ == "recall":
                r = float(psql_scalar(dsn, g["recall_sql"]))
                rec.update(recall=r,
                           passed=(r > 0.0 and r >= g.get("min", 0.0)),
                           zero_recall=(r == 0.0))
            else:
                raise ValueError(f"unknown gate type {typ}")
        except Exception as e:
            rec.update(passed=False, error=str(e))
        results.append(rec)
        if not rec["passed"]:
            worst = 1
        print(f"{'PASS' if rec['passed'] else 'FAIL'} {name}: "
              f"{ {k: v for k, v in rec.items() if k not in ('gate','type')} }",
              file=sys.stderr)

    open(args.out, "w").write(json.dumps(
        {"kind": "gates", "engine": spec.get("engine"), "results": results}) + "\n")
    return worst


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("measure", help="emit raw timing samples as JSONL")
    m.add_argument("--dsn", required=True)
    m.add_argument("--spec", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--run", default="adhoc")
    m.add_argument("--samples", type=int, default=SAMPLES)
    m.add_argument("--warmup", type=int, default=WARMUP)
    m.set_defaults(func=cmd_measure)

    s = sub.add_parser("stats", help="compute statistics from raw JSONL")
    s.add_argument("raw", nargs="+")
    s.add_argument("--out")
    s.set_defaults(func=cmd_stats)

    g = sub.add_parser("gate", help="run correctness gates")
    g.add_argument("--dsn", required=True)
    g.add_argument("--spec", required=True)
    g.add_argument("--out", required=True)
    g.set_defaults(func=cmd_gate)

    args = ap.parse_args()
    rc = args.func(args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()

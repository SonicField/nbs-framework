#!/usr/bin/env python3
"""CinderX Baseline Comparison — Phase 2a

Compares two baseline test runs and reports regressions.

Usage:
    python3 17-02-2026-cinderx-baseline-compare.py <baseline_dir> <target_dir>

Example:
    python3 17-02-2026-cinderx-baseline-compare.py \
        results/stock_20260217T120000Z \
        results/stub_20260217T130000Z
"""

import csv
import json
import os
import sys
from pathlib import Path


def load_summary(report_dir: str) -> dict:
    """Load summary.csv into a dict keyed by test_name."""
    summary_path = os.path.join(report_dir, "summary.csv")
    if not os.path.exists(summary_path):
        print(f"ERROR: {summary_path} not found", file=sys.stderr)
        sys.exit(1)

    results = {}
    with open(summary_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            results[row["test_name"]] = row
    return results


def load_environment(report_dir: str) -> dict:
    """Load environment.txt."""
    env_path = os.path.join(report_dir, "environment.txt")
    env = {}
    if os.path.exists(env_path):
        with open(env_path) as f:
            for line in f:
                if "=" in line:
                    key, val = line.strip().split("=", 1)
                    env[key] = val
    return env


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    baseline_dir = sys.argv[1]
    target_dir = sys.argv[2]

    baseline = load_summary(baseline_dir)
    target = load_summary(target_dir)
    baseline_env = load_environment(baseline_dir)
    target_env = load_environment(target_dir)

    print("=" * 70)
    print("CinderX Baseline Comparison")
    print("=" * 70)
    print(f"Baseline: {baseline_env.get('mode', '?')} ({baseline_dir})")
    print(f"Target:   {target_env.get('mode', '?')} ({target_dir})")
    print()

    # Collect all test names
    all_tests = sorted(set(list(baseline.keys()) + list(target.keys())))

    regressions = []
    improvements = []
    unchanged = []
    new_tests = []
    missing_tests = []

    for test in all_tests:
        b = baseline.get(test)
        t = target.get(test)

        if b is None:
            new_tests.append((test, t))
            continue
        if t is None:
            missing_tests.append((test, b))
            continue

        b_fail = int(b["failed"]) + int(b["errors"])
        t_fail = int(t["failed"]) + int(t["errors"])
        b_pass = int(b["passed"])
        t_pass = int(t["passed"])

        if t_fail > b_fail:
            regressions.append((test, b, t))
        elif t_fail < b_fail:
            improvements.append((test, b, t))
        else:
            unchanged.append((test, b, t))

    # Print regressions (most important)
    if regressions:
        print(f"REGRESSIONS ({len(regressions)} tests):")
        print("-" * 70)
        for test, b, t in regressions:
            b_fail = int(b["failed"]) + int(b["errors"])
            t_fail = int(t["failed"]) + int(t["errors"])
            print(f"  {test}: {b_fail} failures → {t_fail} failures "
                  f"(+{t_fail - b_fail})")
        print()

    # Print improvements
    if improvements:
        print(f"IMPROVEMENTS ({len(improvements)} tests):")
        print("-" * 70)
        for test, b, t in improvements:
            b_fail = int(b["failed"]) + int(b["errors"])
            t_fail = int(t["failed"]) + int(t["errors"])
            print(f"  {test}: {b_fail} failures → {t_fail} failures "
                  f"(-{b_fail - t_fail})")
        print()

    # Print unchanged
    if unchanged:
        print(f"UNCHANGED ({len(unchanged)} tests):")
        print("-" * 70)
        for test, b, t in unchanged:
            b_fail = int(b["failed"]) + int(b["errors"])
            status = "CLEAN" if b_fail == 0 else f"{b_fail} failures (pre-existing)"
            print(f"  {test}: {status}")
        print()

    # Summary
    print("=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"  Regressions:  {len(regressions)}")
    print(f"  Improvements: {len(improvements)}")
    print(f"  Unchanged:    {len(unchanged)}")
    print(f"  New tests:    {len(new_tests)}")
    print(f"  Missing:      {len(missing_tests)}")
    print()

    # Totals
    for label, data, env in [("Baseline", baseline, baseline_env),
                              ("Target", target, target_env)]:
        total_pass = sum(int(r["passed"]) for r in data.values())
        total_fail = sum(int(r["failed"]) for r in data.values())
        total_err = sum(int(r["errors"]) for r in data.values())
        total_skip = sum(int(r["skipped"]) for r in data.values())
        print(f"  {label} ({env.get('mode', '?')}): "
              f"{total_pass} passed, {total_fail} failed, "
              f"{total_err} errors, {total_skip} skipped")

    # Exit code: non-zero if regressions found
    if regressions:
        print(f"\nVERDICT: FAIL — {len(regressions)} regressions detected")
        sys.exit(1)
    else:
        print("\nVERDICT: PASS — no regressions detected")
        sys.exit(0)


if __name__ == "__main__":
    main()

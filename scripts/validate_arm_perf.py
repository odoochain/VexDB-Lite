#!/usr/bin/env python3
"""validate_arm_perf.py — aggregate Marina Petrova ARM qps runs.

Reads results/<label>_<ts>/run_*.json and emits summary.md + summary.csv with:
    mean, stddev (sample), 95% CI (t critical from normal-tail erfc fallback),
    min, max, recall@10 mean.

If --baseline-dir is supplied, also performs Welch's t-test vs that baseline
and reports:
    delta_qps, delta_pct, welch_t, welch_p (two-sided), CI overlap?,
    effect_size (Cohen's d).

Decision gates (Marina protocol):
    - Welch p < 0.05
    - 95% CI does NOT overlap
    - effect size |delta_qps| / mean_baseline >= 5%
    - recall@10 regression <= 0.5pp

Pure stdlib (statistics + math + json + csv + argparse).
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import statistics
import sys
from dataclasses import dataclass, asdict
from typing import List, Optional


@dataclass
class RunStat:
    label: str
    arch: str
    n: int
    mean_qps: float
    stddev_qps: float
    ci95_low: float
    ci95_high: float
    min_qps: float
    max_qps: float
    mean_recall_at_10: float
    raw_qps: List[float]


def _normal_p_two_sided(z: float) -> float:
    """Two-sided p-value from |z| using erfc — kept for reference only.

    NOTE: at R=10 (df≈18) this normal-tail approximation can flip the p<0.05
    gate vs the real Student-t value (e.g. t=2.0 → normal p≈0.046 but t-CDF
    p≈0.060). Welch's t-test should call ``_t_cdf_two_sided`` instead.
    """
    return math.erfc(abs(z) / math.sqrt(2.0))


def _betacf(a: float, b: float, x: float, max_iter: int = 200, eps: float = 3e-7) -> float:
    """Lentz's method for the continued fraction of the regularized incomplete beta."""
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < 1e-30:
        d = 1e-30
    d = 1.0 / d
    h = d
    for m in range(1, max_iter + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-30:
            d = 1e-30
        c = 1.0 + aa / c
        if abs(c) < 1e-30:
            c = 1e-30
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-30:
            d = 1e-30
        c = 1.0 + aa / c
        if abs(c) < 1e-30:
            c = 1e-30
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < eps:
            return h
    return h


def _betai(a: float, b: float, x: float) -> float:
    """Regularized incomplete beta I_x(a, b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    bt = math.exp(
        math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
        + a * math.log(x) + b * math.log(1.0 - x)
    )
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def _t_cdf_two_sided(t: float, df: float) -> float:
    """Two-sided p-value of Student's t with `df` degrees of freedom.

    p = I_x(df/2, 1/2) where x = df / (df + t**2). At df=18, t=2.0 returns
    ~0.0608 (matches scipy.stats.t.sf*2 to 3 decimals).
    """
    if df <= 0:
        return 1.0
    x = df / (df + t * t)
    return _betai(df / 2.0, 0.5, x)


def _t_critical_95(df: float) -> float:
    """Rough 95% two-sided t critical. For df >= 30 use 1.96; small-sample
    table for 1..29; pure stdlib (no scipy)."""
    table = {
        1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571,
        6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228,
        11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131,
        16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086,
        21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060,
        26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045,
    }
    if df <= 0:
        return float('inf')
    if df < 1:
        return 12.706
    k = int(math.floor(df))
    if k in table:
        return table[k]
    return 1.96


def load_runs(results_dir: str) -> List[dict]:
    paths = sorted(glob.glob(os.path.join(results_dir, "run_*.json")))
    runs = []
    for p in paths:
        with open(p, "r", encoding="utf-8") as f:
            runs.append(json.load(f))
    if not runs:
        raise SystemExit(f"no run_*.json found in {results_dir}")
    return runs


def aggregate(label: str, arch: str, runs: List[dict]) -> RunStat:
    qps = [float(r.get("qps", r.get("queries_per_second", 0.0))) for r in runs]
    recalls = [float(r.get("recall_at_10", r.get("recall", 0.0))) for r in runs]
    n = len(qps)
    mean = statistics.fmean(qps)
    sd = statistics.stdev(qps) if n >= 2 else 0.0
    se = sd / math.sqrt(n) if n >= 1 else 0.0
    tcrit = _t_critical_95(n - 1)
    ci_low = mean - tcrit * se
    ci_high = mean + tcrit * se
    return RunStat(
        label=label,
        arch=arch,
        n=n,
        mean_qps=mean,
        stddev_qps=sd,
        ci95_low=ci_low,
        ci95_high=ci_high,
        min_qps=min(qps),
        max_qps=max(qps),
        mean_recall_at_10=statistics.fmean(recalls) if recalls else 0.0,
        raw_qps=qps,
    )


def welch_ttest(a: RunStat, b: RunStat) -> dict:
    """Welch's t-test, two-sided. a = candidate, b = baseline."""
    na, nb = a.n, b.n
    va = a.stddev_qps ** 2
    vb = b.stddev_qps ** 2
    if na < 2 or nb < 2 or (va == 0 and vb == 0):
        return {"t": float('nan'), "p": float('nan'), "df": 0.0}
    se = math.sqrt(va / na + vb / nb)
    if se == 0:
        return {"t": float('inf'), "p": 0.0, "df": 0.0}
    t = (a.mean_qps - b.mean_qps) / se
    num = (va / na + vb / nb) ** 2
    den = ((va / na) ** 2) / max(na - 1, 1) + ((vb / nb) ** 2) / max(nb - 1, 1)
    df = num / den if den > 0 else 0.0
    # Use the real Student-t CDF (regularized incomplete beta). The previous
    # normal-tail approximation could flip the p<0.05 gate at R=10 (df≈18).
    p = _t_cdf_two_sided(abs(t), df)
    return {"t": t, "p": p, "df": df}


def ci_overlap(a: RunStat, b: RunStat) -> bool:
    return not (a.ci95_low > b.ci95_high or b.ci95_low > a.ci95_high)


def cohens_d(a: RunStat, b: RunStat) -> float:
    sa, sb = a.stddev_qps, b.stddev_qps
    pooled = math.sqrt(((a.n - 1) * sa * sa + (b.n - 1) * sb * sb) / max(a.n + b.n - 2, 1))
    if pooled == 0:
        return float('nan')
    return (a.mean_qps - b.mean_qps) / pooled


def write_csv(path: str, stat: RunStat, comparison: Optional[dict] = None) -> None:
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["metric", "value"])
        for k, v in asdict(stat).items():
            if k == "raw_qps":
                w.writerow([k, ";".join(f"{x:.2f}" for x in v)])
            else:
                w.writerow([k, v])
        if comparison:
            w.writerow([])
            w.writerow(["comparison_metric", "value"])
            for k, v in comparison.items():
                w.writerow([k, v])


def write_markdown(path: str, stat: RunStat, baseline: Optional[RunStat] = None,
                   comparison: Optional[dict] = None) -> None:
    lines = []
    lines.append(f"# ARM qps validation — `{stat.label}` ({stat.arch})\n")
    lines.append(f"- runs: {stat.n}")
    lines.append(f"- mean qps: **{stat.mean_qps:.2f}**")
    lines.append(f"- stddev: {stat.stddev_qps:.2f}  ({(stat.stddev_qps / stat.mean_qps * 100 if stat.mean_qps else 0):.2f}%)")
    lines.append(f"- 95% CI: [{stat.ci95_low:.2f}, {stat.ci95_high:.2f}]")
    lines.append(f"- min / max: {stat.min_qps:.2f} / {stat.max_qps:.2f}")
    lines.append(f"- recall@10 (mean): {stat.mean_recall_at_10:.4f}")
    lines.append("")
    lines.append("## raw runs")
    lines.append("")
    lines.append("| run | qps |")
    lines.append("|----:|----:|")
    for i, q in enumerate(stat.raw_qps, 1):
        lines.append(f"| {i} | {q:.2f} |")
    lines.append("")

    if baseline and comparison:
        gate = comparison["gate"]
        lines.append("## A/B vs baseline\n")
        lines.append(f"- baseline label: `{baseline.label}` ({baseline.arch}), mean {baseline.mean_qps:.2f}")
        lines.append(f"- delta qps: **{comparison['delta_qps']:+.2f}** ({comparison['delta_pct']:+.2f}%)")
        lines.append(f"- Welch t = {comparison['welch_t']:.3f}, df ≈ {comparison['welch_df']:.1f}, p ≈ {comparison['welch_p']:.4g}")
        lines.append(f"- 95% CI overlap: **{'YES' if comparison['ci_overlap'] else 'NO'}**")
        lines.append(f"- Cohen's d: {comparison['cohens_d']:.3f}")
        lines.append(f"- recall delta: {comparison['recall_delta_pp']:+.3f} pp")
        lines.append("")
        lines.append("### Marina gates")
        lines.append("")
        lines.append(f"- p < 0.05 ............. {'PASS' if gate['p_lt_05'] else 'FAIL'}")
        lines.append(f"- CI non-overlap ....... {'PASS' if gate['ci_disjoint'] else 'FAIL'}")
        lines.append(f"- effect >= 5% ......... {'PASS' if gate['effect_5pct'] else 'FAIL'}")
        lines.append(f"- recall regression <= 0.5pp ... {'PASS' if gate['recall_ok'] else 'FAIL'}")
        lines.append("")
        lines.append(f"**Verdict: {'SIGNIFICANT IMPROVEMENT' if gate['all_pass'] and comparison['delta_qps'] > 0 else 'INCONCLUSIVE / NO-OP'}**")
        lines.append("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def parse_args(argv: List[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Aggregate ARM qps validation runs.")
    p.add_argument("--results-dir", required=True, help="Dir containing run_*.json")
    p.add_argument("--label", required=True)
    p.add_argument("--arch", required=True)
    p.add_argument("--baseline-dir", default=None,
                   help="Optional baseline results dir (for A/B Welch's t-test)")
    return p.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)

    runs = load_runs(args.results_dir)
    stat = aggregate(args.label, args.arch, runs)

    baseline_stat: Optional[RunStat] = None
    comparison: Optional[dict] = None
    if args.baseline_dir:
        b_runs = load_runs(args.baseline_dir)
        # Try to recover baseline label/arch from first run JSON.
        b_label = b_runs[0].get("label", "baseline")
        b_arch = b_runs[0].get("arch", "?")
        baseline_stat = aggregate(b_label, b_arch, b_runs)

        wt = welch_ttest(stat, baseline_stat)
        delta = stat.mean_qps - baseline_stat.mean_qps
        delta_pct = (delta / baseline_stat.mean_qps * 100.0) if baseline_stat.mean_qps else 0.0
        d = cohens_d(stat, baseline_stat)
        overlap = ci_overlap(stat, baseline_stat)
        recall_delta_pp = (stat.mean_recall_at_10 - baseline_stat.mean_recall_at_10) * 100.0

        gate = {
            "p_lt_05": (not math.isnan(wt["p"])) and wt["p"] < 0.05,
            "ci_disjoint": not overlap,
            "effect_5pct": abs(delta_pct) >= 5.0,
            "recall_ok": recall_delta_pp >= -0.5,
        }
        gate["all_pass"] = all(gate.values())

        comparison = {
            "delta_qps": delta,
            "delta_pct": delta_pct,
            "welch_t": wt["t"],
            "welch_p": wt["p"],
            "welch_df": wt["df"],
            "ci_overlap": overlap,
            "cohens_d": d,
            "recall_delta_pp": recall_delta_pp,
            "gate": gate,
        }

    md_path = os.path.join(args.results_dir, "summary.md")
    csv_path = os.path.join(args.results_dir, "summary.csv")
    write_markdown(md_path, stat, baseline_stat, comparison)
    # Flatten gate for CSV.
    flat_cmp = None
    if comparison:
        flat_cmp = {k: v for k, v in comparison.items() if k != "gate"}
        for gk, gv in comparison["gate"].items():
            flat_cmp[f"gate_{gk}"] = gv
    write_csv(csv_path, stat, flat_cmp)
    print(f"[validate_arm_perf.py] wrote {md_path}")
    print(f"[validate_arm_perf.py] wrote {csv_path}")
    if comparison:
        verdict = "PASS" if comparison["gate"]["all_pass"] and comparison["delta_qps"] > 0 else "INCONCLUSIVE"
        print(f"[validate_arm_perf.py] A/B verdict vs {args.baseline_dir}: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

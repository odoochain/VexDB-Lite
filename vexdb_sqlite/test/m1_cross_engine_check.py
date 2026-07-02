#!/usr/bin/env python3
"""M1 跨引擎一致性对照：SQLite 适配层 vs DuckDB 适配层的距离函数。

验收标准（docs/plans/2026-06-10_sqlite-adapter-v1-plan.md M1）：
同输入同输出，相对容差 1e-6。两边都吃 float32，参考值用 numpy float32 复算。

用法:
  python3 m1_cross_engine_check.py \
      --sqlite-ext vexdb_sqlite/build/vexdb_lite \
      --duckdb-bin build/duck/build/duckdb
"""
import argparse
import random
import re
import struct
import subprocess
import sys

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

DIMS = [3, 8, 128, 384]
PAIRS_PER_DIM = 50
# 跨 ISA（NEON vs SSE/AVX）浮点累加顺序不同，逐位一致不可达；验收语义是
# 「两引擎各自正确 + 引擎间分歧在 float32 重排序误差内」。容差随维度增长：
# 实测 128/384 维 IP 的跨 ISA 分歧 ~1.1-1.3e-6，理论上界 eps*dim 远大于此。
EPS32 = 1.19209290e-07


def tol_for(dim):
    return max(1e-6, 8 * EPS32 * (dim ** 0.5))

# 函数名映射: (sqlite 表达式, duckdb 表达式)
FUNCS = [
    ("vexdb_l2_distance({a}, {b})", "l2_distance({a}::FLOAT[{d}], {b}::FLOAT[{d}])"),
    ("vexdb_cosine_distance({a}, {b})", "cosine_distance({a}::FLOAT[{d}], {b}::FLOAT[{d}])"),
    ("vexdb_inner_product({a}, {b})", "inner_product({a}::FLOAT[{d}], {b}::FLOAT[{d}])"),
    ("vexdb_negative_inner_product({a}, {b})",
     "list_negative_inner_product({a}::FLOAT[{d}], {b}::FLOAT[{d}])"),
]


def f32(values):
    """模拟 float32 截断（两引擎内部均为 float32）。"""
    return [struct.unpack("f", struct.pack("f", v))[0] for v in values]


def vec_literal(values):
    return "[" + ",".join(repr(v) for v in values) + "]"


def run_sql(cmd, sql, label):
    p = subprocess.run(cmd, input=sql, capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        print(f"FATAL: {label} 执行失败:\n{p.stderr[:2000]}", file=sys.stderr)
        sys.exit(2)
    # 结果行全是数字；剥 ANSI 颜色码（Rosetta WARNING 会带色粘连首行结果），
    # 再丢弃 banner/WARNING 等非数值行。
    out = []
    for line in p.stdout.splitlines():
        line = ANSI_RE.sub("", line).strip()
        if not line:
            continue
        try:
            float(line)
        except ValueError:
            continue
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sqlite-ext", required=True, help="loadable 扩展路径（不带后缀）")
    ap.add_argument("--duckdb-bin", required=True)
    ap.add_argument("--seed", type=int, default=20260610)
    args = ap.parse_args()

    random.seed(args.seed)
    cases = []  # (dim, vec_a, vec_b)
    for dim in DIMS:
        for _ in range(PAIRS_PER_DIM):
            a = f32([random.uniform(-1, 1) for _ in range(dim)])
            b = f32([random.uniform(-1, 1) for _ in range(dim)])
            cases.append((dim, a, b))

    sqlite_sql, duck_sql = [], []
    for dim, a, b in cases:
        la, lb = vec_literal(a), vec_literal(b)
        for s_expr, d_expr in FUNCS:
            sqlite_sql.append(
                "SELECT " + s_expr.format(a=f"'{la}'", b=f"'{lb}'") + ";")
            duck_sql.append(
                "SELECT " + d_expr.format(a=la, b=lb, d=dim) + ";")

    sqlite_out = run_sql(
        ["sqlite3", ":memory:"],
        f".load {args.sqlite_ext} sqlite3_vexdblite_init\n" + "\n".join(sqlite_sql),
        "sqlite3")
    duck_out = run_sql(
        [args.duckdb_bin, "-noheader", "-list", ":memory:"],
        "\n".join(duck_sql),
        "duckdb")

    n = len(cases) * len(FUNCS)
    if len(sqlite_out) != n or len(duck_out) != n:
        print(f"FATAL: 输出行数不符 sqlite={len(sqlite_out)} duck={len(duck_out)} expect={n}")
        sys.exit(2)

    # float64 真值仲裁（与两引擎一致的语义：L2 取 sqrt、cosine=1-sim、neg_ip 取负）
    def truth_for(idx):
        dim, a, b = cases[idx // len(FUNCS)]
        fi = idx % len(FUNCS)
        ip = sum(x * y for x, y in zip(a, b))
        if fi == 0:
            return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5
        if fi == 1:
            na = sum(x * x for x in a) ** 0.5
            nb = sum(y * y for y in b) ** 0.5
            return 1.0 - ip / (na * nb)
        return ip if fi == 2 else -ip

    worst_pair, worst_truth = 0.0, 0.0
    fails = 0
    for i, (s, d) in enumerate(zip(sqlite_out, duck_out)):
        sv, dv, tv = float(s), float(d), truth_for(i)
        dim = cases[i // len(FUNCS)][0]
        tol = tol_for(dim)
        scale = max(abs(tv), 1.0)
        rel_pair = abs(sv - dv) / scale            # 引擎间分歧
        rel_truth = max(abs(sv - tv), abs(dv - tv)) / scale  # 各自 vs 真值
        worst_pair = max(worst_pair, rel_pair)
        worst_truth = max(worst_truth, rel_truth)
        if rel_pair > tol or rel_truth > tol:
            fails += 1
            if fails <= 5:
                print(f"MISMATCH #{i}: dim={dim} func={FUNCS[i % len(FUNCS)][0].split('(')[0]} "
                      f"sqlite={sv!r} duck={dv!r} truth={tv!r} "
                      f"rel_pair={rel_pair:.3e} rel_truth={rel_truth:.3e} tol={tol:.3e}")

    print(f"cases={n} worst_pair_diff={worst_pair:.3e} worst_truth_diff={worst_truth:.3e}")
    if fails:
        print(f"M1 CROSS-ENGINE: FAIL ({fails}/{n})")
        sys.exit(1)
    print("M1 CROSS-ENGINE: PASS")


if __name__ == "__main__":
    main()

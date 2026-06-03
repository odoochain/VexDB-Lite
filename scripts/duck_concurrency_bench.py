#!/usr/bin/env python3
"""DuckDB 向量检索并发吞吐对比: vex (GRAPH_INDEX) vs VSS (HNSW).

同一个官方 DuckDB 1.5.2 进程内分别加载两个扩展, 同数据/同 HNSW 参数/同 ef_search,
在 1/4/8/16 个并发客户端线程下测 QPS, 并各报 recall@10 作为操作点.
"""
import duckdb, h5py, numpy as np, pyarrow as pa
import time, threading, argparse, statistics

DATA = "/home/ecs-user/ann-bench-data/sift-128-euclidean.hdf5"
VEX_EXT = "/home/ecs-user/.duckdb_vex/vexdb_lite.duckdb_extension"
DIM = 128
K = 10
M = 16
EFC = 128
WARMUP_S = 3.0
MEASURE_S = 20.0
CONC = [1, 4, 8, 16]


def load_data():
    with h5py.File(DATA, "r") as f:
        train = np.ascontiguousarray(f["train"][:], dtype=np.float32)
        test = np.ascontiguousarray(f["test"][:], dtype=np.float32)
        gt = f["neighbors"][:, :K].astype(np.int64)
    return train, test, gt


def make_table(con, train):
    n = train.shape[0]
    flat = pa.array(train.reshape(-1))
    fsl = pa.FixedSizeListArray.from_arrays(flat, DIM)
    tbl = pa.table({"id": pa.array(np.arange(n, dtype=np.int32)), "vec": fsl})
    con.register("train_arrow", tbl)
    con.execute(f"CREATE TABLE base (id INTEGER, vec FLOAT[{DIM}])")
    con.execute(f"INSERT INTO base SELECT id, vec::FLOAT[{DIM}] FROM train_arrow")
    con.unregister("train_arrow")


ENGINES = {
    "vex": dict(
        load=[f"LOAD '{VEX_EXT}'"],
        index=f"CREATE INDEX vexidx ON base USING GRAPH_INDEX (vec) "
               f"WITH (m={M}, ef_construction={EFC}, metric='l2', threads=16)",
        distfn="l2_distance",
        efs=lambda efs: f"SET vexdb_ef_search={efs}",
    ),
    "vss": dict(
        load=["INSTALL vss", "LOAD vss"],
        index=f"CREATE INDEX vssidx ON base USING HNSW (vec) "
               f"WITH (metric='l2sq', M={M}, ef_construction={EFC})",
        distfn="array_distance",
        efs=lambda efs: f"SET hnsw_ef_search={efs}",
    ),
}


def query_sql(distfn):
    return (f"SELECT id FROM base ORDER BY {distfn}(vec, ?::FLOAT[{DIM}]) LIMIT {K}")


def check_index_used(con, distfn):
    q = query_sql(distfn).replace("?", "[" + ",".join("0.0" for _ in range(DIM)) + "]")
    plan = "\n".join(str(r) for r in con.execute("EXPLAIN " + q).fetchall())
    up = plan.upper()
    return ("HNSW_INDEX_SCAN" in up or "GRAPH_INDEX" in up or "INDEX_SCAN" in up
            or "COLUMN_DATA" in up), plan


def recall(con, distfn, test, gt):
    sql = query_sql(distfn)
    hit = 0
    tot = 0
    for i in range(test.shape[0]):
        res = con.execute(sql, [test[i].tolist()]).fetchall()
        got = set(r[0] for r in res)
        truth = set(gt[i].tolist())
        hit += len(got & truth)
        tot += K
    return hit / tot


def bench_conc(con, distfn, test, nthreads):
    sql = query_sql(distfn)
    nq = test.shape[0]
    qlists = [test[i].tolist() for i in range(nq)]
    stop = threading.Event()
    counts = [0] * nthreads
    barrier = threading.Barrier(nthreads + 1)

    def worker(tid):
        cur = con.cursor()
        idx = tid
        barrier.wait()  # warmup start
        # warmup
        wend = time.time() + WARMUP_S
        while time.time() < wend:
            cur.execute(sql, [qlists[idx % nq]]).fetchall()
            idx += nthreads
        barrier.wait()  # measure start
        c = 0
        while not stop.is_set():
            cur.execute(sql, [qlists[idx % nq]]).fetchall()
            idx += nthreads
            c += 1
        counts[tid] = c

    threads = [threading.Thread(target=worker, args=(t,)) for t in range(nthreads)]
    for t in threads:
        t.start()
    barrier.wait()   # release warmup
    barrier.wait()   # release measure
    t0 = time.time()
    time.sleep(MEASURE_S)
    stop.set()
    for t in threads:
        t.join()
    elapsed = time.time() - t0
    total = sum(counts)
    return total / elapsed, total, elapsed


def run_engine(name, train, test, gt, efs):
    cfg = ENGINES[name]
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute("SET threads=16")
    for s in cfg["load"]:
        con.execute(s)
    print(f"\n===== engine={name} efs={efs} =====", flush=True)
    t = time.time()
    make_table(con, train)
    print(f"[{name}] load 1M rows: {time.time()-t:.1f}s", flush=True)
    t = time.time()
    con.execute(cfg["index"])
    print(f"[{name}] build index (m={M} efc={EFC}): {time.time()-t:.1f}s", flush=True)
    con.execute(cfg["efs"](efs))
    used, plan = check_index_used(con, cfg["distfn"])
    print(f"[{name}] index used by query: {used}", flush=True)
    if not used:
        print(plan, flush=True)
    t = time.time()
    rec = recall(con, cfg["distfn"], test, gt)
    print(f"[{name}] recall@{K} (efs={efs}): {rec:.4f}  ({test.shape[0]} queries, {time.time()-t:.1f}s)", flush=True)
    results = []
    for c in CONC:
        qps, total, el = bench_conc(con, cfg["distfn"], test, c)
        print(f"[{name}] conc={c:2d}  QPS={qps:9.1f}  (n={total}, {el:.1f}s)", flush=True)
        results.append((c, qps))
    con.close()
    return rec, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--efs", type=int, default=200)
    ap.add_argument("--engines", default="vex,vss")
    args = ap.parse_args()
    print("loading SIFT1M ...", flush=True)
    train, test, gt = load_data()
    print(f"train={train.shape} test={test.shape} gt={gt.shape}", flush=True)
    summary = {}
    for name in args.engines.split(","):
        rec, results = run_engine(name, train, test, gt, args.efs)
        summary[name] = (rec, results)

    print("\n\n================ SUMMARY (efs=%d, m=%d, efc=%d, k=%d) ================" % (args.efs, M, EFC, K))
    hdr = "engine   recall@%d  " % K + "".join(f"  c={c:<2d}QPS" for c in CONC)
    print(hdr)
    for name, (rec, results) in summary.items():
        row = f"{name:7s}  {rec:7.4f}   " + "".join(f"  {q:8.1f}" for _, q in results)
        print(row)
    # scaling factor vs c=1
    print("\nscaling (QPS_c / QPS_1):")
    for name, (rec, results) in summary.items():
        base = results[0][1]
        row = f"{name:7s}  " + "".join(f"  {q/base:6.2f}x" for _, q in results)
        print(row)


if __name__ == "__main__":
    main()

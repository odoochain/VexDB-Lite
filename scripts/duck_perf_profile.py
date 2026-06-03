#!/usr/bin/env python3
"""vex 搜索 perf 火焰图采样：建 1M 索引后,起 N 线程无限 kNN 查询,供 perf record 采样。"""
import duckdb, h5py, numpy as np, pyarrow as pa
import time, threading, sys

DATA = "/home/ecs-user/ann-bench-data/sift-128-euclidean.hdf5"
EXT = "/home/ecs-user/vex_sym.duckdb_extension"   # 带符号
DIM = 128; K = 10; M = 16; EFC = 128; EFS = 200
NTHREADS = int(sys.argv[1]) if len(sys.argv) > 1 else 4

with h5py.File(DATA, "r") as f:
    train = np.ascontiguousarray(f["train"][:], dtype=np.float32)
    test = np.ascontiguousarray(f["test"][:], dtype=np.float32)
print(f"loaded train={train.shape} test={test.shape}", flush=True)

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("SET threads=16")
con.execute(f"LOAD '{EXT}'")
n = train.shape[0]
fsl = pa.FixedSizeListArray.from_arrays(pa.array(train.reshape(-1)), DIM)
con.register("a", pa.table({"id": pa.array(np.arange(n, dtype=np.int32)), "vec": fsl}))
con.execute(f"CREATE TABLE base (id INTEGER, vec FLOAT[{DIM}])")
con.execute(f"INSERT INTO base SELECT id, vec::FLOAT[{DIM}] FROM a")
con.unregister("a")
t = time.time()
con.execute(f"CREATE INDEX vexidx ON base USING GRAPH_INDEX (vec) WITH (m={M}, ef_construction={EFC}, metric='l2', threads=16)")
print(f"build index: {time.time()-t:.1f}s", flush=True)
con.execute(f"SET vexdb_ef_search={EFS}")

sql = f"SELECT id FROM base ORDER BY l2_distance(vec, ?::FLOAT[{DIM}]) LIMIT {K}"
qlists = [test[i].tolist() for i in range(test.shape[0])]
nq = len(qlists)

def worker(tid):
    cur = con.cursor()
    i = tid
    while True:
        cur.execute(sql, [qlists[i % nq]]).fetchall()
        i += NTHREADS

print(f"STEADY nthreads={NTHREADS} (perf record now)", flush=True)
for t in range(NTHREADS):
    threading.Thread(target=worker, args=(t,), daemon=True).start()
while True:
    time.sleep(5)

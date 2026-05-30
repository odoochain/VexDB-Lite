# Known Limitation: Parallel Hybrid Build Recall Gap

**最后更新**: 2026-05-21
**严重程度**: P1(不阻塞 v1)
**状态**: 已知,有 workaround,不修

## 现象

| 配置 | 实测 recall@10 (SIFT1M m=16 efc=128 efs=200) | 对照 pgvector |
|---|---|---|
| serial pw=0 mwm=2GB (全 in-memory) | ~0.99 | ~0.99 |
| serial pw=0 mwm=512MB (hybrid in-mem→disk) | **0.9931** | 0.9964 (差 **0.33pp**) |
| parallel pw=4 mwm=2GB (全 DSM) | ~0.99 | ~0.99 |
| **parallel pw=4 mwm=512MB (DSM 超 cap → disk-from-start)** | **0.9892** (ARM) / **0.9713** (x86) | **0.9965** (差 **0.73-2.52pp**) |

**触发条件**:`parallel_workers > 0` **且** `maintenance_work_mem` 不足以容纳整图 DSM 估算。

## 影响范围

- 仅 **PostgreSQL plugin** `vexdb_graph` index 的 parallel CREATE INDEX
- 仅 `max_parallel_maintenance_workers > 0` + `maintenance_work_mem` 不够装下 `estimated_rows × per_elem_size` 时触发
- **DuckDB 后端不受影响**
- serial build(`max_parallel_maintenance_workers = 0`)不受影响

## 推测根因(未完全定位)

- lite parallel insert 路径有未定位的 race
- `stat` cache(`BitSpan<uint>` on c-index)在并发反向边更新场景下与邻居数组 idx 可能错位
- 实测**关掉 stat cache** 让 recall 大幅退步(从 0.9979 → 0.9237),说明 cache 是算法必需,**不是单纯的优化错误**
- 真正根因需要 TSan / instrument dump / 主库逐行 walk-through 才能定位

## 临时 workaround(给生产用户)

按推荐顺序:

1. **首选**:**调大 `maintenance_work_mem`**,使 DSM 估算装下整图。lite 的 DSM 估算 ≈ `rows × (vec_size + 2×m×sizeof(T) + 一些 padding)`。SIFT1M m=16 vec_size=512B ≈ 800MB,建议 `mwm = 1GB+`
   ```sql
   SET maintenance_work_mem = '2GB';
   ```
2. **次选**:**用 serial build**,recall 接近 pgvector
   ```sql
   SET max_parallel_maintenance_workers = 0;
   ```
3. **不推荐**:`max_parallel_maintenance_workers > 0` + `maintenance_work_mem` 不足。**接受 recall 低 0.7-2.5pp**。

## 不修的理由

1. **serial 已生产可用**(0.9931 ≈ pgvector 0.9964 差 0.33pp)
2. parallel + mwm 不足是**边缘场景**(prod 通常给 mwm ≥ 数据集大小)
3. 已多轮失败迭代,**无 TSan/instrument 支撑下属于盲改**(参见 `docs/decisions/2026-05-21_stop-parallel-hybrid-tuning.md`)
4. 主库 parallel insert 实现需独立 sprint walk-through(预估 1-2 周)
5. v1 兼容矩阵 + 发版节奏 > 这 2pp recall gap

## 重启条件

参见 `docs/decisions/2026-05-21_stop-parallel-hybrid-tuning.md` 的"何时可重启 P1"段。

## 相关 commits

```
0a618dd0f1  parallel build DSM 估算加 mwm cap
1ce70f521a  backward select_neighbors dist_cache key collision
c84b4c4f3b  out_of_memory safety margin 只 gate serial
8e6639bf10  serial: 取消 mwm<1GB 早退,对齐主库 hybrid
```

## 相关文档

- `docs/decisions/2026-05-21_stop-parallel-hybrid-tuning.md` — 止损决策记录

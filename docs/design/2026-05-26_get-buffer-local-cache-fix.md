# get_buffer 高并发 locmap 锁争用修复设计

**日期**: 2026-05-26
**关联**: `docs/reports/2026-05-26_vexdb-reads16-flamegraph.md`
**状态**: 设计待评审

## 1. 问题回顾

reads=16 高并发下 `VecBufferManager::get_buffer`(`src/vector_smgr.cpp:622`)占 ~67% CPU。
每次向量访问都对**唯一共享的 `cur_pool.locmap`**(boost `concurrent_flat_map`)调
`try_emplace_or_cvisit`。`perf annotate` 显示热点在 boost reader 自旋锁
`rw_spinlock::lock_shared()` 的 `lock cmpxchg` + 重试自旋(≈55% CPU),**不是** `ref_count`
原子(~8%)。

根因机制(修正同事的假设):
- 不是 OS 标脏(sys CPU 仅 3.7%,96% 用户态)。
- "同时访问的点几乎不重叠"被 **HNSW hub 访问模式推翻**:每次搜索都从同一 entry point
  出发,反复访问少数 hub 节点 → 这些 sig 落入 boost map 同一 group → 16 backend 争用
  同一 group 的 reader 锁字 `state_` → cacheline 乒乓。
- ref_count 不是主因(同事这点对)。

## 2. 调用模式(决定方案的关键事实)

热点调用方全部是 `graph_index_storage.h` 的 `get_distance*`:
```cpp
VecBuffer vec_buf = vec_read_buffer(index, id, elem_size, st);  // pin
char *val = vec_buf.get_vecbuf();
dist = d.get_distance_single(query, val, dim);                   // 用一次
vec_buf.release();                                               // 立即归还
```
- 借用-计算-归还紧凑,buf 不跨另一次 get_buffer 持有。
- backend 内搜索单线程(PG fork 模型,每 backend 一进程一线程)。
- 一次查询内对 hub 节点反复 get_buffer(成千次),命中率高。

## 3. 现有 pin / evict 不变量(正确性基石)

- `get_buffer` 命中:在 locmap bucket 锁下 `ref_count.fetch_add(1)`。
- `release`:`ref_count.fetch_sub(1)`。
- `do_evict`(`src/vector_smgr.cpp:393`):在 **block 自旋锁 + locmap bucket 锁** 下,
  **仅当 `ref_count==0`** 才回收 slot(push freelist + `sig.rel_id=InvalidOid`)。
- `vec_invalidate_buffer_cache`(`:895`/`:931`):同样仅 `ref_count==0` 才回收;否则
  `set_invalid()` 标记但不回收。

**核心推论**:只要某 slot `ref_count≥1`,evict 与 invalidate 都无法回收它 → 其
`tag.sig` 保持不变。**持有 pin = sig 稳定 = 无需校验。**

## 4. 方案:per-backend 本地缓存(持久 pin)

每 backend 一个进程本地 LRU:`BufferSignature → VecBufferLoc`,缓存条目自身持有一份
**常驻 pin**(ref_count+1)。

### 读路径
```
get_buffer_fast(sig):
  命中本地缓存:
    loc = entry.loc            # 常驻 pin 保证 sig 不变,无需校验、不碰 locmap
    [变体决定是否再 +1 caller pin]
    return VecBuffer(loc)
  未命中:
    res = get_buffer(...)      # 走原慢路径(locmap pin +1 = 常驻 pin)
    本地 LRU 插入 (sig→res.loc)
    LRU 满 → 淘汰最旧条目 → 对其 release 常驻 pin (-1)
    return res
```

### 三个变体(待评审取舍)

| 变体 | 命中路径成本 | 正确性依赖 | 风险 |
|---|---|---|---|
| **A: caller 再 pin** | 1× `ref_count.fetch_add` + lookup;release 时 `fetch_sub` | 常驻 pin 保 sig 稳定;caller pin 保 LRU 淘汰不影响在用 buf | 最稳;hub 上 ref_count xadd 有争用但单条指令无重试 |
| **B: 借用不计数** | 纯 hash lookup,**零原子零锁**;release 为 no-op(哨兵 pool_offset=-2) | 还需保证 LRU 不在 buf 在用期间淘汰该条 | 若 caller 跨多次 get_buffer 持有 buf 且 LRU churn,use-after-evict |
| **C: 仅每查询/每 scan 作用域** | 同 A/B,但缓存生命周期=单次 index scan,scan 结束全部 release | 自动 invalidation 安全(scan 内无并发写本表) | 跨查询不缓存,但 hub 重用在 scan 内已捕获 |

### 生命周期 / 失效(correctness 关键)

本地缓存持久 pin 会**阻止** evict/invalidate 回收该 slot(因 ref_count≥1)。
若另一 backend 改写/删除该向量,`vec_invalidate_buffer_cache` 只能 `set_invalid()` 标记,
无法回收;本地缓存会**继续服务旧数据 → 正确性 bug**(跨 backend 无 sinval 通知)。

候选缓解:
1. **每 scan/每事务作用域**(变体 C):scan 结束 flush(release 全部常驻 pin)。ANN
   benchmark 读多写少,scan 内本表无并发写,天然安全。**首选**。
2. 注册 `CacheInvalidateRelcache` / sinval 回调,写发生时 flush 本地缓存。复杂。
3. 本地缓存条目存 `sig` + 每次命中校验 `tag[loc].sig==sig`:但持久 pin 下 sig 不变,
   校验永远通过,挡不住"逻辑失效但物理 slot 没变"的情况。无效。

## 5. 倾向方案

**变体 C + A 的组合**:缓存作用域绑定单次 index scan(`GraphIndex` scan state 上挂一个
有界 LRU,如 256 条);命中路径用 caller-pin(变体 A 的稳健性);scan teardown 时
release 全部常驻 pin。

理由:
- 自动 invalidation 安全(无需 sinval)。
- hub 节点重用发生在单次 scan 的搜索循环内(成千次 get_buffer),C 足以消除 locmap 争用。
- 变体 A 的 caller-pin 避免依赖"LRU 不淘汰在用 buf"的脆弱假设。

## 5b. 评审结论(2026-05-26,三位 reviewer)

三份独立评审一致裁决 **⚠️有条件赞成**,并推翻原"C+A"倾向。要点:

### 必须修正(否则不可上线)
1. **砍掉变体 A,改纯 C(release no-op)**。A 的 caller-pin 把 55% 的 `lock_shared`
   争用换成 ~8% 的 `ref_count.fetch_add/fetch_sub` 争用,落在 hub slot 同类 hot
   cacheline 上,收益停在 ~90% pgvector;只有 release no-op 才吃掉这 55%+8%,Amdahl
   估算可达 ~2700 QPS 追平 pgvector。**已用代码确认**:search 候选集只存
   `(id,layer,dist)`(algorithm.h:128/186),`get_distance` pin-用-release **不跨持有
   buf** → 变体 B/C 的 borrow-no-recount 在 search 路径安全。
2. **crash / longjmp 泄漏是最高风险 gate**。持久 pin 写共享内存 `ref_count`,释放责任
   在进程私有 LRU。SIGKILL/segfault/OOM-kill 不跑回调 → 泄漏的 `ref_count` 永久 ≥1 →
   slot 永不可 evict,**甚至永久卡死 `wait_locmap_freeze`(:824 无限重试)与
   `redistribute_block` 块再分配**。记忆 `feedback_parallel_build_oom_let_crash` 明确
   "crash 是常态",所以泄漏必然发生。**全仓现无任何 ResourceOwner/XactCallback/PG_TRY
   兜底复位 ref_count**(已 grep 确认)。
3. **失效正确性**:持久 pin 让 invalidate 只能 `set_invalid()` 无法回收 → 服务旧数据。
   纯 scan 作用域不足以覆盖:同事务先查后写、holdable cursor、VACUUM 重叠。flush 边界
   应收到**单次 top-k 搜索循环**级,而非整个 index scan。
4. **build / parallel worker**:已确认 build 走 `WithBulkbuf=true`(bulkbuf)**不经
   get_buffer**,天然免疫;但仍需断言本地缓存只在 `WithBulkbuf=false` 的 scan 路径启用。

### 明确否决
- **"按 sig 高位分 N 个 locmap"无效**:boost concurrent_flat_map 锁已是 **per-group**
  (每 15 槽一把 rw_spinlock,`concurrent_table.hpp:1159`),map 已有数千 group。分 map
  后 hub 仍落各自 group,争用密度不变。
- **cvisit-first 只是免费小优化**:cvisit 的 shared 路径仍调 `lock_shared()` 即仍 CAS
  `state_`,省不掉那 55%。可顺手做,但不能当主方案。

### 动手前必须验证(实证缺口)→ V1 已验证(2026-05-26)
- **V1. 55% 口径 → 确认是全局占比,收益充足**。在测试机 x86 test host 重抓 fp perf
  (debug so,reads=16 QPS≈1288 满载):`perf report --no-children` 显示
  **`VecBufferManager::get_buffer` 全局 self% = 74.29%**(第二名 `GraphIndexAlgorithm
  ::search` 仅 10.37%)。结合上轮 annotate(get_buffer 内部 lock_shared 自旋指令
  cmpxchg 28.96%+jne 26.12%+jmp 19.08% ≈ 74%),得 lock_shared ≈ 74.29%×74% ≈ **55%
  全局 CPU**。**Amdahl**:去掉 55% → 1/0.45 ≈ 2.22× → 1288×2.22 ≈ **2860 QPS >
  pgvector 2524**。纯 C(命中走本地、release no-op)收益上限充足。reviewer 担心的
  "可能只是 get_buffer 内部 55%(全局37%)"被排除。
- **V2. hub group**:`perf c2c` 在该云 ECS 不可用(memory events disabled),未单独验证。
  但对决策不关键——"分 map 无效"已由 boost per-group 锁粒度(代码)否决,V2 仅学术确认。

### 操作教训(踩坑)
- **替换 .so 必须先 stop PG**:运行时 `cp` 覆盖正被 mmap 的 `pg_vexdb.so` → 代码页与磁盘
  文件不一致 → `vector buffer worker` SIGBUS(signal 7)→ PG crash-restart。还原顺序:
  stop → cp → start,不要 `cp` 后 `restart`。
- **idle 测量要用差分**:读 `/proc/stat` 的 `$5` 累计值除以总累计 = 自开机平均 idle,
  恒≈95%,与当前负载无关。负载自检必须采两次算 delta(diag.sh 的 U1-U0 写法才对)。
  本次因此误判"负载未起来"白跑数轮。
- **冷启 reads=16 vs reads=1**:pool warm 后 reads=1 QPS≈311、reads=16 QPS≈1269/1288
  均正常;之前误以为冷启 stall,实为 idle 测量 bug。

### 修订方案(取代第 5 节)
**纯变体 C + borrow-no-recount + 强制兜底**:
- 每 backend 一个有界 LRU(`sig→VecBufferLoc`,上限初定 **64~128**,需按 hub 实际集合
  大小 + pool 配额实测,防止持久 pin 占满 pool 加剧 major-fault 抖动)。
- 命中:纯 hash lookup,返回 `pool_offset=-2` 哨兵 VecBuffer,`release()` 为 no-op
  (零原子零锁)。
- 未命中:走原 get_buffer(locmap pin = 常驻 pin),插入 LRU;LRU 淘汰时
  `release_vector_buffer` 减常驻 pin。
- **生命周期挂 `RegisterResourceReleaseCallback`(RESOURCE_RELEASE_BEFORE_LOCKS)+
  rescan 显式 flush**,保证 query abort / error longjmp / rescan 都强制 release 全部
  常驻 pin。**LRU 元数据内存与 pin 释放生命周期解耦**(context reset 前先 flush)。
- **crash 兜底**:正常/abort 路径靠回调;硬 crash(SIGKILL)无法回调 → 接受
  "crash 后 vecbuf 共享池整片重置"(与现有 "crash→PG restart 兜" 运维模型一致),
  在 postmaster crash-restart 路径清 `ref_count`。**此项需拍板**。

## 5c. 实现与验证结果:方案证伪(2026-05-26)

按修订方案(纯 scan 作用域 + borrow-no-recount)实现并在测试机 x86 test host 验证:

- 实现:`include/local_vec_cache.h` + `src/local_vec_cache.cpp`,接入 `vec_read_buffer`
  快路径、`VecBuffer::release` 哨兵(pool_offset=-2)、`graph_index_scan.cpp` 的
  `algo.search` 用 activate/PG_TRY 包裹。编译通过,部署无 crash、recall 正常。
- 结果:**无收益**。reads=16 QPS 1288(基线)→ 1380(search 级)/ 1347(跨 search
  常驻),均在噪声范围,远未达 Amdahl 估算的 2860。
- **命中率实测 0.4%**(hits≈765 / misses≈201136 per backend per 20 万次)→ 方案前提
  (时间局部性 / hub 重复访问)不成立。

### 根因(推翻 5b 之前的假设)
- HNSW search 单次内 `USet visited` 去重(`graph_index_algorithm.h:535/596`),**每个
  向量只访问一次**;跨查询每个 query vector 不同,layer 0 beam search(ef=200)访问各自
  邻域的几百~几千个**不同**向量,跨查询几乎不重复;真正跨查询重复的只有极少数上层 hub
  (顶层几个节点)→ 命中率天生 <0.5%。
- 所以 `get_buffer` self 74% 是**海量不同向量各查一次 locmap** 的开销,**不是少数 hub
  的重复查询**。利用时间局部性的本地缓存对"每个 key 基本只访问一次"的负载无效。
- reviewer `afe28` 的质疑得到证实:"hub sig 落同一 group"从未成立(Hasher 含 offset,
  不同向量落不同 group)。reads=16 退化的真因不是"重复 hub 争用同一 group 锁",而是
  16 backend 并发对 locmap 海量**不同 key** 查询的结构性退化(reads=16 per-core 吞吐 82
  vs reads=1 311)。

### 下一步候选(本地缓存方向作废)
1. **重新定位退化机制**:reads=16 vs reads=1 用 `perf stat` 比 LLC-load-misses /
   内存带宽 / context-switches,判断是 (a) 内存带宽饱和(16× locmap group 数组遍历),
   还是 (b) boost concurrent_flat_map 跨 group 共享状态(size_ 原子等),还是 (c) hub
   节点 ref_count cacheline 争用,还是 (d) PG buffer/pin 层。c2c 不可用,改 perf stat
   + 事件采样定位。
2. **若是 (b) 锁本身固有开销**:get_buffer 命中常见时改 `cvisit` 替代
   `try_emplace_or_cvisit`(只读 shared 路径,省 emplace 探测)——但 lock_shared 的
   cmpxchg 仍在,收益有限,需实测。
3. **若是 (a) 带宽**:locmap 结构/布局优化(缩小 group entry、提高 cache 命中)。
4. 代码改动暂留在分支 `perf/get-buffer-local-cache`,不合并。

## 5d. locmap 分区方案:成功(2026-05-26)

放弃本地缓存,改对标 pgvector 把单一共享 locmap 分区。实现:`VecBufferPool::locmap`
从单个 `Holder<bufmap>` 改为 `Holder<bufmap> locmap[LOCMAP_NSHARD]`(NSHARD=64),加
`locmap_for(sig)`(hash 低位选分区,group 用高位,正交);8 个单 sig 访问点改走
`locmap_for(sig)`,2 个全表操作(invalidate 全表 / verify)遍历所有分区。本地缓存代码
保留但 dormant(scan 不再 activate)。

测试机 x86 test host reads=16 验证(plot.py 对 ground truth 算 recall):

| 指标 | 基线 | **分区(64)** | pgvector |
|---|---|---|---|
| reads=16 QPS | 1288 | **3021** | 2524 |
| 提升 | — | **2.35×** | — |
| recall@10 | — | **0.9966** | 0.9971 |
| reads=1 QPS | 311 | 298 | 263 |

**结论:同等 recall(0.997)下 vexdb 分区版 QPS 反超 pgvector 19.7%,无 crash。**
完美对上 Amdahl 估算(去 55% locmap 争用 → 2.2× → ~2860,实测 3021)。证实退化来自
**单一共享 locmap 实例**的争用(map 级共享状态 + boost per-group reader 锁字
false-sharing),分 64 区后争用密度降 64 倍。reviewer afe 的"分 map 无效"被推翻——其
前提"hub 落同一 group"本就被 0.4% 命中率证伪;真实争用是 16 backend 查**不同 key** 打
同一 map 实例,分片正解。

正确性:recall 0.9966 ≈ pgvector 0.9971(分区不改 HNSW 搜索逻辑,只改 buffer 查找的
map 选址;sig→shard 确定性映射保证一致)。图:`docs/reports/2026-05-26_locmap-shard-recall-qps.png`。

### 待办(收尾)
- 清理 dormant 的本地缓存代码(local_vec_cache.{h,cpp} + vec_read_buffer 快路径 +
  VecBuffer 哨兵),或明确标注保留理由。
- NSHARD=64 的内存开销评估(每 pool 64 个 map 实例 × 每实例 init ~ shard_init entries)。
- 正确性回归:并发 invalidate(UPDATE/DELETE 触发全表/单 sig 失效)在分区下的测试。
- pin 私有化(PrivateRefCount 式)作为进一步优化的后续项(本次未做,分区已达标)。

## 6. 待评审问题(原始,部分已由 5b 回答)

1. 变体 C 的"scan 作用域"是否真能覆盖压测的 hub 重用?会不会漏掉跨 scan/跨查询的重用导致
   收益不足?
2. 持久 pin 占用 slot 是否会加剧 pool 容量抖动(已知 reads=16 major-faults 5.7×)?
   LRU 上限怎么定?
3. caller-pin 的 `ref_count.fetch_add` 在 hub slot 上是否仍构成新瓶颈(16 backend 争用
   同一 ref_count cacheline)?需不需要 padding / per-backend ref?
4. 变体 B(借用不计数)在现有 `get_distance*` 调用模式下是否实际安全(buf 不跨 get_buffer
   持有)?能否用编译期约束保证?
5. 缓存挂在哪:`GraphIndex` scan state? thread_local? backend 全局 + ResourceOwner 回调?
6. 是否有比"per-backend 缓存"更简单的方向(如把单 locmap 按 sig 高位分 N 个独立 map,
   分散 hub group 锁)?这个方向能否同样消除 hub 争用?

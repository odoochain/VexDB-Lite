# NEON vs GENERAL "75 个 .test diff" 定性报告

**Action**: P0 spec #3 (NEON-induced .test diffs) → DELETE，see §Conclusion.

**日期**: 2026-05-15
**触发**: NEONv8 重启（boost.PP `_DISPATCH_PAD_` 哨兵修复）准备落地。集成回归审查把"75 .test diff 性质未定"列为 P0，要求先定性。
**作者**: triage agent
**结论先行**: **前置主张「75 个 .test diff」无法在当前 working tree 复现，证据链严重不足，需要新一轮可重复实验后才能立项；不应继续把它当作 Fix A 的硬阻塞 P0**。

---

## 一、为什么不能在当前 repo 复现

### 1.1 本机硬件不匹配

| 项 | 值 |
|---|---|
| host | `Darwin 25.3.0` |
| `uname -m` | **`x86_64`** (Apple Intel) |
| NEONV8 编译 | 不可能（架构宏 `COMPILER_TARGET_ARM` 评估为假，宏定义直接进 `#define COMPILER_SUPPORT_NEONV8 0`） |

打开 `distance/core/architecture_macro.h:159-194` 的 ARM 关闭块只在 ARM 主机有效。在
x86_64 上即使把这 36 行删掉，`COMPILER_SUPPORT_NEON` 仍被 `#if !defined(__ARM_NEON)` 顶部分支判定为 0。
本机不能编出 NEONV8 dispatcher。

### 1.2 render-duck 是纯 YAML→.test 转译，与 SIMD 路径无关

读 `tests/spec/_lib/render.py`：

- 入参：`tests/spec/**/*.yaml`
- 出参：`vexdb_duckdb/test/sql/vex/spec_run/*.test`
- **不执行 SQL**，只做模板替换 + dialect 字典展开

也就是说：**改 `architecture_macro.h` 之后跑 `make render-duck`，rendered .test 内容不会变一行**。
expected 是从 yaml 里抄过来的，期望值就是期望值，跟 NEON / GENERAL 选了谁无关。

**所以 jury 报告里描述的工作流（"启用 NEON → 渲染 → git diff test/sql/vex/ 看 75 个文件 diff"）在概念上就走不通**。
唯一能让 .test 文件 diff 的，是有人手工跑了测试、把 actual 写回 expected（即 `regenerate`）。
若 bug-analyzer agent 当时做了这一步，那 75 diff 是 **runtime 失败** + 手工 regenerate 的产物，不是 render 流程本身。

### 1.3 标志性「smoking gun」case 在仓库里不存在

bug-analyzer 报告的典型 case：

> `vector_min_function.test` expected `nan`、NEON 实际 `-3.418669`

事实核查：

```bash
$ find vexdb_duckdb/test/sql/vex -name "vector_min*"
(空)
$ find tests/spec -name "*.yaml" -exec grep -l "vector_min\|vec_min" {} \;
(空)
$ find tests/spec -name "*.yaml" | xargs grep -l '\bnan\b\|NaN'
(空，仅 distance_edge_cases.yaml 使用 1e30 / 1e-30 极端值但无 nan)
```

- 仓库里**没有** `vector_min` 函数（vector ops 只有 add / sub / dot / cosine / l2_norm / mul / div，见 `tests/spec/duckdb/functions/vector_ops.yaml`，无 reduce-min）。
- 仓库里**没有** 任何 spec yaml 引入 `nan` 期望值。
- 整个 `vexdb_duckdb/test/sql/vex/spec_run/` 里没有任一 `.test` 文件名匹配 `*min*` 或 `*nan*`。

最合理推断：**bug-analyzer 在「临时 worktree」里 hallucinate 了一个不存在的测试文件名 + 不存在的数值差**。
（另一种可能：跑的是别的 fork / 历史 commit。但无任何工件留下来证伪。）

### 1.4 2026-05-09 ARM NEON retest 给出反向证据

2026-05-09 ARM PG19 NEON retest 报告（commit `358dcde9bd`，PG 路径 NEONV8 真的跑起来了的那次）显式记录：

> Recall stayed unchanged across the retest:
> - 10k: `Recall@10 = 0.999500`, `Recall@100 = 0.995050`
> - 100k: `Recall@10 = 0.997500`, `Recall@100 = 0.974600`
> - 1M:   `Recall@10 = 0.986000`, `Recall@100 = 0.940750`
>
> enabling the NEON path did not change ANN quality in the tested workload —
> the changes are performance-path changes, not algorithm-path changes.

**Recall 完全不变**直接否定了"NEON 算错"假说在算法层成立。
唯一可能的 .test 差异面是 **scalar 标量距离结果**（distance_edge_cases / vector_ops），
那也只能是 FMA / 累加顺序导致的 ULP 级尾位差异——即 **fp-tolerance 类**而非 bug-in-general。

---

## 二、PG 路径 vs DuckDB 路径的差异本质

| 路径 | 比较器 | ULP 容忍度 | NEON ULP diff 会否触发失败 |
|---|---|---|---|
| **PG** spec runner (`tests/spec/_lib/docker/compare.py`) | 浮点 atol=1e-4 / rtol=1e-3 | ✅ 已容忍 | **不会** |
| **DuckDB** spec runner (sqllogictest `.test`) | 字符串精确匹配 | ❌ 零容忍 | **会** |

`compare.py:27-28`：

```python
ATOL = 1e-4
RTOL = 1e-3
```

`cells_equiv()`：浮点 abs/rel 容差 + NaN 双侧匹配 + Inf 严格相等 + 向量字面量元素级容差。

**结论**：PG 端 NEON vs GENERAL 即使有 ULP 差异也不会引发 spec 失败；
真正能产生「75 .test diff」的只能是 DuckDB 端的 sqllogictest 文件（spec_run/ 下 204 个 .test）。
但这 204 个文件本身 99% 是图索引/扫描相关，scalar 距离仅占 5 个文件
（`distance_l2 / distance_edge_cases / distance_simd / vector_ops / distance_defensive`），
绝无可能有 75 个文件命中。

**75 这个数字本身就过于离谱**——本机 sqllogictest `.test` 总数 204，其中：
- 与 SIMD distance 相关的（scalar 暴露具体浮点数）大概率 < 10 个
- 与图索引相关的（暴露 recall / 命中 id 列表）30-40 个
- 与图索引相关的（仅校验 rowcount/explain 不暴露数值）100+ 个

即使所有 distance-scalar + 一半 recall-list 都坏掉，也最多 20+ 个文件，不到 75。

---

## 三、定性判断

| 维度 | 判断 |
|---|---|
| 文献证据（jury 报告） | 把 75 列为 P0，但**未给原始 diff 文件** |
| bug-analyzer agent 证据 | 临时 worktree 已清理，**0 工件保留**；典型 case `vector_min` 不存在 |
| 仓库可复现性 | x86_64 host **不可能**复现 ARM NEONV8 编译 |
| 历史 ARM NEON 实测 | 2026-05-09 retest: recall 不变（反向证据） |
| 比较器设计 | PG 已有 1e-3 / 1e-4 容差，**绝大多数 ULP diff 不会暴露成失败** |
| 数量学合理性 | 204 个 .test 中能暴露具体浮点的 ≤ 30 个，**75 物理上几乎不可能** |

**综合定性**：**「75 个 .test diff」是 bug-analyzer agent 的产物未经核实，倾向于幻觉或夸大**。

真实可能的情形（按可能性排序）：

1. **bug-analyzer hallucinated**（≈ 60%）
   - `vector_min` 不存在直接打脸；
   - 75 数字与仓库结构不符；
   - 工件丢失无可复核。
2. **bug-analyzer 在一个旧 commit / fork 上跑的**（≈ 25%）
   - 描述对得上别的项目，不是当前 dev 分支。
3. **真有少量 (< 20 个) fp-tolerance 类 ULP 差异，被 agent 报大了**（≈ 15%）
   - 即便如此，性质也是 fp-tolerance 而非 bug-in-general（recall 不变 + ATOL 已存在）。

---

## 四、对 Fix A 落地的影响

### 4.1 阻塞性判断

| 阻塞项 | 真实阻塞性 | 备注 |
|---|---|---|
| 75 .test diff 必须先逐项定性 | 🟡 **降级到 P2**（不阻塞） | 证据链不成立，要求先复现 |
| INT8 36-spec fallback byte-exact（spec #2） | 🔴 **保留 P0** | 这个是真实的 |
| Architecture Usage = NEONV8（spec #1） | 🔴 **保留 P0** | 这个是真实的 |
| `-ffp-contract=fast` ULP（spec #4） | 🟡 P1 | 跟 75 diff 是同一类问题，加 1e-3 容差即可 |

### 4.2 推荐落地路径

不应让一个"无法复现"的指控阻塞 Fix A。改走**实测先行**：

1. **Fix A 先编译落地** → 推到 ARM 测试机（<arm-test-host>）；
2. **在 ARM 测试机** 跑 `make render-duck` 后跑 DuckDB sqllogictest 全量：
   ```bash
   ssh build-user@<arm-test-host> 'cd VexDB-Lite && bash build.sh release && \
     ./build/release/test/unittest "test/sql/vex/*" 2>&1 | tee /tmp/neon-test.log'
   ```
3. **统计真实失败数**：
   - 若 ≤ 5 个、且都是 scalar 距离 ULP → 加 sqllogictest `tolerance` 或在 yaml 加 approx 包装，**fp-tolerance 类**；
   - 若有图索引 hit-id 列表错乱 → 才需要警惕 **bug-in-general**；
   - 若 ≥ 50 个失败 → 才回过头来验 bug-analyzer 报告是否成立。
4. **报告把实测结果写回本文档**，更新到正式的 75-diff triage（取决于真实数字）。

### 4.3 建议行动

- NEON 与 GENERAL 任何数值面差异都需要审计；
- 「75 .test diff」需要原始证据，**当前无法在 working tree 复现，亦无原工件**；
- 建议把 spec #3 改成**实测驱动**：Fix A 编译后在 ARM 机跑全量，失败 case 现场归类；
- 历史 retest 报告（2026-05-09）显示 recall 不变，**算法层无 bug 风险**；
- INT8 fallback byte-exact、`index_inspect = NEONV8`、`-ffp-contract=fast` ULP 这三条 P0/P1 全部保留。

---

## 五、若真复现出 diff 后的预案

留作未来执行（不在本报告交付物范围内）：

### 5.1 fp-tolerance 类（预期 80%+）

- 改 yaml expected 加 approx 标记，或在 render.py 给 DuckDB 端注入 sqllogictest `tolerance` 指令；
- 单独 commit `test(spec): allow 1e-3 fp tolerance for NEON-vs-GENERAL ULP diff`；
- **不修源码**。

### 5.2 bug-in-general 类（预期 < 5%，若有）

- 单独 commit `fix(distance/general): correct NaN propagation / accumulation order`；
- 按 NEON 输出重新渲染对应 .test；
- **PR 描述里独立列项**，不混在 Fix A NEONV8 重启 commit 里。

### 5.3 混合（预期 < 15%）

- 拆两条 commit（先 bug fix 再 tolerance），保持 git history 可审计。

---

## 六、未解决问题（留给真复现后填）

1. ARM 测试机上 `bash build.sh release` 后跑 sqllogictest 全量，真实失败 case 列表？
2. INT8 36-spec 在 PG ARM 路径下 byte-exact 是否真的成立（需要 `gdb` 对比或 manylinux 容器复测）？
3. `-ffp-contract=fast` 在 macOS arm64（M-series）和 Kunpeng/Kirin 上的 ULP 量级是否相同？

---

## 七、交付物 / 行动建议

- ✅ 本报告 `docs/analysis/2026-05-15_neon-75-diff-triage.md`
- 📋 **建议把"75 .test diff 性质未定"从 P0 降级到 P2，并标注「证据不足，待实测复现」**；
- 📋 **建议 spec #3** 改成 "Fix A 落地后在 ARM 测试机跑全量 sqllogictest，失败 case 现场归类入本文档"。

**Fix A 不应被这一项阻塞**。

---

## 附：核查命令清单

```bash
# 确认无 vector_min
find vexdb_duckdb/test/sql/vex tests/spec -iname "*vector_min*" -o -iname "*vec_min*"

# 确认无 nan 期望
grep -rli '\bnan\b\|NaN' tests/spec/duckdb tests/spec/shared

# 确认 PG 比较器有容差
grep -nE 'ATOL|RTOL|cells_equiv' tests/spec/_lib/docker/compare.py

# 确认 host 无 ARM
uname -m  # 期望 x86_64，不能编 NEONV8

# 引用历史 ARM NEON retest（recall 不变）
# 见 2026-05-09 ARM PG19 NEON retest 报告（commit 358dcde9bd）
```

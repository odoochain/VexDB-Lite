# VexDB-Lite 全平台本地构建 + 测试验证方案

## Context

VexDB-Lite 公众号文章展示了 7 种平台接入方式（Python/iOS/Android/WASM/Java/Rust/Node.js），需要对每个平台进行本地构建并验证功能正确性。目前 Python/iOS/Android/WASM 已有构建脚本，Java/Rust/Node.js 需要新建。所有平台缺少统一的功能验证测试。

---

## 第一部分：本地构建（7 个平台）

### 已有平台（直接构建）

| # | 平台 | 命令 | 产物 |
|---|------|------|------|
| 1 | macOS Release | `bash build.sh release` | `duckdb/build/release/src/duckdb` |
| 2 | Python wheel | `bash packaging/build_wheel.sh dist` | `dist/duckdb-1.5.0-*.whl` |
| 3 | iOS device | `bash build.sh ios` | `duckdb/build/ios_arm64/libvexdb/libvexdb.a` |
| 4 | iOS simulator | `bash build.sh ios --sim` | `duckdb/build/ios_sim/libvexdb/libvexdb.a` |
| 5 | Android arm64 | `bash build.sh android` | `duckdb/build/android_arm64-v8a/libvexdb/libvexdb.a` |
| 6 | WASM | `bash build.sh wasm` | `duckdb/build/wasm/src/libduckdb_static.a` |

### 新增平台（需创建打包脚本）

| # | 平台 | 脚本 | 产物 |
|---|------|------|------|
| 7 | Java JDBC | `packaging/build_java.sh` | `dist/vexdb-lite-1.5.0.jar` |
| 8 | Rust | `packaging/build_rust.sh` | `dist/vexdb-lite-rs/` (本地 crate) |
| 9 | Node.js | `packaging/build_node.sh` | `dist/vexdb-lite-*.tgz` |

### 新增共享脚本

**`packaging/inject_vex.sh`** — extracts existing injection logic into a reusable function：
1. 复制 `extension/vex/` 到目标 DuckDB 源码树
2. 复制 `physical_create_graph_index.{cpp,hpp}`
3. 补丁 `schema/CMakeLists.txt`
4. 添加 `duckdb_extension_load(vex)` 到 `extension_config.cmake`

现有的 `build_wheel.sh` / `prepare_source.sh` 也重构为调用 `inject_vex.sh`。

### 新增语言打包策略

**Java**: clone `duckdb-java` v1.5.0.0 → clone DuckDB v1.5.0 源码 → `inject_vex.sh` 注入 → `vendor.py --duckdb` 重新生成 CMakeLists → `make release` → 产出 JAR

**Rust**: clone `duckdb-rs` → 解压内置 `duckdb.tar.gz` → 注入 VEX 源码 → 更新 `manifest.json` → 重新打包 → patch `build.rs` 启用 VEX → `cargo build --release --features bundled`

**Node.js**: clone `duckdb-node` → clone DuckDB v1.5.0 源码 → `inject_vex.sh` 注入 → `vendor.py --duckdb` 重新生成 `binding.gyp` → `npm install` + `npx node-pre-gyp build` → `npm pack`

### 前置依赖安装（一次性）

```bash
# Android NDK
export ANDROID_NDK="$HOME/Library/Android/sdk/ndk/27.0.12077973"

# Emscripten (WASM)
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest

# Java
brew install maven openjdk@17

# Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Node.js
brew install node
```

---

## 第二部分：跨平台测试验证框架

### 架构

```
Layer 3: 语言绑定测试 (pytest / JUnit / cargo test / jest)
Layer 2: SQL 测试套件 (现有 80+ .test 文件，桌面端)
Layer 1: C API 核心测试 (test_vexdb.c，全平台通用)
```

### Layer 1：C API 核心测试（全平台）

纯 C 程序，使用 DuckDB C API (`duckdb.h`)，可交叉编译到所有目标平台。

**文件结构**：
```
test/capi/
  CMakeLists.txt           # 构建规则（桌面 + 交叉编译）
  test_vexdb.c             # 主驱动
  test_helpers.h           # ASSERT_OK / ASSERT_FLOAT_EQ 等宏
  tests/
    test_basic.c           # DB 打开/关闭、扩展加载验证
    test_types.c           # FLOAT[N] 创建、NULL、维度校验
    test_distance.c        # L2/cosine/IP 正确性（已知向量）
    test_index.c           # GRAPH_INDEX 建索引 + ANN 搜索 + 召回率
    test_persistence.c     # checkpoint → 重启 → 数据完整性
    test_pq.c              # PQ 量化索引正确性
    test_hybrid.c          # HYBRID_INDEX (条件编译)
  data/
    vectors_128d_1000.h    # 编译时嵌入的测试向量（固定种子）
    gen_test_vectors.py    # 生成上述头文件的脚本
```

**核心测试用例**：

| 模块 | 用例数 | 关键验证点 |
|------|-------|-----------|
| test_basic | 3 | DB 打开关闭、VEX 扩展已加载 |
| test_types | 5 | FLOAT[128] CRUD、NULL、维度不匹配报错 |
| test_distance | 8 | L2/cosine/IP 已知值验证、128 维高维一致性 |
| test_index | 6 | GRAPH_INDEX 三种 metric、1000 向量 recall ≥ 95% |
| test_persistence | 3 | checkpoint 后重开 DB、插入/删除后一致性 |
| test_pq | 3 | PQ 索引创建、搜索 recall ≥ 85%、持久化 |
| test_hybrid | 3 | HYBRID_INDEX 创建、分区过滤搜索、持久化 |

**召回率测试算法**：
1. 插入 1000 条 128 维向量（编译时嵌入，固定种子）
2. 建索引前先暴力搜索 10 个 query 的 top-10 作为 ground truth
3. 建 GRAPH_INDEX，设 `ef_search=100`，ANN 搜索同样 query
4. recall = |交集| / 10，取 10 个 query 平均值，断言 ≥ 0.95

### 各平台执行方式

| 平台 | 构建方式 | 执行方式 |
|------|---------|---------|
| macOS | CMake 原生编译，链接 `duckdb_static` | 直接运行 `./test_vexdb` |
| iOS Simulator | ios-toolchain 交叉编译，链接 `libvexdb.a` | `xcrun simctl spawn booted ./test_vexdb` |
| Android | android-toolchain 交叉编译（x86_64），链接 `libvexdb.a` | `adb push` + `adb shell` 在模拟器运行 |
| WASM | `emcmake` 编译，链接 `libduckdb_static.a` | `node --experimental-wasm-threads test_vexdb.js` |

**集成到 build.sh**：新增 `test-capi` 命令
```bash
bash build.sh test-capi                # 桌面原生
bash build.sh test-capi --ios-sim      # iOS 模拟器
bash build.sh test-capi --android      # Android 模拟器
bash build.sh test-capi --wasm         # Node.js WASM
```

### Layer 3：语言绑定测试

| 语言 | 测试文件 | 框架 | 执行 |
|------|---------|------|------|
| Python | `test/python/test_vexdb.py` | pytest | `pip install dist/*.whl && pytest test/python/` |
| Java | `test/java/.../VexDBTest.java` | JUnit 5 | `cd test/java && mvn test` |
| Rust | `test/rust/tests/test_vexdb.rs` | cargo test | `cd test/rust && cargo test` |
| Node.js | `test/nodejs/test_vexdb.test.js` | jest | `cd test/nodejs && npm test` |

每个语言测试覆盖：扩展加载、距离函数、GRAPH_INDEX ANN 搜索、召回率。

---

## 第三部分：实施顺序

| 阶段 | 内容 | 涉及文件 |
|------|------|---------|
| **1** | 创建 `inject_vex.sh`，重构 `build_wheel.sh` 使用它 | `packaging/inject_vex.sh`, `packaging/build_wheel.sh` |
| **2** | 构建已有平台（iOS/Android/WASM），验证产物正常 | `build.sh` (现有) |
| **3** | 创建 C API 核心测试 + 测试向量生成 | `test/capi/*` |
| **4** | 桌面端运行 C API 测试，调通 | `build.sh test-capi` |
| **5** | iOS Simulator / Android Emulator / WASM 交叉编译 + 运行测试 | `test/capi/CMakeLists.txt`, `build.sh` |
| **6** | 创建 Java/Rust/Node.js 打包脚本 | `packaging/build_{java,rust,node}.sh` |
| **7** | 创建语言绑定测试 | `test/{python,java,rust,nodejs}/` |
| **8** | 全平台端到端验证 | 所有 |

---

## 验证方式

每个平台构建完成后执行对应测试，全部通过即为验证成功：

```bash
# 1. 桌面 C API 测试
bash build.sh test-capi

# 2. Python
pip install dist/*.whl --force-reinstall --no-deps && pytest test/python/ -v

# 3. iOS Simulator
bash build.sh test-capi --ios-sim

# 4. Android Emulator
bash build.sh test-capi --android

# 5. WASM
bash build.sh test-capi --wasm

# 6. Java
bash packaging/build_java.sh dist && cd test/java && mvn test

# 7. Rust
bash packaging/build_rust.sh dist && cd test/rust && cargo test

# 8. Node.js
bash packaging/build_node.sh dist && cd test/nodejs && npm test
```

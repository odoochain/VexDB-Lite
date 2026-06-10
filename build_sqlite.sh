#!/bin/bash
# vexdb_lite SQLite 适配层独立构建。
# 仿 vexdb_pg 独立 CMake 模式（根 build.sh 深耦合 DuckDB 源码树，不复用）。
#
#   bash build_sqlite.sh build    # 配置 + 编译双形态 + smoke test 二进制
#   bash build_sqlite.sh test     # build + 跑五层测试（smoke ×4 + spec）
#   bash build_sqlite.sh package  # Release 编 macOS arm64+x86_64 → dist/ tarball + SHA256
#   bash build_sqlite.sh vendor   # 仅拉取 SQLite amalgamation
#   bash build_sqlite.sh clean    # 删 build 目录
set -e

DIR="$(cd "$(dirname "$0")" && pwd)/vexdb_sqlite"
BUILD_DIR="$DIR/build"
CMD="${1:-build}"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

# 解析 cmake：优先 PATH，其次 conan/brew 常见位置（非交互 shell 常无 cmake on PATH）。
CMAKE="$(command -v cmake || true)"
if [ -z "$CMAKE" ]; then
    for c in \
        /opt/homebrew/bin/cmake \
        /usr/local/bin/cmake \
        "$HOME"/.conan/data/cmake/*/_/_/package/*/CMake.app/Contents/bin/cmake \
        "$HOME"/.conan2/p/cmak*/p/bin/cmake \
        /Applications/CMake.app/Contents/bin/cmake; do
        [ -x "$c" ] && CMAKE="$c" && break
    done
fi
[ -z "$CMAKE" ] && { echo "找不到 cmake：brew install cmake，或激活含 cmake 的 conan/conda 环境" >&2; exit 1; }

case "$CMD" in
    vendor)
        bash "$DIR/vendor_sqlite.sh"
        ;;
    build|test)
        [ -f "$DIR/third_party/sqlite/sqlite3ext.h" ] || bash "$DIR/vendor_sqlite.sh"
        "$CMAKE" -S "$DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}"
        "$CMAKE" --build "$BUILD_DIR" -j "$NCPU"
        echo ""
        echo "=== 产物 ==="
        ls -la "$BUILD_DIR"/*.dylib "$BUILD_DIR"/*.so "$BUILD_DIR"/*.a 2>/dev/null || true
        if [ "$CMD" = "test" ]; then
            echo ""
            echo "=== M0 静态注册冒烟 ==="
            "$BUILD_DIR/m0_static_smoke"
            echo "=== M1 距离层冒烟 ==="
            "$BUILD_DIR/m1_distance_smoke"
            echo "=== M2 虚拟表冒烟 ==="
            "$BUILD_DIR/m2_vtab_smoke"
            echo "=== M3 HNSW 冒烟 ==="
            "$BUILD_DIR/m3_hnsw_smoke"
            echo "=== M4 spec（L2 DSL 渲染 + runner） ==="
            bash "$DIR/../tests/spec/_lib/docker/run_sqlite.sh"
        fi
        ;;
    package)
        # macOS 双 arch 发版包：loadable dylib + 静态 .a + 公共头 + 文档。
        # linux x86_64/aarch64 走 scripts/release.sh 的远程 manylinux 链（sqlite target）。
        [ -f "$DIR/third_party/sqlite/sqlite3ext.h" ] || bash "$DIR/vendor_sqlite.sh"
        VERSION="${VEXDB_SQLITE_VERSION:-0.1.0}"
        DIST="$DIR/../dist/sqlite"
        mkdir -p "$DIST"
        for ARCH in arm64 x86_64; do
            BD="$DIR/build-pkg-$ARCH"
            "$CMAKE" -S "$DIR" -B "$BD" -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_OSX_ARCHITECTURES=$ARCH -DVEXDB_SQLITE_BUILD_TESTS=OFF
            "$CMAKE" --build "$BD" -j "$NCPU"
            PKG="vexdb-lite-sqlite-${VERSION}-macos-${ARCH}"
            STAGE="$DIST/$PKG"
            rm -rf "$STAGE"
            mkdir -p "$STAGE"
            cp "$BD/vexdb_lite.dylib" "$STAGE/"
            cp "$BD/libvexdb_lite_static.a" "$STAGE/"
            cp "$DIR/include/vexdb_sqlite.h" "$STAGE/"
            cp "$DIR/README.md" "$STAGE/"
            tar -C "$DIST" -czf "$DIST/$PKG.tar.gz" "$PKG"
            rm -rf "$STAGE"
            echo "打包: $DIST/$PKG.tar.gz"
        done
        ( cd "$DIST" && shasum -a 256 *.tar.gz > SHA256SUMS )
        echo ""
        ls -la "$DIST"/*.tar.gz "$DIST/SHA256SUMS"
        ;;
    clean)
        rm -rf "$BUILD_DIR" "$DIR"/build-pkg-*
        echo "已清理 build 目录"
        ;;
    *)
        echo "Usage: $0 [build|test|package|vendor|clean]"
        exit 1
        ;;
esac

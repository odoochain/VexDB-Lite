# 由 DuckDB 构建系统(根 Makefile 的 EXT_CONFIG)引入，声明要构建的扩展。
# SOURCE_DIR 指向本目录(community/CMakeLists.txt 所在)。
duckdb_extension_load(vexdb_lite
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

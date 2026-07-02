// 向量值解析/编码 —— UDF 与 GRAPH_INDEX 虚拟表共用。
//
// 输入双形态：float32 BLOB（小端平铺，零拷贝视图）或 JSON 数组 TEXT（解析进
// owned 缓冲）。维度上限 65535（distance dispatcher 的 uint16 dim）。
#ifndef VEXDB_SQLITE_VECTOR_CODEC_H
#define VEXDB_SQLITE_VECTOR_CODEC_H

#ifdef VEXDB_SQLITE_CORE
#include "sqlite3.h"
#else
#include "sqlite3ext.h"
#endif

#include <string>
#include <vector>

namespace vexdb_sqlite {

constexpr size_t kMaxDim = 65535;

struct VectorView {
    const float *data = nullptr;
    size_t dim = 0;
    std::vector<float> owned;  // TEXT 解析时持有数据
};

// 解析 '[1.0, 2.0]' 形式的 JSON 数组。失败返回 false 并填 err。
bool ParseJsonArray(const char *text, std::vector<float> &out, std::string &err);

// 把一个 SQL 值解析成向量视图（BLOB 零拷贝 / TEXT 解析）。
bool GetVector(sqlite3_value *val, VectorView &out, std::string &err);

}  // namespace vexdb_sqlite

#endif  // VEXDB_SQLITE_VECTOR_CODEC_H

#include "vexdb_sqlite_internal.h"
#include "functions/vector_codec.h"

#include <cstdlib>

namespace vexdb_sqlite {

bool ParseJsonArray(const char *text, std::vector<float> &out, std::string &err) {
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '[') {
        err = "vector text must be a JSON array like '[1.0, 2.0]'";
        return false;
    }
    p++;
    while (true) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']') { p++; break; }
        char *end = nullptr;
        float v = strtof(p, &end);
        if (end == p) {
            err = "invalid number in vector JSON at offset " + std::to_string(p - text);
            return false;
        }
        out.push_back(v);
        p = end;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ',') { p++; continue; }
        if (*p == ']') { p++; break; }
        err = "expected ',' or ']' in vector JSON at offset " + std::to_string(p - text);
        return false;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '\0') {
        err = "trailing garbage after vector JSON";
        return false;
    }
    if (out.empty()) {
        err = "vector must not be empty";
        return false;
    }
    return true;
}

bool GetVector(sqlite3_value *val, VectorView &out, std::string &err) {
    switch (sqlite3_value_type(val)) {
    case SQLITE_BLOB: {
        int len = sqlite3_value_bytes(val);
        if (len <= 0 || len % 4 != 0) {
            err = "vector blob length must be a positive multiple of 4 (float32), got " +
                  std::to_string(len);
            return false;
        }
        out.data = static_cast<const float *>(sqlite3_value_blob(val));
        out.dim = static_cast<size_t>(len) / 4;
        break;
    }
    case SQLITE_TEXT: {
        const char *text = reinterpret_cast<const char *>(sqlite3_value_text(val));
        if (!ParseJsonArray(text, out.owned, err)) return false;
        out.data = out.owned.data();
        out.dim = out.owned.size();
        break;
    }
    default:
        err = "vector argument must be a float32 BLOB or JSON array TEXT";
        return false;
    }
    if (out.dim > kMaxDim) {
        err = "vector dimension " + std::to_string(out.dim) + " exceeds max " +
              std::to_string(kMaxDim);
        return false;
    }
    return true;
}

}  // namespace vexdb_sqlite

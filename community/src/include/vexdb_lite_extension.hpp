#pragma once

// DuckDB's static-link loader codegen (generated_extension_headers.hpp) includes
// "<ext>_extension.hpp" resolved from the extension SOURCE_DIR/src/include. The
// canonical declaration lives in vexdb_duckdb/include/; this header makes it
// visible at the location DuckDB's generated loader expects when vexdb_lite is
// linked statically (community-extensions CI builds the in-tree duckdb binary).
#include "duckdb.hpp"

namespace duckdb {

class VexdbLiteExtension : public Extension {
public:
    void Load(ExtensionLoader &loader) override;
    std::string Name() override;
    std::string Version() const override;
};

void LoadInternal(ExtensionLoader &loader);

} // namespace duckdb

#include "vexdb_lite_extension.hpp"

#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#include "vex_optimizer.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"



#ifndef VEXDB_DUCK_GIT_HASH
#define VEXDB_DUCK_GIT_HASH "unknown"
#endif

#ifndef VEXDB_DUCK_BUILD_TIME
#define VEXDB_DUCK_BUILD_TIME "unknown"
#endif

namespace duckdb {

static void VexVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetValue(0, StringVector::AddString(
                           result, "vexdb_lite duck extension " VEXDB_DUCK_GIT_HASH " (" VEXDB_DUCK_BUILD_TIME ")"));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void RegisterIndexTypes(DBConfig &config) {
    IndexType graph_index_type;
    graph_index_type.name = GraphIndex::TYPE_NAME;
    graph_index_type.create_instance = GraphIndex::Create;
    graph_index_type.create_plan = GraphIndex::CreatePlan;
    config.GetIndexTypes().RegisterIndexType(std::move(graph_index_type));
}

// Version locking note: C++ extensions have no stable ABI, so a binary built for
// one DuckDB version must not be loaded into another. DuckDB enforces this natively
// from the extension's footer (the VERSION_FIELD stamped by build_duck.sh strip /
// fix_duckdb_footer.py): the loader rejects a version mismatch with a clear error
// before dlopen — even under allow_unsigned_extensions. No in-init guard is needed.
void LoadInternal(ExtensionLoader &loader) {
    VexFunctions::Register(loader);
    loader.RegisterFunction(ScalarFunction("vexdb_version", {}, LogicalType::VARCHAR, VexVersionFunction));

    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);

    RegisterIndexTypes(config);
    OptimizerExtension::Register(config, VexOptimizerExtension());

    config.AddExtensionOption("vexdb_ef_search", "Search expansion factor for GRAPH_INDEX.",
                              LogicalType::INTEGER, Value::INTEGER(40));
    config.AddExtensionOption("vexdb_brute_force_threshold",
                              "Run exact brute-force search when index row count <= threshold. "
                              "Below ~10k rows ANN offers no perf win but loses recall, so default "
                              "favors precision. Set higher to bypass HNSW after large delete+reinsert "
                              "churn where recall has degraded.",
                              LogicalType::UBIGINT, Value::UBIGINT(10000));
    // off (default): search ignores PQ, runs HNSW on raw vectors.
    // pq_only:       brute-force over PQ codes using a precomputed distance
    //                table. Approximate but fast for small indexes; useful
    //                as a way to verify PQ training is healthy.
    config.AddExtensionOption("vexdb_pq_search_mode",
                              "PQ search mode for GRAPH_INDEX: 'off' or 'pq_only'.",
                              LogicalType::VARCHAR, Value("off"));
    // pq_only-mode refinement: take top k*factor by PQ distance, then re-rank
    // via raw vector L2/cosine. Trades a per-query raw-vec lookup for higher
    // recall. 1.0 disables. Compact-mode indexes ignore (no raw vec to fetch).
    config.AddExtensionOption("vexdb_pq_refine_k_factor",
                              "PQ refine factor: SearchPQ takes top k*factor candidates "
                              "and re-ranks by raw distance. 1.0 = no refine. Range [1.0, 1000.0].",
                              LogicalType::DOUBLE, Value::DOUBLE(1.0));

}

void VexdbLiteExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string VexdbLiteExtension::Name() {
    return "vexdb_lite";
}

std::string VexdbLiteExtension::Version() const {
    return "0.1.0";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(vexdb_lite, loader) {
    duckdb::LoadInternal(loader);
}
}

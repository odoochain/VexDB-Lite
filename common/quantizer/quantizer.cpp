#include <cstring>
#include "quantizer/quantizer.h"
#include "quantizer/pq/pq.h"
#include "quantizer/rabitq/rabitq.h"

void validate_quantizer(const char *value) {
    if (!value) {
        return;
    }
    if (strcmp(value, "none") != 0 &&
        strcmp(value, "pq") != 0 &&
        strcmp(value, "rabitq") != 0 &&
        strcmp(value, "plain") != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("invalid quantizer type \"%s\"", value)));
    }
}

QuantizerType extract_qt(const char *value) {
    if (strcmp(value, "none") == 0 || strcmp(value, "plain") == 0) {
        return QuantizerType::NONE;
    }
    if (strcmp(value, "pq") == 0) {
        return QuantizerType::PQ;
    }
    if (strcmp(value, "rabitq") == 0) {
        return QuantizerType::RABITQ;
    }
    __builtin_unreachable();
}

void PQMetaInfo::init(uint32 dim)
{
    graph_pq = false;
    pq_set_param(dim, m, k);
}

void RaBitQMeta::init(uint32 dim)
{
    enabled = false;
    size_t padded_dim = RABITQ_PADDED_DIM(dim);
    size_t cid_size = sizeof(uint16);
    size_t bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    size_t ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
    quant_size = cid_size + bin_size + ext_size;
    query_rescaling_factor = rabitq::get_const_scaling_factors(dim, 3);
}

void QuantizerMetaInfo::init(QuantizerType qt_type, uint32 dimension)
{
    set_type(qt_type);
    num_new_data = 0;
    centroids_version = 0;
    code_version = 0;
    if (qt_type == QuantizerType::NONE) {
        auto meta = metainfo;
        memset(&meta, 0, sizeof(meta));
    } else if (qt_type == QuantizerType::PQ) {
        PQMetaInfo &pq_metainfo = get_pq_metainfo();
        pq_metainfo.init(dimension);
    } else if (qt_type == QuantizerType::RABITQ) {
        RaBitQMeta &rbq_meta = get_rabitq_meta();
        rbq_meta.init(dimension);
    }
}
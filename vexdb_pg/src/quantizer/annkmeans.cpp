#include "platform/platform_compat.h"
#include "annkmeans.h"
#include "ann_utils.h"
#include "data_type/halfvec.h"

using namespace ann_helper;

void ann_sample_rows(FloatVectorArray samples, Relation heap, Relation index,
    int dimensions, int sample_nums, bool need_norm, DistPrecisionType precision_type)
{
    if (heap == NULL || samples == NULL) return;

    samples->length = 0;
    Snapshot snap = GetActiveSnapshot();
#if PG_VERSION_NUM >= 190000
    TableScanDesc scan = table_beginscan(heap, snap, 0, NULL, 0);
#else
    TableScanDesc scan = table_beginscan(heap, snap, 0, NULL);
#endif
    TupleTableSlot *slot = table_slot_create(heap, NULL);

    IndexInfo *indexInfo = BuildIndexInfo(index);
    EState *estate = CreateExecutorState();
    ExprContext *econtext = GetPerTupleExprContext(estate);
    Datum values[INDEX_MAX_KEYS];
    bool isnull[INDEX_MAX_KEYS];

    vector_preprocess_func norm_func = need_norm
        ? get_vector_preprocess_func(Metric::FAST_COSINE, precision_type, dimensions)
        : NULL;

    int collected = 0;
    while (collected < sample_nums &&
           table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        econtext->ecxt_scantuple = slot;
        FormIndexDatum(indexInfo, slot, estate, values, isnull);
        if (isnull[0]) continue;

        Pointer vec_p = NULL;
        char *v = DatumGetVector(values[0], precision_type, &vec_p);
        char *value = v;

        /* Normalize if needed */
        if (norm_func) {
            char *temp = (char *)alloc_vector(dimensions * VEC_ELEM_SIZE(precision_type));
            norm_func(value, dimensions, temp);
            value = temp;
        }

        /* Convert HALF to float for storage in FloatVectorArray */
        float *half2float = NULL;
        if (precision_type == DistPrecisionType::HALF) {
            half2float = alloc_floatvector(dimensions);
            halfs_to_floats((half *)value, half2float, dimensions);
            if (value != v) {
                free_vector(value);
            }
            value = (char *)half2float;
        }

        std::memcpy(FloatVectorArrayGet(samples, collected), value,
                    sizeof(float) * dimensions);
        collected++;
        samples->length = collected;

        if (half2float) {
            free_vector(half2float);
        } else if (value != v) {
            free_vector(value);
        }
        if (vec_p != DatumGetPointer(values[0])) {
            pfree(vec_p);
        }
        ResetExprContext(econtext);
    }
    FreeExecutorState(estate);
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
}

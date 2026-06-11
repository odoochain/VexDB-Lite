#include "ann_utils.h"
#include "floatvector.h"
#include "data_type/halfvec.h"
#include "data_type/int8vec.h"
#include "platform/platform_compat.h"

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
}

// TypenameGetTypid only searches pg_catalog. Our extension types live in
// the schema where CREATE EXTENSION installed them; resolve "schema.name"
// via search_path then SearchSysCache2 in (TYPENAMENSP).
static Oid lookup_extension_type_oid(const char *name)
{
    Oid nsp_oid = LookupExplicitNamespace("public", /*missing_ok*/ true);
    if (!OidIsValid(nsp_oid)) return InvalidOid;
    return GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
                           PointerGetDatum(name),
                           ObjectIdGetDatum(nsp_oid));
}

Oid get_floatvector_oid(void) { return lookup_extension_type_oid("floatvector"); }
Oid get_halfvector_oid(void)  { return lookup_extension_type_oid("halfvector"); }
Oid get_int8vector_oid(void)  { return lookup_extension_type_oid("int8vector"); }

size_t get_relstats_reltuples(Relation rel)
{
    size_t reltuples_stats = 0;
    if (rel == NULL) {
        return reltuples_stats;
    }

    Oid relid = RelationGetRelid(rel);
    HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (HeapTupleIsValid(tuple)) {
        Form_pg_class pg_class_form = (Form_pg_class)GETSTRUCT(tuple);
        /* PG uses reltuples = -1.0 as "stats not collected"; (size_t)(-1.0f)
         * overflows to UINT64_MAX and bypasses callers' `> 0` guards. */
        float reltuples_raw = pg_class_form->reltuples;
        reltuples_stats = (reltuples_raw > 0) ? (size_t)reltuples_raw : 0;
        ReleaseSysCache(tuple);
    }

    return reltuples_stats;
}

void populate_index_partition_name(Relation index, char *indexName, char *partIndexName)
{
    sprintf(indexName, "%s", RelationGetRelationName(index));
    partIndexName[0] = '\0';
}

Buffer AnnNewBuffer(Relation index, ForkNumber forkNum)
{
    Buffer buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

void AnnCommitBuffer(Buffer buf)
{
    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);
}

void check_ann_attributes(Relation index)
{
    Assert(RelationGetDescr(index)->natts >= 1);
    Oid vector_oids[] = {get_floatvector_oid(), get_halfvector_oid(), get_int8vector_oid()};
    int natts = RelationGetDescr(index)->natts;
    Oid first_attr_oid = TupleDescAttr(RelationGetDescr(index), 0)->atttypid;

    bool is_vector = false;
    for (Oid oid : vector_oids) {
        if (first_attr_oid == oid) { is_vector = true; break; }
    }
    if (!is_vector) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("The first attribute of index must be floatvector, "
                               "halfvector, or int8vector.")));
    }
    for (int i = 1; i < natts; ++i) {
        for (Oid oid : vector_oids) {
            if (TupleDescAttr(RelationGetDescr(index), i)->atttypid == oid) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("only the first column may be a vector type")));
            }
        }
    }
    // Reject duplicate underlying-table attributes — PG does not catch
    // (col, col) in CREATE INDEX since each list entry is a distinct
    // index attribute even when they reference the same heap column.
    for (int i = 0; i < natts; ++i) {
        AttrNumber a = index->rd_index->indkey.values[i];
        for (int j = i + 1; j < natts; ++j) {
            if (a != 0 && a == index->rd_index->indkey.values[j]) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("duplicate column in index")));
            }
        }
    }
}

Buffer AnnLoadBuffer(Relation index, BlockNumber blkNo)
{
    Buffer buf = ReadBuffer(index, blkNo);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    return buf;
}

Buffer AnnLoadBufferExtended(Relation index, ForkNumber forkNum, BlockNumber blkNo)
{
    Buffer buf = ReadBufferExtended(index, forkNum, blkNo, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

bool isHybridIndex(Relation index) { return RelationGetDescr(index)->natts > 1; }

bool AnnNormValue(ann_helper::distance_func procinfo, Datum *value, FloatVector *result)
{
    FloatVector *v = DatumGetFloatVector(*value);
    double norm = procinfo(v->x, v->x, v->dim);
    if (norm <= 0) {
        return false;
    }

    if (result == NULL) {
        result = InitFloatVector(v->dim);
    }

    for (int i = 0; i < v->dim; ++i) {
        result->x[i] = v->x[i] / norm;
    }

    if ((Pointer)v != DatumGetPointer(*value)) {
        pfree(v);
    }

    *value = PointerGetDatum(result);
    return true;
}

char *DatumGetVector(Datum value, DistPrecisionType type, Pointer *vec_out)
{
    char *vector = NULL;
    if (type == DistPrecisionType::FLOAT) {
        FloatVector *tempvec = DatumGetFloatVector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    } else if (type == DistPrecisionType::HALF) {
        HalfVector *tempvec = DatumGetHalfVector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    } else {
        Assert(type == DistPrecisionType::INT8);
        Int8Vector *tempvec = DatumGetInt8Vector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    }
    return vector;
}

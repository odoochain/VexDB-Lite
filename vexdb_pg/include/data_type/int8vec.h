#ifndef INT8VEC_H
#define INT8VEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "fmgr.h"

#define INT8VEC_MAX_DIM 16384

#define INT8VEC_SIZE(_dim)     (offsetof(Int8Vector, x) + sizeof(int8_t) * (_dim))
#define DatumGetInt8Vector(x)  ((Int8Vector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_INT8VEC_P(x) DatumGetInt8Vector(PG_GETARG_DATUM(x))
#define PG_RETURN_INT8VEC_P(x) PG_RETURN_POINTER(x)

struct Int8Vector {
    int32 vl_len_;  /* varlena header (do not touch directly!) */
    int16 dim;      /* number of dimensions */
    int16 unused;   /* reserved for future use, always zero */
    int8_t x[FLEXIBLE_ARRAY_MEMBER];
};

extern Int8Vector *InitInt8Vector(int dim);

extern Datum int8vector_to_int8vector(PG_FUNCTION_ARGS);
extern Datum int8vector_in(PG_FUNCTION_ARGS);
extern Datum int8vector_out(PG_FUNCTION_ARGS);
extern Datum int8vector_typmod_in(PG_FUNCTION_ARGS);
extern Datum int8vector_typmod_out(PG_FUNCTION_ARGS);
extern Datum int8vector_recv(PG_FUNCTION_ARGS);
extern Datum int8vector_send(PG_FUNCTION_ARGS);
extern Datum int8vector_l2_distance(PG_FUNCTION_ARGS);
extern Datum int8vector_l2_squared_distance(PG_FUNCTION_ARGS);
extern Datum int8vector_inner_product(PG_FUNCTION_ARGS);
extern Datum int8vector_negative_inner_product(PG_FUNCTION_ARGS);
extern Datum int8vector_cosine_distance(PG_FUNCTION_ARGS);
extern Datum int8vector_spherical_distance(PG_FUNCTION_ARGS);
extern Datum int8vector_dims(PG_FUNCTION_ARGS);
extern Datum int8vector_l2_norm(PG_FUNCTION_ARGS);
extern Datum int8vector_add(PG_FUNCTION_ARGS);
extern Datum int8vector_sub(PG_FUNCTION_ARGS);
extern Datum int8vector_lt(PG_FUNCTION_ARGS);
extern Datum int8vector_le(PG_FUNCTION_ARGS);
extern Datum int8vector_eq(PG_FUNCTION_ARGS);
extern Datum int8vector_ne(PG_FUNCTION_ARGS);
extern Datum int8vector_ge(PG_FUNCTION_ARGS);
extern Datum int8vector_gt(PG_FUNCTION_ARGS);
extern Datum int8vector_cmp(PG_FUNCTION_ARGS);
extern Datum int8vector_sortsupport(PG_FUNCTION_ARGS);
extern Datum hashint8vector(PG_FUNCTION_ARGS);
extern Datum int8vector_subvector(PG_FUNCTION_ARGS);
extern void int8s_to_floats(int8_t *int8s, float *f, uint32 dim);

#ifdef __cplusplus
}
#endif

#endif /* INT8VEC_H */

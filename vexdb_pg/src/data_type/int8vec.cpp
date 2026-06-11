#include "platform/platform_compat.h"

#include <math.h>
#include "data_type/int8vec.h"
#include "data_type/vec_common.h"
#include "global_instance.h"

extern "C" {
#include "utils/sortsupport.h"
}

#define INT8_L2_SQUARED_DIST g_instance.annvec_cxt.int8_l2_squared_distance
#define INT8_NEGATIVE_INNER_PRODUCT_DIST g_instance.annvec_cxt.int8_negative_inner_product
#define INT8_COSINE_DIST g_instance.annvec_cxt.int8_cosine_distance


Int8Vector *InitInt8Vector(int dim)
{
    int size = INT8VEC_SIZE(dim);
    Int8Vector *result = (Int8Vector *)palloc0(size);
    SET_VARSIZE(result, size);
    result->dim = dim;
    return result;
}

static void CheckDims(Int8Vector * a, Int8Vector * b)
{
    if (a->dim != b->dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("different int8vector dimensions %d and %d", a->dim, b->dim)));
    }
}

static void CheckExpectedDim(int32 typmod, int dim)
{
    if (typmod != -1 && typmod != dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("expected %d dimensions, not %d", typmod, dim)));
    }
}

static void CheckDim(int dim)
{
    if (dim < 1) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("int8vector dimension must be at least 1")));
    }
    if (dim > INT8VEC_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("int8vector gets dimension %d exceeding maximum %d", dim, INT8VEC_MAX_DIM)));
    }
}

static inline void int8_out_of_range(char *num)
{
    ereport(ERROR,
        (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
            errmsg("\"%s\" is out of range for type int8_t", num),
            errdetail("Value must be between %d and %d.", INT8_MIN, INT8_MAX)));
}


static bool is_outof_int8(long val)
{
    return (val < INT8_MIN || val > INT8_MAX);
}

static bool int8vector_isspace(char ch)
    { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f'; }

static Int8Vector* int8vector_input(char* lit, int32 atttypmod)
{
    int8_t x[INT8VEC_MAX_DIM];
    int dim = 0;
    char *pt = lit;
    Int8Vector *result;

    while (int8vector_isspace(*pt)) {
        pt++;
    }

    if (*pt != '[') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type int8vector: \"%s\"", lit),
                 errdetail("int8vector contents must start with \"[\".")));
    }

    pt++;
    while (int8vector_isspace(*pt)) {
        pt++;
    }

    if (*pt == ']') {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("int8vector dimension must be at least 1")));
    }

    for (;;) {
        char *stringEnd;

        if (dim == INT8VEC_MAX_DIM) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("int8vector cannot have more than %d dimensions", INT8VEC_MAX_DIM)));
        }

        while (int8vector_isspace(*pt)) {
            pt++;
        }

        /* Check for empty string like float4in */
        if (*pt == '\0') {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type int8vector: \"%s\"", lit)));
        }

        errno = 0;
        long lval = strtol(pt, &stringEnd, 10);

        if (stringEnd == pt) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type int8vector: \"%s\"", lit),
                     errdetail("Expected an integer.")));
        }

        if (errno == ERANGE || is_outof_int8(lval)) {
            int8_out_of_range(pnstrdup(pt, stringEnd - pt));
        }

        x[dim] = (int8_t)lval;
        dim++;
        pt = stringEnd;

        while (int8vector_isspace(*pt)) {
            pt++;
        }

        if (*pt == ',') {
            pt++;
        } else if (*pt == ']') {
            pt++;
            break;
        } else {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type int8vector: \"%s\"", lit)));
        }
    }

    /* Only whitespace is allowed after the closing brace */
    while (int8vector_isspace(*pt)) {
        pt++;
    }

    if (*pt != '\0') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type int8vector: \"%s\"", lit),
                 errdetail("Junk after closing right brace.")));
    }

    CheckDim(dim);
    CheckExpectedDim(atttypmod, dim);

    result = InitInt8Vector(dim);
    memcpy(result->x, x, dim * sizeof(int8_t));
    return result;
}

extern "C" {
/*
 * Convert textual representation to internal representation
 */
PG_FUNCTION_INFO_V1(int8vector_in);
Datum int8vector_in(PG_FUNCTION_ARGS)
{
    char *lit = PG_GETARG_CSTRING(0);
    int32 typmod = PG_GETARG_INT32(2);
    Int8Vector *result = int8vector_input(lit, typmod);
    PG_RETURN_INT8VEC_P(result);
}

/*
 * Convert internal representation to textual representation
 */
PG_FUNCTION_INFO_V1(int8vector_out);
Datum int8vector_out(PG_FUNCTION_ARGS)
{
    Int8Vector *vector = PG_GETARG_INT8VEC_P(0);
    int dim = vector->dim;
    char *buf;
    char *ptr;

    /*
     * Maximum length per int8_t: "-128" → 4 characters
     * Total: dim * 4 (digits) + (dim - 1) (commas) + 2 ([ and ]) + 1 (\0)
     */
    buf = (char *) palloc(dim * 4 + dim + 2);  // safe upper bound
    ptr = buf;

    *ptr++ = '[';

    for (int i = 0; i < dim; i++) {
        if (i > 0) {
            *ptr++ = ',';
        }

        /* Convert int8_t to string */
        ptr += snprintf(ptr, 6, "%d", (int) vector->x[i]);
    }

    *ptr++ = ']';
    *ptr = '\0';

    PG_FREE_IF_COPY(vector, 0);
    PG_RETURN_CSTRING(buf);
}

/*
 * Convert type modifier
 */
PG_FUNCTION_INFO_V1(int8vector_typmod_in);
Datum int8vector_typmod_in(PG_FUNCTION_ARGS)
{
    ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
    int32 *tl;
    int n;

    tl = ArrayGetIntegerTypmods(ta, &n);

    if (n != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid type modifier")));
    }

    if (*tl < 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("int8vector dimension must be at least 1")));
    }

    if (*tl > INT8VEC_MAX_DIM) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("int8vector cannot have more than %d dimensions", INT8VEC_MAX_DIM)));
    }

    PG_RETURN_INT32(*tl);
}


PG_FUNCTION_INFO_V1(int8vector_typmod_out);
Datum int8vector_typmod_out(PG_FUNCTION_ARGS)
{
    int32 typmod = PG_GETARG_INT32(0);
    if (typmod < 0) {
        PG_RETURN_CSTRING(pstrdup(""));
    }

    /* Convert typmod to string */
    char *result = psprintf("(%d)", typmod);
    PG_RETURN_CSTRING(result);
}

/*
 * Convert external binary representation to internal representation
 */
PG_FUNCTION_INFO_V1(int8vector_recv);
Datum int8vector_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo) PG_GETARG_POINTER(0);
    int32 typmod = PG_GETARG_INT32(2);
    int16 dim = pq_getmsgint(buf, sizeof(int16));
    int16 unused = pq_getmsgint(buf, sizeof(int16));

    CheckDim(dim);
    CheckExpectedDim(typmod, dim);

    if (unused != 0) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("expected unused to be 0, not %d", unused)));
    }

    Int8Vector *result = InitInt8Vector(dim);
    for (int i = 0; i < dim; i++) {
        result->x[i] = (int8_t) pq_getmsgbyte(buf);
    }

    PG_RETURN_INT8VEC_P(result);
}

/*
 * Convert internal representation to the external binary representation
 */
PG_FUNCTION_INFO_V1(int8vector_send);
Datum int8vector_send(PG_FUNCTION_ARGS)
{
    Int8Vector *vec = PG_GETARG_INT8VEC_P(0);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendint(&buf, vec->dim, sizeof(int16));
    pq_sendint(&buf, vec->unused, sizeof(int16));
    for (int i = 0; i < vec->dim; i++) {
        pq_sendbyte(&buf, (unsigned char)vec->x[i]);
    }

    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Convert int8 vector to int8 vector
 * This is needed to check the type modifier
 */
PG_FUNCTION_INFO_V1(int8vector_to_int8vector);
Datum int8vector_to_int8vector(PG_FUNCTION_ARGS)
{
    Int8Vector *vec = PG_GETARG_INT8VEC_P(0);
    int32 typmod = PG_GETARG_INT32(1);
    CheckExpectedDim(typmod, vec->dim);
    PG_RETURN_INT8VEC_P(vec);
}

/*
 * Get the L2 distance between int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_l2_distance);
Datum int8vector_l2_distance(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);

    CheckDims(a, b);

    PG_RETURN_FLOAT8(sqrt((double) INT8_L2_SQUARED_DIST(a->x, b->x, a->dim)));
}

/*
 * Get the L2 squared distance between int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_l2_squared_distance);
Datum int8vector_l2_squared_distance(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);

    CheckDims(a, b);

    PG_RETURN_FLOAT8((double) INT8_L2_SQUARED_DIST(a->x, b->x, a->dim));
}

/*
 * Get the inner product of two int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_inner_product);
Datum int8vector_inner_product(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);

    CheckDims(a, b);

    PG_RETURN_FLOAT8((double) -INT8_NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim));
}

/*
 * Get the negative inner product of two int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_negative_inner_product);
Datum int8vector_negative_inner_product(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);

    CheckDims(a, b);
    double distance = INT8_NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(abs(distance) == 0.0 ? 0 : distance);
}

/*
 * Get the cosine distance between two int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_cosine_distance);
Datum int8vector_cosine_distance(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    CheckDims(a, b);
    double similarity = INT8_COSINE_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(1 + similarity);
}

/*
 * Get the distance for spherical k-means
 * Currently uses angular distance since needs to satisfy triangle inequality
 * Assumes inputs are unit vectors (skips norm)
 */
PG_FUNCTION_INFO_V1(int8vector_spherical_distance);
Datum int8vector_spherical_distance(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);

    CheckDims(a, b);

    double distance = -INT8_NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);

    /* Prevent NaN with acos with loss of precision */
    if (distance > 1) {
        distance = 1;
    } else if (distance < -1) {
        distance = -1;
    }

    PG_RETURN_FLOAT8(acos(distance) / M_PI);
}

/*
 * Get the dimensions of a int8 vector
 */
PG_FUNCTION_INFO_V1(int8vector_dims);
Datum int8vector_dims(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    PG_RETURN_INT32(a->dim);
}

/*
 * Get the L2 norm of a int8 vector
 */
PG_FUNCTION_INFO_V1(int8vector_l2_norm);
Datum int8vector_l2_norm(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    double norm = -INT8_NEGATIVE_INNER_PRODUCT_DIST(a->x, a->x, a->dim);
    PG_RETURN_FLOAT8(sqrt(norm));
}

/*
 * Add int8 vectors
 */

PG_FUNCTION_INFO_V1(int8vector_add);
Datum int8vector_add(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    int8_t *ax = a->x;
    int8_t *bx = b->x;

    CheckDims(a, b);

    Int8Vector *result = InitInt8Vector(a->dim);
    int8_t *rx = result->x;

    for (int i = 0, imax = a->dim; i < imax; i++) {
        long temp = ax[i] + bx[i];
        if (is_outof_int8(temp)) {
            char val_str[32];
            pg_ltoa(temp, val_str);
            int8_out_of_range(val_str);
        }
        rx[i] = (int8_t)temp;
    }

    PG_RETURN_INT8VEC_P(result);
}

/*
 * Subtract int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_sub);
Datum int8vector_sub(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    int8_t *ax = a->x;
    int8_t *bx = b->x;

    CheckDims(a, b);

    Int8Vector *result = InitInt8Vector(a->dim);
    int8_t *rx = result->x;

    /* Auto-vectorized */
    for (int i = 0, imax = a->dim; i < imax; i++) {
        long temp = ax[i] - bx[i];
        if (is_outof_int8(temp)) {
            char val_str[32];
            pg_ltoa(temp, val_str);
            int8_out_of_range(val_str);
        }
        rx[i] = (int8_t)temp;
    }

    PG_RETURN_INT8VEC_P(result);
}

/*
 * Get a subvector
 */
PG_FUNCTION_INFO_V1(int8vector_subvector);
Datum int8vector_subvector(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    int32 start = PG_GETARG_INT32(1);
    int32 count = PG_GETARG_INT32(2);
    int32 end;
    Int8Vector *result;
    int32 dim;

    if (count < 1) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("int8vector dimension must be at least 1")));
    }

    /*
     * Check if (start + count > a->dim), avoiding integer overflow. a->dim
     * and count are both positive, so a->dim - count won't overflow.
     */
    if (start > a->dim - count) {
        end = a->dim + 1;
    } else {
        end = start + count;
    }

    /* Indexing starts at 1, like substring */
    if (start < 1) {
        start = 1;
    } else if (start > a->dim) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("int8vector dimension must be at least 1")));
    }

    dim = end - start;
    CheckDim(dim);
    result = InitInt8Vector(dim);
    memcpy(result->x, a->x + start - 1, dim * sizeof(int8_t));
    PG_RETURN_POINTER(result);
}

/*
 * Internal helper to compare int8 vectors
 */
static int int8vector_cmp_internal(Int8Vector * a, Int8Vector * b)
{
    int dim = Min(a->dim, b->dim);

    /* Check values before dimensions to be consistent with Postgres arrays */
    for (int i = 0; i < dim; i++) {
        if (a->x[i] < b->x[i]) {
            return -1;
        }

        if (a->x[i] > b->x[i]) {
            return 1;
        }
    }

    if (a->dim < b->dim) {
        return -1;
    }

    if (a->dim > b->dim) {
        return 1;
    }

    return 0;
}

/*
 * Less than
 */
PG_FUNCTION_INFO_V1(int8vector_lt);
Datum int8vector_lt(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_BOOL(int8vector_cmp_internal(a, b) < 0);
}

/*
 * Less than or equal
 */
PG_FUNCTION_INFO_V1(int8vector_le);
Datum int8vector_le(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_BOOL(int8vector_cmp_internal(a, b) <= 0);
}

/*
 * Equal
 */
PG_FUNCTION_INFO_V1(int8vector_eq);
Datum int8vector_eq(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_BOOL(int8vector_cmp_internal(a, b) == 0);
}

/*
 * Not equal
 */
PG_FUNCTION_INFO_V1(int8vector_ne);
Datum int8vector_ne(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_BOOL(int8vector_cmp_internal(a, b) != 0);
}

/*
 * Greater than or equal
 */
PG_FUNCTION_INFO_V1(int8vector_ge);
Datum int8vector_ge(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_BOOL(int8vector_cmp_internal(a, b) >= 0);
}

/*
 * Greater than
 */
PG_FUNCTION_INFO_V1(int8vector_gt);
Datum int8vector_gt(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_BOOL(int8vector_cmp_internal(a, b) > 0);
}

/*
 * Compare int8 vectors
 */
PG_FUNCTION_INFO_V1(int8vector_cmp);
Datum int8vector_cmp(PG_FUNCTION_ARGS)
{
    Int8Vector *a = PG_GETARG_INT8VEC_P(0);
    Int8Vector *b = PG_GETARG_INT8VEC_P(1);
    PG_RETURN_INT32(int8vector_cmp_internal(a, b));
}


static int int8vector_fastcmp(Datum x, Datum y, SortSupport)
{
    Int8Vector *a = (Int8Vector *)DatumGetInt8Vector(x);
    Int8Vector *b = (Int8Vector *)DatumGetInt8Vector(y);
    return int8vector_cmp_internal(a, b);
}

PG_FUNCTION_INFO_V1(int8vector_sortsupport);
Datum int8vector_sortsupport(PG_FUNCTION_ARGS)
{
    SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);
    ssup->comparator = int8vector_fastcmp;
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(hashint8vector);
Datum hashint8vector(PG_FUNCTION_ARGS)
{
    Int8Vector *a = (Int8Vector *)PG_GETARG_INT8VEC_P(0);
    return hash_any((unsigned char *)a->x, sizeof(int8_t) * a->dim);
}

} /* extern "C" */
void int8s_to_floats(int8_t *int8s, float *f, uint32 dim)
{
    for (uint32 i = 0; i < dim; ++i) {
        f[i] = (float)int8s[i];
    }
}

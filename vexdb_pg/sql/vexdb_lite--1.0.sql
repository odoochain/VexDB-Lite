-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION vexdb_lite" to load this file. \quit

-- floatvector type

CREATE TYPE floatvector;

CREATE FUNCTION floatvector_in(cstring, oid, integer) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_out(floatvector) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_typmod_in(cstring[]) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_recv(internal, oid, integer) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_send(floatvector) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE floatvector (
    INPUT     = floatvector_in,
    OUTPUT    = floatvector_out,
    TYPMOD_IN = floatvector_typmod_in,
    RECEIVE   = floatvector_recv,
    SEND      = floatvector_send,
    STORAGE   = external
);

-- floatvector distance functions

CREATE FUNCTION l2_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_l2_squared_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION inner_product(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_negative_inner_product(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cosine_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_spherical_distance(floatvector, floatvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector utility functions

CREATE FUNCTION vector_dims(floatvector) RETURNS integer
    AS 'MODULE_PATHNAME', 'floatvector_dims' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vector_norm(floatvector) RETURNS float8
    AS 'MODULE_PATHNAME', 'floatvector_norm' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION l2_normalize(floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector private functions

CREATE FUNCTION floatvector_add(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_sub(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_lt(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_le(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_eq(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_ne(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_ge(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_gt(floatvector, floatvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_cmp(floatvector, floatvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hashfloatvector(floatvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector cast functions

CREATE FUNCTION floatvector(floatvector, integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'floatvector_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(real[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(double precision[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'array_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(integer[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'array_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_floatvector(numeric[], integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'array_to_floatvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_to_float4(floatvector, integer, boolean) RETURNS real[]
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- floatvector casts

CREATE CAST (floatvector AS floatvector)
    WITH FUNCTION floatvector(floatvector, integer, boolean) AS IMPLICIT;

CREATE CAST (floatvector AS real[])
    WITH FUNCTION floatvector_to_float4(floatvector, integer, boolean) AS IMPLICIT;

-- Accept all four common numeric array element types; the underlying C
-- impl already handles INT4 / FLOAT4 / FLOAT8 / NUMERIC element OIDs.
-- Without these, `INSERT … VALUES (array[1.2, 2.2, 3.3])` fails because
-- the array literal infers `numeric[]`, for which no cast existed.
CREATE CAST (real[] AS floatvector)
    WITH FUNCTION array_to_floatvector(real[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (double precision[] AS floatvector)
    WITH FUNCTION array_to_floatvector(double precision[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (integer[] AS floatvector)
    WITH FUNCTION array_to_floatvector(integer[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (numeric[] AS floatvector)
    WITH FUNCTION array_to_floatvector(numeric[], integer, boolean) AS ASSIGNMENT;

-- floatvector operators

CREATE OPERATOR <-> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_negative_inner_product,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <=> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = cosine_distance,
    COMMUTATOR = '<=>'
);

-- Duck-side parity: <~> is negative inner product, matching DuckDB's <~> /
-- list_negative_inner_product. (Same procedure as <#>; <#> is the pgvector-style
-- alias, <~> keeps the operator token usable on DuckDB where # is a comment char.)
CREATE OPERATOR <~> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_negative_inner_product,
    COMMUTATOR = '<~>'
);

-- Duck-side parity: vector_add/vector_sub alias floatvector_add/sub.
CREATE FUNCTION vector_add(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'floatvector_add' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION vector_sub(floatvector, floatvector) RETURNS floatvector
    AS 'MODULE_PATHNAME', 'floatvector_sub' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_add,
    COMMUTATOR = +
);

CREATE OPERATOR - (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_sub
);

CREATE OPERATOR < (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_lt,
    COMMUTATOR = >, NEGATOR = >=,
    RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_le,
    COMMUTATOR = >=, NEGATOR = >,
    RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);

CREATE OPERATOR = (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_eq,
    COMMUTATOR = =, NEGATOR = <>,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR <> (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_ne,
    COMMUTATOR = <>, NEGATOR = =,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_ge,
    COMMUTATOR = <=, NEGATOR = <,
    RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = floatvector, RIGHTARG = floatvector, PROCEDURE = floatvector_gt,
    COMMUTATOR = <, NEGATOR = <=,
    RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);

-- floatvector opclasses

CREATE OPERATOR CLASS floatvector_ops
    DEFAULT FOR TYPE floatvector USING btree AS
    OPERATOR 1 <,
    OPERATOR 2 <=,
    OPERATOR 3 =,
    OPERATOR 4 >=,
    OPERATOR 5 >,
    FUNCTION 1 floatvector_cmp(floatvector, floatvector);

CREATE OPERATOR CLASS hash_floatvector_ops
    FOR TYPE floatvector USING hash AS
    OPERATOR 1 =,
    FUNCTION 1 hashfloatvector(floatvector);


-- access method

CREATE FUNCTION vexdb_graph_amhandler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'graph_index_amhandler' LANGUAGE C;

CREATE ACCESS METHOD vexdb_graph
    TYPE INDEX
    HANDLER vexdb_graph_amhandler;

COMMENT ON ACCESS METHOD vexdb_graph IS 'HNSW graph index access method for vector similarity search';

-- vexdb_graph opclasses for floatvector

CREATE OPERATOR CLASS floatvector_l2_ops
    FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <-> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_l2_squared_distance(floatvector, floatvector);

CREATE OPERATOR CLASS floatvector_ip_ops
    FOR TYPE floatvector USING vexdb_graph AS
    -- <#> (strategy 1): pgvector 兼容写法。
    -- <~> (strategy 2): 跨引擎统一写法，与 DuckDB 的 <~> 负内积一致。
    -- 两者同为负内积；metric 来自索引元数据(FUNCTION 1)，与查询用哪个算符无关。
    OPERATOR 1 <#> (floatvector, floatvector) FOR ORDER BY float_ops,
    OPERATOR 2 <~> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_negative_inner_product(floatvector, floatvector);

CREATE OPERATOR CLASS floatvector_cosine_ops
    DEFAULT FOR TYPE floatvector USING vexdb_graph AS
    OPERATOR 1 <=> (floatvector, floatvector) FOR ORDER BY float_ops,
    FUNCTION 1 floatvector_negative_inner_product(floatvector, floatvector),
    FUNCTION 2 vector_norm(floatvector);


-- inspect functions

CREATE FUNCTION index_inspect(regclass)
    RETURNS TABLE(attribute text, content text)
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

COMMENT ON FUNCTION index_inspect(regclass) IS
    'Returns statistics about a vexdb_graph index';

CREATE FUNCTION vectorbuffer_inspect()
    RETURNS TABLE(used_space text, elem_size text, elem_nums int8,
                  hit int8, miss int8, eviction_rate float8)
    AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON FUNCTION vectorbuffer_inspect() IS
    'Returns statistics about the vector buffer cache';

-- vexdb_index_info: SRF that lists all vexdb_graph indexes with metadata.
-- Schema mirrors duckdb/vexdb_duckdb/functions/index_info_function.cpp.
CREATE FUNCTION vexdb_index_info()
    RETURNS TABLE(
        index_name        text,
        indexname         text,
        index_type        text,
        table_name        text,
        partition_count   int4,
        node_count        int8,
        max_level         int4,
        dimension         int4,
        row_id_map_size   int8,
        m                 int4,
        ef_construction   int4,
        metric            text,
        use_pq            bool,
        pq_m              int4,
        memory_bytes      int8,
        pq_codes_bytes    int8,
        pq_codebook_bytes int8,
        memory_mode       text)
    AS 'MODULE_PATHNAME' LANGUAGE C;

COMMENT ON FUNCTION vexdb_index_info() IS
    'Lists all vexdb_graph indexes with metadata (mirrors duck-side schema)';

-- ========== halfvec type ==========

CREATE TYPE halfvector;

CREATE FUNCTION halfvector_in(cstring, oid, integer) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_out(halfvector) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_typmod_in(cstring[]) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_typmod_out(integer) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_recv(internal, oid, integer) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_send(halfvector) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE halfvector (
    INPUT     = halfvector_in,
    OUTPUT    = halfvector_out,
    TYPMOD_IN = halfvector_typmod_in,
    TYPMOD_OUT = halfvector_typmod_out,
    RECEIVE   = halfvector_recv,
    SEND      = halfvector_send,
    STORAGE   = external
);

CREATE FUNCTION l2_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME', 'halfvector_l2_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_l2_squared_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION inner_product(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME', 'halfvector_inner_product' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_negative_inner_product(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cosine_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME', 'halfvector_cosine_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_spherical_distance(halfvector, halfvector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_dims(halfvector) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_norm(halfvector) RETURNS float8
    AS 'MODULE_PATHNAME', 'halfvector_l2_norm' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION l2_normalize(halfvector) RETURNS halfvector
    AS 'MODULE_PATHNAME', 'halfvector_l2_normalize' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_add(halfvector, halfvector) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_sub(halfvector, halfvector) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_lt(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_le(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_eq(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_ne(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_ge(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_gt(halfvector, halfvector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_cmp(halfvector, halfvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hashhalfvector(halfvector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector(halfvector, integer, boolean) RETURNS halfvector
    AS 'MODULE_PATHNAME', 'halfvector_to_halfvector' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION array_to_halfvector(real[], integer, boolean) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_to_float4(halfvector, integer, boolean) RETURNS real[]
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION floatvector_to_halfvector(floatvector, integer, boolean) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_to_floatvector(halfvector, integer, boolean) RETURNS floatvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (halfvector AS halfvector)
    WITH FUNCTION halfvector(halfvector, integer, boolean) AS IMPLICIT;

CREATE CAST (halfvector AS real[])
    WITH FUNCTION halfvector_to_float4(halfvector, integer, boolean) AS IMPLICIT;

CREATE CAST (real[] AS halfvector)
    WITH FUNCTION array_to_halfvector(real[], integer, boolean) AS ASSIGNMENT;

CREATE CAST (floatvector AS halfvector)
    WITH FUNCTION floatvector_to_halfvector(floatvector, integer, boolean) AS ASSIGNMENT;

CREATE CAST (halfvector AS floatvector)
    WITH FUNCTION halfvector_to_floatvector(halfvector, integer, boolean) AS ASSIGNMENT;

CREATE OPERATOR <-> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_negative_inner_product,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <=> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = cosine_distance,
    COMMUTATOR = '<=>'
);

CREATE OPERATOR <~> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_negative_inner_product,
    COMMUTATOR = '<~>'
);

CREATE OPERATOR + (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_add,
    COMMUTATOR = +
);

CREATE OPERATOR - (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_sub
);

CREATE OPERATOR < (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_lt,
    COMMUTATOR = >, NEGATOR = >=,
    RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_le,
    COMMUTATOR = >=, NEGATOR = >,
    RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);

CREATE OPERATOR = (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_eq,
    COMMUTATOR = =, NEGATOR = <>,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR <> (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_ne,
    COMMUTATOR = <>, NEGATOR = =,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_ge,
    COMMUTATOR = <=, NEGATOR = <,
    RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = halfvector, RIGHTARG = halfvector, PROCEDURE = halfvector_gt,
    COMMUTATOR = <, NEGATOR = <=,
    RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);

CREATE OPERATOR CLASS halfvector_ops
    DEFAULT FOR TYPE halfvector USING btree AS
    OPERATOR 1 <,
    OPERATOR 2 <=,
    OPERATOR 3 =,
    OPERATOR 4 >=,
    OPERATOR 5 >,
    FUNCTION 1 halfvector_cmp(halfvector, halfvector);

CREATE OPERATOR CLASS hash_halfvector_ops
    FOR TYPE halfvector USING hash AS
    OPERATOR 1 =,
    FUNCTION 1 hashhalfvector(halfvector);

CREATE OPERATOR CLASS halfvector_l2_ops
    FOR TYPE halfvector USING vexdb_graph AS
    OPERATOR 1 <-> (halfvector, halfvector) FOR ORDER BY float_ops,
    FUNCTION 1 halfvector_l2_squared_distance(halfvector, halfvector);

CREATE OPERATOR CLASS halfvector_ip_ops
    FOR TYPE halfvector USING vexdb_graph AS
    -- <#> (strategy 1): pgvector 兼容写法。
    -- <~> (strategy 2): 跨引擎统一写法，与 DuckDB 的 <~> 负内积一致。
    -- 两者同为负内积；metric 来自索引元数据(FUNCTION 1)，与查询用哪个算符无关。
    OPERATOR 1 <#> (halfvector, halfvector) FOR ORDER BY float_ops,
    OPERATOR 2 <~> (halfvector, halfvector) FOR ORDER BY float_ops,
    FUNCTION 1 halfvector_negative_inner_product(halfvector, halfvector);

CREATE OPERATOR CLASS halfvector_cosine_ops
    FOR TYPE halfvector USING vexdb_graph AS
    OPERATOR 1 <=> (halfvector, halfvector) FOR ORDER BY float_ops,
    FUNCTION 1 halfvector_negative_inner_product(halfvector, halfvector),
    FUNCTION 2 halfvector_norm(halfvector);

CREATE FUNCTION halfvector_sortsupport(internal) RETURNS internal
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION halfvector_accum(internal, halfvector) RETURNS internal
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION halfvector_avg(internal) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION halfvector_subvector(halfvector, int4, int4) RETURNS halfvector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ========== int8vec type ==========

CREATE TYPE int8vector;

CREATE FUNCTION int8vector_in(cstring, oid, integer) RETURNS int8vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_out(int8vector) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_typmod_in(cstring[]) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_typmod_out(integer) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_recv(internal, oid, integer) RETURNS int8vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_send(int8vector) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE int8vector (
    INPUT     = int8vector_in,
    OUTPUT    = int8vector_out,
    TYPMOD_IN = int8vector_typmod_in,
    TYPMOD_OUT = int8vector_typmod_out,
    RECEIVE   = int8vector_recv,
    SEND      = int8vector_send,
    STORAGE   = external
);

CREATE FUNCTION l2_distance(int8vector, int8vector) RETURNS float8
    AS 'MODULE_PATHNAME', 'int8vector_l2_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_l2_squared_distance(int8vector, int8vector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION inner_product(int8vector, int8vector) RETURNS float8
    AS 'MODULE_PATHNAME', 'int8vector_inner_product' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_negative_inner_product(int8vector, int8vector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION cosine_distance(int8vector, int8vector) RETURNS float8
    AS 'MODULE_PATHNAME', 'int8vector_cosine_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_spherical_distance(int8vector, int8vector) RETURNS float8
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_dims(int8vector) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_norm(int8vector) RETURNS float8
    AS 'MODULE_PATHNAME', 'int8vector_l2_norm' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_add(int8vector, int8vector) RETURNS int8vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_sub(int8vector, int8vector) RETURNS int8vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_lt(int8vector, int8vector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_le(int8vector, int8vector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_eq(int8vector, int8vector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_ne(int8vector, int8vector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_ge(int8vector, int8vector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_gt(int8vector, int8vector) RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_cmp(int8vector, int8vector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hashint8vector(int8vector) RETURNS int4
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_negative_inner_product,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <=> (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = cosine_distance,
    COMMUTATOR = '<=>'
);

CREATE OPERATOR <~> (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_negative_inner_product,
    COMMUTATOR = '<~>'
);

CREATE OPERATOR + (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_add,
    COMMUTATOR = +
);

CREATE OPERATOR - (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_sub
);

CREATE OPERATOR < (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_lt,
    COMMUTATOR = >, NEGATOR = >=,
    RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_le,
    COMMUTATOR = >=, NEGATOR = >,
    RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);

CREATE OPERATOR = (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_eq,
    COMMUTATOR = =, NEGATOR = <>,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR <> (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_ne,
    COMMUTATOR = <>, NEGATOR = =,
    RESTRICT = eqsel, JOIN = eqjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_ge,
    COMMUTATOR = <=, NEGATOR = <,
    RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = int8vector, RIGHTARG = int8vector, PROCEDURE = int8vector_gt,
    COMMUTATOR = <, NEGATOR = <=,
    RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);

CREATE OPERATOR CLASS int8vector_ops
    DEFAULT FOR TYPE int8vector USING btree AS
    OPERATOR 1 <,
    OPERATOR 2 <=,
    OPERATOR 3 =,
    OPERATOR 4 >=,
    OPERATOR 5 >,
    FUNCTION 1 int8vector_cmp(int8vector, int8vector);

CREATE OPERATOR CLASS hash_int8vector_ops
    FOR TYPE int8vector USING hash AS
    OPERATOR 1 =,
    FUNCTION 1 hashint8vector(int8vector);

CREATE OPERATOR CLASS int8vector_l2_ops
    FOR TYPE int8vector USING vexdb_graph AS
    OPERATOR 1 <-> (int8vector, int8vector) FOR ORDER BY float_ops,
    FUNCTION 1 int8vector_l2_squared_distance(int8vector, int8vector);

CREATE OPERATOR CLASS int8vector_ip_ops
    FOR TYPE int8vector USING vexdb_graph AS
    -- <#> (strategy 1): pgvector 兼容写法。
    -- <~> (strategy 2): 跨引擎统一写法，与 DuckDB 的 <~> 负内积一致。
    -- 两者同为负内积；metric 来自索引元数据(FUNCTION 1)，与查询用哪个算符无关。
    OPERATOR 1 <#> (int8vector, int8vector) FOR ORDER BY float_ops,
    OPERATOR 2 <~> (int8vector, int8vector) FOR ORDER BY float_ops,
    FUNCTION 1 int8vector_negative_inner_product(int8vector, int8vector);

CREATE OPERATOR CLASS int8vector_cosine_ops
    FOR TYPE int8vector USING vexdb_graph AS
    OPERATOR 1 <=> (int8vector, int8vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(int8vector, int8vector);

CREATE FUNCTION int8vector_to_int8vector(int8vector, integer, boolean) RETURNS int8vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_sortsupport(internal) RETURNS internal
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8vector_subvector(int8vector, int4, int4) RETURNS int8vector
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

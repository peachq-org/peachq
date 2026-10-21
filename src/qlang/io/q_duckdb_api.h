/* q_duckdb_api — private, transcribed subset of DuckDB's v2 C API.
 *
 * Transcribed from the MIT-licensed `duckdb_v2.h` of DuckDB v2.0.0 (github.com/duckdb/duckdb); the GA header is
 * byte-identical to the alpha48841 one it was first taken from.
 * Deliberately NOT #include'd: the bridge reaches DuckDB ONLY via dlopen + a dlsym'd fn-pointer table
 * (docs/duckdb-api.md, Packaging), so a plain `make` compiles with no vendor tree present.  The v2 surface is the
 * whole table — a library without it is refused by the loader (docs/duckdb-architecture.md). */
#ifndef Q_DUCKDB_API_H
#define Q_DUCKDB_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint64_t duck_idx_t;

/* DUCKDB_V2_ERROR: 0 is success; every other code comes with an error-info handle carrying DuckDB's text */
typedef int32_t duck_state;
#define QDuckSuccess 0
/* ERROR_INPUT_METADATA_DEFERRED: a statement that expands into a group knows its metadata only once it steps */
#define QDuckMetadataDeferred 2005

/* DUCKDB_V2_RESULT_TYPE: a query returned rows, anything else counted or did nothing. */
typedef enum { QDuckReturnQuery = 0, QDuckReturnChangedRows = 1, QDuckReturnNothing = 2 } duck_return_t;

/* DUCKDB_V2_STATEMENT_TYPE: the one value the bridge reads — every other statement kind may change the catalog */
#define QDuckStmtSelect 1

/* DUCKDB_V2_LOGICAL_TYPE_ID — the ids the bridge dispatches on (values are ABI). */
enum {
    QDUCK_TYPE_SQLNULL      = 1,
    QDUCK_TYPE_BOOLEAN      = 10,
    QDUCK_TYPE_TINYINT      = 11,
    QDUCK_TYPE_SMALLINT     = 12,
    QDUCK_TYPE_INTEGER      = 13,
    QDUCK_TYPE_BIGINT       = 14,
    QDUCK_TYPE_DATE         = 15,
    QDUCK_TYPE_TIME         = 16,   /* int64 µs since midnight */
    QDUCK_TYPE_TIMESTAMP_S  = 17,   /* int64 s since 1970 */
    QDUCK_TYPE_TIMESTAMP_MS = 18,   /* int64 ms since 1970 */
    QDUCK_TYPE_TIMESTAMP    = 19,   /* int64 µs since 1970 */
    QDUCK_TYPE_TIMESTAMP_NS = 20,   /* int64 ns since 1970 — q timestamp's pair */
    QDUCK_TYPE_DECIMAL      = 21,
    QDUCK_TYPE_FLOAT        = 22,
    QDUCK_TYPE_DOUBLE       = 23,
    QDUCK_TYPE_VARCHAR      = 25,
    QDUCK_TYPE_BLOB         = 26,
    QDUCK_TYPE_INTERVAL     = 27,   /* {months; days; int64 µs} — the q durations' carrier (ADR 19) */
    QDUCK_TYPE_UTINYINT     = 28,
    QDUCK_TYPE_USMALLINT    = 29,
    QDUCK_TYPE_UINTEGER     = 30,
    QDUCK_TYPE_UBIGINT      = 31,
    QDUCK_TYPE_TIMESTAMP_TZ = 32,   /* int64 µs since 1970, UTC — the zone is the session's, never stored */
    QDUCK_TYPE_TIMESTAMP_TZ_NS = 33,
    QDUCK_TYPE_TIME_TZ      = 34,   /* uint64 bits: µs << 24, then 57599 - the offset's seconds (ADR 21) */
    QDUCK_TYPE_TIME_NS      = 35,   /* int64 ns since midnight, never past the 24th hour — q timespan's pair */
    QDUCK_TYPE_BIT          = 36,   /* string_t: one padding-count byte, then the bits MSB-first */
    QDUCK_TYPE_BIGNUM       = 39,   /* a 3-byte header, then big-endian magnitude bytes (ADR 21) */
    QDUCK_TYPE_UHUGEINT     = 49,
    QDUCK_TYPE_HUGEINT      = 50,
    QDUCK_TYPE_UUID         = 54,
    QDUCK_TYPE_GEOMETRY     = 60,
    QDUCK_TYPE_STRUCT       = 100,
    QDUCK_TYPE_LIST         = 101,
    QDUCK_TYPE_MAP          = 102,
    QDUCK_TYPE_ENUM         = 104,
    QDUCK_TYPE_UNION        = 107,
    QDUCK_TYPE_ARRAY        = 108,
    QDUCK_TYPE_VARIANT      = 109,
    QDUCK_TYPE_TUPLE        = 110,
};
typedef int32_t duck_type;

/* Every DuckDB type id spelled as DuckDB itself declares it (`typeof`/DESCRIBE): the base word of the envelope's
 * `dtype` and of the message channel; dispatch stays on the enum.  "?" for an id this build does not know. */
static inline const char* q_duckdb_type_name(duck_type t) {
    switch (t) {
        case 0:                         return "INVALID";
        case QDUCK_TYPE_SQLNULL:        return "\"NULL\"";
        case 3:                         return "ANY";
        case QDUCK_TYPE_BOOLEAN:        return "BOOLEAN";
        case QDUCK_TYPE_TINYINT:        return "TINYINT";
        case QDUCK_TYPE_SMALLINT:       return "SMALLINT";
        case QDUCK_TYPE_INTEGER:        return "INTEGER";
        case QDUCK_TYPE_BIGINT:         return "BIGINT";
        case QDUCK_TYPE_DATE:           return "DATE";
        case QDUCK_TYPE_TIME:           return "TIME";
        case QDUCK_TYPE_TIMESTAMP_S:    return "TIMESTAMP_S";
        case QDUCK_TYPE_TIMESTAMP_MS:   return "TIMESTAMP_MS";
        case QDUCK_TYPE_TIMESTAMP:      return "TIMESTAMP";
        case QDUCK_TYPE_TIMESTAMP_NS:   return "TIMESTAMP_NS";
        case QDUCK_TYPE_DECIMAL:        return "DECIMAL";
        case QDUCK_TYPE_FLOAT:          return "FLOAT";
        case QDUCK_TYPE_DOUBLE:         return "DOUBLE";
        case QDUCK_TYPE_VARCHAR:        return "VARCHAR";
        case QDUCK_TYPE_BLOB:           return "BLOB";
        case QDUCK_TYPE_INTERVAL:       return "INTERVAL";
        case QDUCK_TYPE_UTINYINT:       return "UTINYINT";
        case QDUCK_TYPE_USMALLINT:      return "USMALLINT";
        case QDUCK_TYPE_UINTEGER:       return "UINTEGER";
        case QDUCK_TYPE_UBIGINT:        return "UBIGINT";
        case QDUCK_TYPE_TIMESTAMP_TZ:   return "TIMESTAMP WITH TIME ZONE";
        case QDUCK_TYPE_TIMESTAMP_TZ_NS: return "TIMESTAMPTZ_NS";
        case QDUCK_TYPE_TIME_TZ:        return "TIME WITH TIME ZONE";
        case QDUCK_TYPE_TIME_NS:        return "TIME_NS";
        case QDUCK_TYPE_BIT:            return "BIT";
        case QDUCK_TYPE_BIGNUM:         return "BIGNUM";
        case QDUCK_TYPE_UHUGEINT:       return "UHUGEINT";
        case QDUCK_TYPE_HUGEINT:        return "HUGEINT";
        case QDUCK_TYPE_UUID:           return "UUID";
        case QDUCK_TYPE_GEOMETRY:       return "GEOMETRY";
        case QDUCK_TYPE_STRUCT:         return "STRUCT";
        case QDUCK_TYPE_LIST:           return "LIST";
        case QDUCK_TYPE_MAP:            return "MAP";
        case QDUCK_TYPE_ENUM:           return "ENUM";
        case QDUCK_TYPE_UNION:          return "UNION";
        case QDUCK_TYPE_ARRAY:          return "ARRAY";
        case QDUCK_TYPE_VARIANT:        return "VARIANT";
        case QDUCK_TYPE_TUPLE:          return "TUPLE";
        default:                        return "?";
    }
}

/* the owned and borrowed handles, all one pointer wide */
typedef struct duck_env_o*          duck_env;
typedef struct duck_instance_o*     duck_instance;
typedef struct duck_connection_o*   duck_connection;
typedef struct duck_factory_o*      duck_factory;        /* borrowed from a connection, lives as long as it */
typedef struct duck_err_o*          duck_err;            /* error info: text borrowed until destroyed */
typedef struct duck_logical_o*      duck_logical_type;
typedef struct duck_value_o*        duck_value;
typedef struct duck_cursor_o*       duck_cursor;         /* the streaming result (duckdb_v2_result) */
typedef struct duck_schema_o*       duck_schema;
typedef struct duck_data_chunk_o*   duck_data_chunk;
typedef struct duck_vector_o*       duck_vector;
typedef struct duck_arena_o*        duck_arena;
typedef struct duck_stmt_iter_o*    duck_stmt_iter;
typedef struct duck_stmt_o*         duck_stmt;
typedef struct duck_collection_o*   duck_collection;
typedef struct duck_append_state_o* duck_append_state;
typedef struct duck_rscan_o*        duck_rscan;
typedef struct duck_rscan_info_o*   duck_rscan_info;
typedef struct duck_qname_o*        duck_qname;
typedef struct duck_context_o*      duck_context;

/* duckdb_v2_str: a borrowed, length-delimited view, never assumed NUL-terminated; identifiers share the layout */
typedef struct { const char* ptr; duck_idx_t len; } duck_str;

/* duckdb_v2_vector_view: a flat vector's elements, validity (NULL = all valid) and, for a dictionary vector, the
 * selection; the bridge flattens before viewing, so `sel` is always NULL here */
typedef struct { const void* data; const uint64_t* validity; const uint32_t* sel; duck_idx_t count; } duck_view;

/* duckdb_v2_opaque: user data with an optional destructor and comparator */
typedef struct { void* ptr; void (*destroy)(void*); bool (*equals)(void*, void*); } duck_opaque;

typedef struct { uint64_t lower; int64_t  upper; } duck_hugeint;   /* HUGEINT, and UUID storage */
typedef struct { uint64_t lower; uint64_t upper; } duck_uhugeint;

/* One INTERVAL cell — 16 bytes.  Months and days are units of no fixed length; only micros is a duration. */
typedef struct { int32_t months; int32_t days; int64_t micros; } duck_interval;

/* One LIST cell: a {start,count} window into the vector's flat child. */
typedef struct { uint64_t offset; uint64_t length; } duck_list_entry;

/* The 16-byte inline-or-pointer cell of VARCHAR/BLOB/BIT/BIGNUM vectors (duckdb_v2_bytes). */
typedef struct {
    union {
        struct { uint32_t length; char prefix[4]; char* ptr; } pointer;
        struct { uint32_t length; char inlined[12]; } inlined;
    } value;
} duck_string_t;

#define QDUCK_STRING_INLINE_MAX 12
static inline const char* q_duckdb_string_data(const duck_string_t* s) {
    return s->value.inlined.length <= QDUCK_STRING_INLINE_MAX
               ? s->value.inlined.inlined
               : s->value.pointer.ptr;
}
static inline uint32_t q_duckdb_string_len(const duck_string_t* s) {
    return s->value.inlined.length;
}

/* Validity bitmask (documented layout: 64 rows per word, bit set = valid). */
static inline bool q_duckdb_validity_ok(const uint64_t* validity, duck_idx_t row) {
    return validity == NULL || (validity[row >> 6] & (1ULL << (row & 63))) != 0;
}

/* The bridge's MATERIALISED result: the schema and every chunk, fetched whole before the cursor is destroyed, so a
 * connection never holds a live result between calls (one is allowed per connection) and a statement's failure is
 * known when it runs.  ncols is the schema's count; names are NUL-terminated copies; the column types are BORROWED
 * from the schema (never destroyed by a reader). */
typedef struct {
    duck_schema      schema;
    duck_data_chunk* chunks;
    duck_idx_t       nchunks;
    duck_idx_t       ncols;
    char**           names;
    duck_return_t    rtype;
} duck_result;

typedef void (*duck_rscan_fn)(duck_rscan_info info, duck_context ctx, duck_err* err);

typedef struct {
    duck_state (*environment_create)(duck_env* out, duck_err* err);
    duck_state (*environment_destroy)(duck_env* env);
    /* startup options are (name, SET-text) pairs; a repeated name takes its last value */
    duck_state (*instance_create_with_options)(duck_env env, const duck_str* names, const duck_str* values,
                                               duck_idx_t n, duck_instance* out, duck_err* err);
    duck_state (*instance_destroy)(duck_instance* inst);
    duck_state (*instance_attach)(duck_instance inst, const duck_str* name, const duck_str* path, void* options,
                                  bool make_default, duck_err* err);
    duck_state (*connection_create)(duck_instance inst, duck_connection* out, duck_err* err);
    duck_state (*connection_destroy)(duck_connection* con);
    duck_state (*connection_get_factory)(duck_connection con, duck_factory* out, duck_err* err);
    duck_state (*error_info_get_text)(duck_err err, duck_str* out);
    duck_state (*error_info_destroy)(duck_err* err);
    /* statements: parsed into an iterator, each executed into a cursor, bound by name or position at execution */
    duck_state (*parse_sql)(duck_connection con, const char* sql, duck_stmt_iter* out, duck_err* err);
    duck_state (*statement_iterator_next)(duck_stmt_iter it, duck_stmt* out, duck_err* err);
    duck_state (*statement_iterator_destroy)(duck_stmt_iter* it);
    duck_state (*sql_statement_destroy)(duck_stmt* st);
    duck_state (*sql_statement_get_parameter_count)(duck_stmt st, duck_idx_t* out, duck_err* err);
    duck_state (*sql_statement_get_parameter_name)(duck_stmt st, duck_idx_t index, duck_str* out, duck_err* err);
    duck_state (*sql_statement_get_type)(duck_stmt st, int32_t* out, duck_err* err);
    duck_state (*statement_bind)(duck_connection con, duck_stmt st, duck_schema* out, duck_schema* params, duck_err* err);
    duck_state (*statement_execute)(duck_connection con, duck_stmt st, const duck_str* names, const duck_value* values,
                                    duck_idx_t n, duck_cursor* out, duck_err* err);
    duck_state (*result_destroy)(duck_cursor* res);
    duck_state (*result_fetch_chunk)(duck_cursor res, duck_data_chunk* out, duck_err* err);   /* NULL at the end */
    duck_state (*result_get_result_type)(duck_cursor res, int32_t* out, duck_err* err);
    duck_state (*result_get_schema)(duck_cursor res, duck_schema* out, duck_err* err);
    duck_state (*schema_get_count)(duck_schema s, duck_idx_t* out, duck_err* err);
    duck_state (*schema_get_field)(duck_schema s, duck_idx_t i, duck_str* name, duck_logical_type* type, duck_err* err);
    duck_state (*schema_destroy)(duck_schema* s);
    /* chunks and vectors: a view to read, mutable data + validity + an arena to write, children by index */
    duck_state (*data_chunk_create)(duck_factory f, const duck_logical_type* types, duck_idx_t n, duck_data_chunk* out,
                                    duck_err* err);
    duck_state (*data_chunk_destroy)(duck_data_chunk* chunk);
    duck_state (*data_chunk_get_size)(duck_data_chunk chunk, duck_idx_t* out, duck_err* err);
    duck_state (*data_chunk_get_vector)(duck_data_chunk chunk, duck_idx_t col, duck_vector* out, duck_err* err);
    duck_state (*vector_get_logical_type)(duck_vector vec, duck_logical_type* out, duck_err* err);
    duck_state (*vector_flatten)(duck_vector vec, duck_err* err);
    duck_state (*vector_get_view)(duck_vector vec, duck_view* out, duck_err* err);
    duck_state (*vector_get_data_mutable)(duck_vector vec, void** out, duck_err* err);
    duck_state (*vector_flat_get_validity_mutable)(duck_vector vec, uint64_t** out, duck_err* err);
    duck_state (*vector_get_size)(duck_vector vec, duck_idx_t* out, duck_err* err);
    duck_state (*vector_set_size)(duck_vector vec, duck_idx_t size, duck_err* err);
    duck_state (*vector_get_arena)(duck_vector vec, duck_arena* out, duck_err* err);
    duck_state (*arena_allocate)(duck_arena arena, duck_idx_t n, uint8_t** out, duck_err* err);
    duck_state (*vector_get_child)(duck_vector vec, duck_idx_t index, duck_vector* out, duck_err* err);
    duck_state (*vector_get_value)(duck_vector vec, duck_idx_t row, duck_value* out, duck_err* err);
    /* logical types: an id, a name (the alias when one is set) and the value parameters that rebuild it */
    duck_state (*logical_type_copy)(duck_logical_type t, duck_logical_type* out, duck_err* err);
    duck_state (*logical_type_destroy)(duck_logical_type* t);
    duck_state (*logical_type_get_id)(duck_logical_type t, int32_t* out, duck_err* err);
    duck_state (*logical_type_get_name)(duck_logical_type t, duck_str* out, duck_err* err);
    duck_state (*logical_type_get_param_count)(duck_logical_type t, duck_idx_t* out, duck_err* err);
    duck_state (*logical_type_get_param)(duck_logical_type t, duck_idx_t i, duck_str* name, duck_value* out, duck_err* err);
    duck_state (*factory_create_type_from_id)(duck_factory f, int32_t id, const duck_str* names, const duck_value* values,
                                              duck_idx_t n, duck_logical_type* out, duck_err* err);
    duck_state (*factory_create_type_from_text)(duck_factory f, const duck_str* text, duck_logical_type* out,
                                                duck_err* err);
    /* values: the parameters bound and the type parameters read */
    duck_state (*value_destroy)(duck_value* v);
    duck_state (*value_is_null)(duck_value v, bool* out, duck_err* err);
    duck_state (*value_to_string)(duck_value v, char* out, duck_idx_t cap, duck_idx_t* len, duck_err* err);
    duck_state (*value_get_type)(duck_value v, duck_logical_type* out, duck_err* err);
    duck_state (*value_get_varchar)(duck_value v, duck_str* out, duck_err* err);
    duck_state (*value_get_utinyint)(duck_value v, uint8_t* out, duck_err* err);
    duck_state (*value_get_bigint)(duck_value v, int64_t* out, duck_err* err);
    duck_state (*value_create_type)(duck_factory f, duck_logical_type t, duck_value* out, duck_err* err);
    duck_state (*value_create_null)(duck_factory f, duck_logical_type t, duck_value* out, duck_err* err);
    duck_state (*value_create_bool)(duck_factory f, bool v, duck_value* out, duck_err* err);
    duck_state (*value_create_utinyint)(duck_factory f, uint8_t v, duck_value* out, duck_err* err);
    duck_state (*value_create_smallint)(duck_factory f, int16_t v, duck_value* out, duck_err* err);
    duck_state (*value_create_int)(duck_factory f, int32_t v, duck_value* out, duck_err* err);
    duck_state (*value_create_bigint)(duck_factory f, int64_t v, duck_value* out, duck_err* err);
    duck_state (*value_create_float)(duck_factory f, float v, duck_value* out, duck_err* err);
    duck_state (*value_create_double)(duck_factory f, double v, duck_value* out, duck_err* err);
    duck_state (*value_create_varchar)(duck_factory f, const duck_str* v, duck_value* out, duck_err* err);
    duck_state (*value_create_date)(duck_factory f, int32_t days, duck_value* out, duck_err* err);
    duck_state (*value_create_interval)(duck_factory f, const duck_interval* v, duck_value* out, duck_err* err);
    duck_state (*value_create_timestamp)(duck_factory f, int64_t micros, duck_value* out, duck_err* err);
    duck_state (*value_create_timestamp_ns)(duck_factory f, int64_t nanos, duck_value* out, duck_err* err);
    duck_state (*value_create_uuid)(duck_factory f, const duck_hugeint* storage, duck_value* out, duck_err* err);
    duck_state (*value_create_list)(duck_factory f, duck_logical_type child, const duck_value* kids, duck_idx_t n,
                                    duck_value* out, duck_err* err);
    /* the write path: chunks appended to a collection, served under a transient name by the connection's
     * replacement scan (docs/duckdb-architecture.md) */
    duck_state (*column_data_collection_create)(duck_factory f, const duck_logical_type* types, duck_idx_t n,
                                                duck_collection* out, duck_err* err);
    duck_state (*column_data_collection_destroy)(duck_collection* c);
    duck_state (*column_data_collection_append_state_create)(duck_collection c, duck_append_state* out, duck_err* err);
    duck_state (*column_data_collection_append_state_destroy)(duck_append_state* s);
    duck_state (*column_data_collection_append)(duck_collection c, duck_append_state s, duck_data_chunk chunk, duck_err* err);
    duck_state (*replacement_scan_create)(duck_factory f, duck_rscan* out, duck_err* err);
    duck_state (*replacement_scan_set_callback)(duck_rscan scan, duck_rscan_fn fn, duck_err* err);
    duck_state (*replacement_scan_set_user_data)(duck_rscan scan, duck_opaque* data, duck_err* err);
    duck_state (*connection_register_replacement_scan)(duck_connection con, duck_rscan scan, duck_err* err);
    duck_state (*replacement_scan_destroy)(duck_rscan* scan);
    duck_state (*replacement_scan_get_user_data)(duck_rscan_info info, void** out, duck_err* err);
    duck_state (*replacement_scan_get_name)(duck_rscan_info info, duck_qname* out, duck_err* err);
    duck_state (*replacement_scan_set_collection)(duck_rscan_info info, duck_collection c, const duck_str* names,
                                                  duck_idx_t n, duck_err* err);
    duck_state (*qname_get_part_count)(duck_qname q, duck_idx_t* out, duck_err* err);
    duck_state (*qname_get_part)(duck_qname q, duck_idx_t i, duck_str* out, duck_err* err);
    duck_state (*qname_destroy)(duck_qname* q);
} duck_api_t;

#endif /* Q_DUCKDB_API_H */

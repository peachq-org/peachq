/* q_duckdb_api — private, transcribed subset of DuckDB's stable C ABI.
 *
 * Transcribed from the MIT-licensed `duckdb.h` shipped with the DuckDB v1.4.5
 * prebuilt (github.com/duckdb/duckdb).  Deliberately NOT #include'd: the
 * bridge reaches DuckDB ONLY via dlopen + a dlsym'd fn-pointer table
 * (docs/duckdb-api.md, Packaging decision), so a plain `make` compiles with no
 * vendor tree present.  Struct layouts here are DuckDB's frozen C ABI (the
 * "deprecated_*" result fields are kept upstream precisely for stability); the
 * >= 1.4 version gate in q_duckdb.c is the tripwire if a future major breaks it. */
#ifndef Q_DUCKDB_API_H
#define Q_DUCKDB_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint64_t duck_idx_t;

typedef enum { QDuckSuccess = 0, QDuckError = 1 } duck_state;

/* What a statement answered (values are ABI): a query returned rows, anything else counted or did nothing. */
typedef enum {
    QDuckReturnInvalid = 0, QDuckReturnChangedRows = 1, QDuckReturnNothing = 2, QDuckReturnQuery = 3
} duck_return_t;

/* duckdb_type ids the bridge dispatches on (values are ABI). */
enum {
    QDUCK_TYPE_BOOLEAN      = 1,
    QDUCK_TYPE_TINYINT      = 2,
    QDUCK_TYPE_SMALLINT     = 3,
    QDUCK_TYPE_INTEGER      = 4,
    QDUCK_TYPE_BIGINT       = 5,
    QDUCK_TYPE_UTINYINT     = 6,
    QDUCK_TYPE_USMALLINT    = 7,
    QDUCK_TYPE_UINTEGER     = 8,
    QDUCK_TYPE_UBIGINT      = 9,
    QDUCK_TYPE_FLOAT        = 10,
    QDUCK_TYPE_DOUBLE       = 11,
    QDUCK_TYPE_TIMESTAMP    = 12,   /* int64 µs since 1970 */
    QDUCK_TYPE_DATE         = 13,
    QDUCK_TYPE_TIME         = 14,
    QDUCK_TYPE_INTERVAL     = 15,   /* {months; days; int64 µs} — the q durations' carrier (ADR 19) */
    QDUCK_TYPE_HUGEINT      = 16,
    QDUCK_TYPE_VARCHAR      = 17,
    QDUCK_TYPE_BLOB         = 18,
    QDUCK_TYPE_DECIMAL      = 19,
    QDUCK_TYPE_TIMESTAMP_S  = 20,   /* int64 s since 1970 */
    QDUCK_TYPE_TIMESTAMP_MS = 21,   /* int64 ms since 1970 */
    QDUCK_TYPE_TIMESTAMP_NS = 22,   /* int64 ns since 1970 — q timestamp's pair */
    QDUCK_TYPE_ENUM         = 23,
    QDUCK_TYPE_LIST         = 24,
    QDUCK_TYPE_STRUCT       = 25,
    QDUCK_TYPE_MAP          = 26,
    QDUCK_TYPE_UUID         = 27,
    QDUCK_TYPE_UNION        = 28,
    QDUCK_TYPE_BIT          = 29,   /* string_t: one padding-count byte, then the bits MSB-first */
    QDUCK_TYPE_TIME_TZ      = 30,   /* uint64 bits: µs << 24, then 57599 - the offset's seconds (ADR 21) */
    QDUCK_TYPE_TIMESTAMP_TZ = 31,   /* int64 µs since 1970, UTC — the zone is the session's, never stored */
    QDUCK_TYPE_UHUGEINT     = 32,
    QDUCK_TYPE_ARRAY        = 33,
    QDUCK_TYPE_VARINT       = 35,   /* string_t: a 3-byte header, then big-endian magnitude bytes (ADR 21) */
    QDUCK_TYPE_TIME_NS      = 39,   /* int64 ns since midnight, never past the 24th hour — q timespan's pair */
};
typedef int32_t duck_type;

/* Every DuckDB type id spelled as DuckDB itself declares it (`typeof`/DESCRIBE), in DUCKDB_TYPE enum order, the ids
 * above included.  The base word of the envelope's `dtype` and of the message channel; dispatch stays on the enum.
 * Slots 37 and 38 are the two LITERAL types the binder resolves before execution, so no result vector can carry one
 * and no probe can confirm their spelling: they keep the fallback rather than a guess. */
static inline const char* q_duckdb_type_name(duck_type t) {
    static const char* const names[] = {
        "INVALID", "BOOLEAN", "TINYINT", "SMALLINT", "INTEGER", "BIGINT", "UTINYINT", "USMALLINT", "UINTEGER",
        "UBIGINT", "FLOAT", "DOUBLE", "TIMESTAMP", "DATE", "TIME", "INTERVAL", "HUGEINT", "VARCHAR", "BLOB",
        "DECIMAL", "TIMESTAMP_S", "TIMESTAMP_MS", "TIMESTAMP_NS", "ENUM", "LIST", "STRUCT", "MAP", "UUID", "UNION",
        "BIT", "TIME WITH TIME ZONE", "TIMESTAMP WITH TIME ZONE", "UHUGEINT", "ARRAY", "ANY", "VARINT", "SQLNULL",
        "?", "?", "TIME_NS", "GEOMETRY", "VARIANT",
    };
    return t >= 0 && (size_t)t < sizeof names / sizeof *names ? names[t] : "?";
}

typedef struct duck_database_o*   duck_database;
typedef struct duck_connection_o* duck_connection;
typedef struct duck_config_o*     duck_config;
typedef struct duck_data_chunk_o* duck_data_chunk;
typedef struct duck_vector_o*     duck_vector;
typedef struct duck_appender_o*   duck_appender;
typedef struct duck_logical_o*    duck_logical_type;

typedef struct { uint64_t lower; int64_t  upper; } duck_hugeint;   /* HUGEINT, and UUID storage */
typedef struct { uint64_t lower; uint64_t upper; } duck_uhugeint;

/* One INTERVAL cell — 16 bytes, the same chunk shape HUGEINT and UUID already read and write, so the appender
 * needs no symbol of its own.  Months and days are units of no fixed length; only micros is a duration. */
typedef struct { int32_t months; int32_t days; int64_t micros; } duck_interval;

/* One LIST cell: a {start,count} window into the vector's flat child. */
typedef struct { uint64_t offset; uint64_t length; } duck_list_entry;

/* Inline-or-pointer string cell used inside VARCHAR/BLOB vectors. */
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

/* duckdb_result — layout frozen by upstream; only ever passed by address. */
typedef struct {
    duck_idx_t deprecated_column_count;
    duck_idx_t deprecated_row_count;
    duck_idx_t deprecated_rows_changed;
    void*      deprecated_columns;
    char*      deprecated_error_message;
    void*      internal_data;
} duck_result;

/* Validity bitmask (documented layout: 64 rows per word, bit set = valid);
 * decoded inline on read, official setter used on write. */
static inline bool q_duckdb_validity_ok(const uint64_t* validity, duck_idx_t row) {
    return validity == NULL || (validity[row >> 6] & (1ULL << (row & 63))) != 0;
}

typedef struct {
    const char* (*library_version)(void);
    duck_state (*open_ext)(const char* path, duck_database* out, duck_config cfg, char** out_err);
    void       (*close)(duck_database* db);
    duck_state (*create_config)(duck_config* out);
    duck_state (*set_config)(duck_config cfg, const char* name, const char* option);
    void       (*destroy_config)(duck_config* cfg);
    duck_state (*connect)(duck_database db, duck_connection* out);
    void       (*disconnect)(duck_connection* con);
    duck_state (*query)(duck_connection con, const char* sql, duck_result* out);
    void       (*destroy_result)(duck_result* res);
    duck_idx_t (*column_count)(duck_result* res);
    const char* (*column_name)(duck_result* res, duck_idx_t col);
    duck_data_chunk (*fetch_chunk)(duck_result res);          /* by value (ABI) */
    void       (*destroy_data_chunk)(duck_data_chunk* chunk);
    duck_idx_t (*data_chunk_get_size)(duck_data_chunk chunk);
    duck_vector (*data_chunk_get_vector)(duck_data_chunk chunk, duck_idx_t col);
    void*      (*vector_get_data)(duck_vector vec);
    uint64_t*  (*vector_get_validity)(duck_vector vec);
    void       (*vector_ensure_validity_writable)(duck_vector vec);
    void       (*validity_set_row_invalid)(uint64_t* validity, duck_idx_t row);
    void       (*vector_assign_string_element_len)(duck_vector vec, duck_idx_t idx,
                                                   const char* str, duck_idx_t len);
    duck_idx_t (*vector_size)(void);
    duck_logical_type (*create_logical_type)(duck_type type);
    void       (*destroy_logical_type)(duck_logical_type* type);
    duck_data_chunk (*create_data_chunk)(duck_logical_type* types, duck_idx_t ncols);
    void       (*data_chunk_reset)(duck_data_chunk chunk);
    void       (*data_chunk_set_size)(duck_data_chunk chunk, duck_idx_t size);
    duck_state (*appender_create_ext)(duck_connection con, const char* catalog,
                                      const char* schema, const char* table, duck_appender* out);
    duck_state (*appender_destroy)(duck_appender* app);       /* flushes */
    duck_state (*append_data_chunk)(duck_appender app, duck_data_chunk chunk);
    /* .duckdb.err[] sources — diagnostic text, never error-value payload */
    const char* (*result_error)(duck_result* res);
    const char* (*appender_error)(duck_appender app);
    void       (*duck_free)(void* p);
    duck_state (*appender_flush)(duck_appender app);  /* flush BEFORE destroy so
                                                       * deferred errors are readable */
    /* LIST columns: the child type is carried by the logical type, the child
     * data by a second vector the entries index into. */
    duck_logical_type (*create_list_type)(duck_logical_type child);
    duck_logical_type (*list_type_child_type)(duck_logical_type t);
    duck_logical_type (*column_logical_type)(duck_result* res, duck_idx_t col);
    duck_type  (*get_type_id)(duck_logical_type t);
    duck_vector (*list_vector_get_child)(duck_vector vec);
    duck_idx_t  (*list_vector_get_size)(duck_vector vec);
    duck_state (*list_vector_reserve)(duck_vector vec, duck_idx_t required);
    duck_state (*list_vector_set_size)(duck_vector vec, duck_idx_t size);
    /* the declaration spelling of a result column (the envelope's dtype): the alias JSON rides on a VARCHAR,
     * DECIMAL/ENUM/ARRAY carry their parameters.  The char* results are freed with duck_free. */
    char*      (*logical_type_get_alias)(duck_logical_type t);
    uint8_t    (*decimal_width)(duck_logical_type t);
    uint8_t    (*decimal_scale)(duck_logical_type t);
    duck_logical_type (*array_type_child_type)(duck_logical_type t);
    duck_idx_t (*array_type_array_size)(duck_logical_type t);
    uint32_t   (*enum_dictionary_size)(duck_logical_type t);
    char*      (*enum_dictionary_value)(duck_logical_type t, duck_idx_t index);
    /* the read side of ENUM and fixed ARRAY: an enum vector holds dictionary indices as wide as its internal type
     * says; an array vector's child holds every row's elements back to back, array_size each */
    duck_logical_type (*vector_get_column_type)(duck_vector vec);
    duck_type  (*enum_internal_type)(duck_logical_type t);
    duck_vector (*array_vector_get_child)(duck_vector vec);
    duck_return_t (*result_return_type)(duck_result res);     /* by value (ABI) */
    /* the integer storage a DECIMAL keeps its unscaled value in: SMALLINT, INTEGER, BIGINT or HUGEINT by width */
    duck_type  (*decimal_internal_type)(duck_logical_type t);
    /* the nested records (ADR 12).  A STRUCT's children, a UNION's members and a MAP's key/value are all read
     * through one shape; physically a UNION vector is a struct whose child 0 is the UTINYINT tag and a MAP
     * vector is a LIST whose child is STRUCT("key", "value"), so both ride the struct and list vector calls. */
    duck_idx_t (*struct_type_child_count)(duck_logical_type t);
    char*      (*struct_type_child_name)(duck_logical_type t, duck_idx_t index);
    duck_logical_type (*struct_type_child_type)(duck_logical_type t, duck_idx_t index);
    duck_vector (*struct_vector_get_child)(duck_vector vec, duck_idx_t index);
    duck_logical_type (*create_struct_type)(duck_logical_type* types, const char** names, duck_idx_t n);
    duck_logical_type (*map_type_key_type)(duck_logical_type t);
    duck_logical_type (*map_type_value_type)(duck_logical_type t);
    duck_logical_type (*create_map_type)(duck_logical_type key, duck_logical_type value);
    duck_idx_t (*union_type_member_count)(duck_logical_type t);
    char*      (*union_type_member_name)(duck_logical_type t, duck_idx_t index);
    duck_logical_type (*union_type_member_type)(duck_logical_type t, duck_idx_t index);
    duck_logical_type (*create_union_type)(duck_logical_type* types, const char** names, duck_idx_t n);
    /* The DEPRECATED materialised-result readers, reached ONLY by .duckdb.unsafeExecText: they force the legacy
     * column materialisation, after which duckdb_fetch_chunk on that SAME result yields nothing — so no result is
     * ever read both ways.  value_varchar's text is DuckDB's own, freed with duck_free; NULL = SQL NULL. */
    duck_idx_t (*row_count)(duck_result* res);
    char*      (*value_varchar)(duck_result* res, duck_idx_t col, duck_idx_t row);
    bool       (*value_is_null)(duck_result* res, duck_idx_t col, duck_idx_t row);
} duck_api_t;

#endif /* Q_DUCKDB_API_H */

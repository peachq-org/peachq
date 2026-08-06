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

/* duckdb_type ids the bridge dispatches on (values are ABI). */
enum {
    QDUCK_TYPE_BOOLEAN      = 1,
    QDUCK_TYPE_SMALLINT     = 3,
    QDUCK_TYPE_INTEGER      = 4,
    QDUCK_TYPE_BIGINT       = 5,
    QDUCK_TYPE_UTINYINT     = 6,
    QDUCK_TYPE_FLOAT        = 10,
    QDUCK_TYPE_DOUBLE       = 11,
    QDUCK_TYPE_DATE         = 13,
    QDUCK_TYPE_TIME         = 14,
    QDUCK_TYPE_VARCHAR      = 17,
    QDUCK_TYPE_BLOB         = 18,
    QDUCK_TYPE_TIMESTAMP_NS = 22,   /* int64 ns since 1970 — q timestamp's pair */
    QDUCK_TYPE_UUID         = 27,
};
typedef int32_t duck_type;

typedef struct duck_database_o*   duck_database;
typedef struct duck_connection_o* duck_connection;
typedef struct duck_config_o*     duck_config;
typedef struct duck_data_chunk_o* duck_data_chunk;
typedef struct duck_vector_o*     duck_vector;
typedef struct duck_appender_o*   duck_appender;
typedef struct duck_logical_o*    duck_logical_type;

typedef struct { uint64_t lower; int64_t upper; } duck_hugeint;  /* UUID storage */

/* Inline-or-pointer string cell used inside VARCHAR/BLOB vectors. */
typedef struct {
    union {
        struct { uint32_t length; char prefix[4]; char* ptr; } pointer;
        struct { uint32_t length; char inlined[12]; } inlined;
    } value;
} duck_string_t;

#define QDUCK_STRING_INLINE_MAX 12
static inline const char* duck_string_data(const duck_string_t* s) {
    return s->value.inlined.length <= QDUCK_STRING_INLINE_MAX
               ? s->value.inlined.inlined
               : s->value.pointer.ptr;
}
static inline uint32_t duck_string_len(const duck_string_t* s) {
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
static inline bool duck_validity_ok(const uint64_t* validity, duck_idx_t row) {
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
    duck_type  (*column_type)(duck_result* res, duck_idx_t col);
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
} duck_api_t;

#endif /* Q_DUCKDB_API_H */

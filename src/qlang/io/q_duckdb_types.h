/* q_duckdb_types — hub logical-type vocabulary (spec:
 * docs/superpowers/specs/2026-07-14-duckdb-fidelity-design.md).
 * CONTRACT surface, APPEND-ONLY: never rename/re-mean/re-carrier a row; add a
 * new one.  The wire-tier rows (int128/timestamptz/timetz/timeus) return with
 * the wire arm — this port carries the v1 storage vocabulary only. */
#ifndef Q_DUCKDB_TYPES_H
#define Q_DUCKDB_TYPES_H

#include "qlang/io/q_duckdb_api.h"   /* duck_type, QDUCK_TYPE_* */
#include <rayforce.h>                /* RAY_* column type tags */
#include <stdbool.h>
#include <stdint.h>

/* q epoch 2000.01.01 vs DuckDB 1970-01-01; every temporal row shifts by these. */
#define QD_EPOCH_DAYS 10957
#define QD_EPOCH_NS   946684800000000000LL

typedef struct {
    int8_t      ray_type;      /* q column carrier tag (RAY_STR = string column,
                                * surfacing as a 0h list of charv cells) */
    duck_type   dk_type;       /* DuckDB type id (write DDL + chunk vectors) */
    const char* sql;           /* canonical DDL spelling */
    const char* logical;       /* hub logical-type name (descriptor spelling) */
    char        meta_ch;       /* .duckdb.meta `t` char (' ' = no equivalent) */
    bool        read_canon;    /* identity row: what bare reads of dk_type produce */
} qd_tmap_t;

/* Read dispatch takes the FIRST row matching a DuckDB type as the identity
 * (degrade) target; non-canon rows need a validating _q_schema row. */
static const qd_tmap_t QD_TYPES[] = {
    { RAY_BOOL,      QDUCK_TYPE_BOOLEAN,      "BOOLEAN",      "bool",      'b', true  },
    { RAY_BYTE_ONLY, QDUCK_TYPE_UTINYINT,     "UTINYINT",     "uint8",     'x', true  },
    { RAY_I16,       QDUCK_TYPE_SMALLINT,     "SMALLINT",     "int16",     'h', true  },
    { RAY_I32,       QDUCK_TYPE_INTEGER,      "INTEGER",      "int32",     'i', true  },
    { RAY_I64,       QDUCK_TYPE_BIGINT,       "BIGINT",       "int64",     'j', true  },
    { RAY_F32,       QDUCK_TYPE_FLOAT,        "REAL",         "float32",   'e', true  },
    { RAY_F64,       QDUCK_TYPE_DOUBLE,       "DOUBLE",       "float64",   'f', true  },
    { RAY_STR,       QDUCK_TYPE_VARCHAR,      "VARCHAR",      "utf8",      's', true  },
    { RAY_SYM,       QDUCK_TYPE_VARCHAR,      "VARCHAR",      "symbol",    's', false },
    { RAY_LIST,      QDUCK_TYPE_BLOB,         "BLOB",         "bytes",     'X', true  },
    { RAY_DATE,      QDUCK_TYPE_DATE,         "DATE",         "date",      'd', true  },
    { RAY_TIME,      QDUCK_TYPE_TIME,         "TIME",         "time",      't', true  },
    { RAY_TIMESTAMP, QDUCK_TYPE_TIMESTAMP_NS, "TIMESTAMP_NS", "timestamp", 'p', true  },
    { RAY_TIMESPAN,  QDUCK_TYPE_BIGINT,       "BIGINT",       "timespan",  'n', false },
    { RAY_GUID,      QDUCK_TYPE_UUID,         "UUID",         "guid",      'g', true  },
    /* sidecar riders: raw underlying int/float in the identity physical —
     * only the descriptor tells them apart. */
    { RAY_MONTH,     QDUCK_TYPE_INTEGER,      "INTEGER",      "month",     'm', false },
    { RAY_MINUTE,    QDUCK_TYPE_INTEGER,      "INTEGER",      "minute",    'u', false },
    { RAY_SECOND,    QDUCK_TYPE_INTEGER,      "INTEGER",      "second",    'v', false },
    { RAY_DATETIME,  QDUCK_TYPE_DOUBLE,       "DOUBLE",       "datetime",  'z', false },
};
#define QD_NTYPES (sizeof QD_TYPES / sizeof *QD_TYPES)

#endif /* Q_DUCKDB_TYPES_H */

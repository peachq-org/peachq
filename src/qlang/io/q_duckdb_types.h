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
#define QD_EPOCH_MS   946684800000LL

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
    { RAY_F32,       QDUCK_TYPE_FLOAT,        "FLOAT",        "float32",   'e', true  },
    { RAY_F64,       QDUCK_TYPE_DOUBLE,       "DOUBLE",       "float64",   'f', true  },
    { RAY_STR,       QDUCK_TYPE_VARCHAR,      "VARCHAR",      "utf8",      'C', true  },
    { RAY_SYM,       QDUCK_TYPE_VARCHAR,      "VARCHAR",      "symbol",    's', false },
    { RAY_LIST,      QDUCK_TYPE_BLOB,         "BLOB",         "bytes",     'X', true  },
    { RAY_DATE,      QDUCK_TYPE_DATE,         "DATE",         "date",      'd', true  },
    { RAY_TIME,      QDUCK_TYPE_INTERVAL,     "INTERVAL",     "time",      't', false },
    { RAY_TIMESTAMP, QDUCK_TYPE_TIMESTAMP_NS, "TIMESTAMP_NS", "timestamp", 'p', true  },
    { RAY_TIMESPAN,  QDUCK_TYPE_BIGINT,       "BIGINT",       "timespan",  'n', false },
    { RAY_GUID,      QDUCK_TYPE_UUID,         "UUID",         "guid",      'g', true  },
    /* sidecar riders: the four q grains SQL has no type of its own for.  ADR 11's `duckvisible` law (2026-09-07):
     * each stores as the DuckDB type its VALUE is — a month is the DATE of its first day, a datetime a TIMESTAMP —
     * so SQL sees a date/time, not a number; only the descriptor tells the grain.  ADR 19 (2026-09-09): the three
     * DURATIONS take INTERVAL, whose micros field is a signed int64, because TIME's 0..24h domain is narrower than
     * the q type it carries and a count outside it leaves a store DuckDB cannot render. */
    { RAY_MONTH,     QDUCK_TYPE_DATE,         "DATE",         "month",     'm', false },
    { RAY_MINUTE,    QDUCK_TYPE_INTERVAL,     "INTERVAL",     "minute",    'u', false },
    { RAY_SECOND,    QDUCK_TYPE_INTERVAL,     "INTERVAL",     "second",    'v', false },
    { RAY_DATETIME,  QDUCK_TYPE_TIMESTAMP,    "TIMESTAMP",    "datetime",  'z', false },
    /* read-side widenings for SQL-born columns; writes keep the rows above */
    { RAY_I16,       QDUCK_TYPE_TINYINT,      "TINYINT",      "int8",      'h', true  },
    /* ADR 19: INTERVAL's canonical read is `n`, months 0 folding days and micros to nanoseconds exactly; a value
     * with months has no q carrier and rides the raw companion.  A DuckDB-BORN TIME column is untouched — it reads
     * as `t` and its declared type comes back through the envelope, exactly as `timestampus` serves `p`. */
    { RAY_TIMESPAN,  QDUCK_TYPE_INTERVAL,     "INTERVAL",     "interval",  'n', true  },
    { RAY_TIME,      QDUCK_TYPE_TIME,         "TIME",         "timeus",    't', true  },
    { RAY_TIMESTAMP, QDUCK_TYPE_TIMESTAMP,    "TIMESTAMP",    "timestampus", 'p', true },
    /* the ADR 5 targets (2026-09-06): lossless widenings and rescalings, the declared type back through the
     * envelope; a DuckDB ENUM decodes its dictionary to symbols under no q domain (the dictionary rides the
     * envelope's dtype; `enumdom` below is the q-BORN column's); a fixed ARRAY is grammar like LIST, not a row */
    { RAY_I32,       QDUCK_TYPE_USMALLINT,    "USMALLINT",    "uint16",      'i', true },
    { RAY_I64,       QDUCK_TYPE_UINTEGER,     "UINTEGER",     "uint32",      'j', true },
    { RAY_TIMESTAMP, QDUCK_TYPE_TIMESTAMP_S,  "TIMESTAMP_S",  "timestampsec", 'p', true },
    { RAY_TIMESTAMP, QDUCK_TYPE_TIMESTAMP_MS, "TIMESTAMP_MS", "timestampms", 'p', true },
    { RAY_TIMESTAMP, QDUCK_TYPE_TIMESTAMP_TZ, "TIMESTAMP WITH TIME ZONE", "timestamptz", 'p', true },
    { RAY_SYM,       QDUCK_TYPE_ENUM,         "ENUM",         "categorical", 's', true },
    /* ADR 8 (2026-09-07): the two q-side refinements of VARCHAR the symbol row was the pattern for. A char
     * column stores one character per row; an enum column stores its SYMBOLS, its domain recorded in the
     * sidecar's `enumdom` and never re-applied on read (ruled 2026-09-13) — so no read carries the enum row,
     * and it has no `.duckdb.meta` char. */
    { RAY_CHARV,     QDUCK_TYPE_VARCHAR,      "VARCHAR",      "char",        'c', false },
    { RAY_ENUM,      QDUCK_TYPE_VARCHAR,      "VARCHAR",      "enum",        ' ', false },
    /* ADR 4 (2026-09-07): the wide integer family (UBIGINT above 2^63, HUGEINT, UHUGEINT) keeps its LOW word in `j`
     * and, once any row needs it, its HIGH word in <c>_q_hi; the declared type comes back through the envelope */
    { RAY_I64,       QDUCK_TYPE_UBIGINT,      "UBIGINT",      "uint64",      'j', true },
    { RAY_I64,       QDUCK_TYPE_HUGEINT,      "HUGEINT",      "int128",      'j', true },
    { RAY_I64,       QDUCK_TYPE_UHUGEINT,     "UHUGEINT",     "uint128",     'j', true },
    /* ADR 4: a DECIMAL column is its UNSCALED integer, the scale carried by the envelope's dtype and nothing
     * else beside it; the word joins the family above, so it needs <c>_q_hi only past 18 digits */
    { RAY_I64,       QDUCK_TYPE_DECIMAL,      "DECIMAL",      "decimal",     'j', true },
    /* ADR 12 (2026-09-07): the three nested RECORDS are one carrier — a q DICT per cell — and the declared type
     * is the only thing that says which of them a dict was, so all three need it and none has a meta char */
    { RAY_LIST,      QDUCK_TYPE_STRUCT,       "STRUCT",       "struct",      ' ', true },
    { RAY_LIST,      QDUCK_TYPE_MAP,          "MAP",          "map",         ' ', true },
    { RAY_LIST,      QDUCK_TYPE_UNION,        "UNION",        "union",       ' ', true },
    /* ADR 20 (2026-09-09): a BIT is an ordered bit SEQUENCE whose length is part of its identity, so it carries as
     * a q boolean vector per cell — `count` is the bit count and leading zeros survive.  Null-less like BLOB, and
     * like BLOB it wears the compound meta char of the cells it holds. */
    { RAY_LIST,      QDUCK_TYPE_BIT,          "BIT",          "bitstring",   'B', true },
    /* ADR 21 (2026-09-10): a zoned temporal is a PAIR of integers and q has a carrier for each half, so the wall
     * time (TIMETZ) or the UTC instant (TIMESTAMPTZ) takes the slot and the offset's seconds ride c_q_tzoff.  A
     * BIGNUM is unbounded — ::BIGINT and ::HUGEINT both refuse it — so no fixed-width carrier can ever suffice:
     * the slot takes 0N and the digits ride the raw companion, ADR 11's mechanism unchanged. */
    { RAY_TIME,      QDUCK_TYPE_TIME_TZ,      "TIME WITH TIME ZONE", "timetz", 't', true },
    { RAY_I64,       QDUCK_TYPE_VARINT,       "VARINT",       "varint",      'j', true },
    /* ADR 2a (2026-09-11): TIME_NS is a NANOSECOND time of day, so `t` would read nearly every value as 0Nt — `n` is
     * exact and total instead, its 0..24h domain six orders inside the carrier and unable to reach 0Nn, so this is
     * the one temporal that needs no companion in either direction.  A q-BORN `n` still stores BIGINT above. */
    { RAY_TIMESPAN,  QDUCK_TYPE_TIME_NS,      "TIME_NS",      "timens",      'n', true },
};
#define QD_NTYPES (sizeof QD_TYPES / sizeof *QD_TYPES)

#endif /* Q_DUCKDB_TYPES_H */

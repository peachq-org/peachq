/* q_duckdb_internal — the shared surface of the q_duckdb.c three-file split: what
 * q_duckdb.c (lifecycle + verb bodies), q_duckdb_codec.c (the type contract both
 * directions) and q_duckdb_schema.c (declared types) call across the seam.  These
 * were file-local statics in the monolith.  NOT public API — that is q_duckdb.h. */
#ifndef Q_DUCKDB_INTERNAL_H
#define Q_DUCKDB_INTERNAL_H

#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_types.h"
#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- q_duckdb.c: the loaded table, the connection slots, the message channel ---- */

extern duck_api_t qd_api;          /* the dlsym'd fn table; all-zero until the loader runs */
#define QAPI (qd_api)
#define QD_TEXT(s) ((s) ? (s) : "")

duck_connection qd_con(int slot);
void   qd_err_stash(int slot, const char* fmt, ...);
ray_t* qd_fail(int slot, const char* what, const char* why);
ray_t* qd_run(int slot, const char* sql, duck_result* out);
ray_t* qd_run2(int slot, const char* sql, duck_result* out, int stash);
ray_t* qd_exec_stmt(int slot, const char* sql);

/* growable SQL text; `oom` latches and every put behind it is a no-op */
typedef struct { char* p; size_t len, cap; int oom; } qd_buf;
void qd_puts(qd_buf* b, const char* s);
void qd_put_ident(qd_buf* b, const char* s, size_t n);
void qd_put_strlit(qd_buf* b, const char* s, size_t n);
void qd_buf_free(qd_buf* b);

/* ---- column map: a manifest row under N levels of DuckDB LIST.  QD_TYPES[] is
 * an append-only contract (fidelity spec 2026-07-14), so nesting is code over
 * the flat table — never rows in it. */

#define QD_MAX_DEPTH 8

typedef struct {
    const qd_tmap_t* leaf;    /* the flat row at the bottom of the nest */
    int              depth;   /* DuckDB LIST levels; 0 = a flat column */
} qd_colmap_t;

typedef struct {
    char col[256];       /* data column name */
    char logical[96];    /* logical-type name (validated against the manifest) */
    bool iskey;          /* q keyed-table key column */
} qd_desc_t;

/* ---- q_duckdb_codec.c: the type contract, both directions ---- */

int8_t qd_surface_of(const qd_colmap_t* cm);
char   qd_meta_char(const qd_colmap_t* cm);
void   qd_logical_name(const qd_colmap_t* cm, char* buf, size_t cap);
bool   qd_parse_logical(const char* s, qd_colmap_t* out);
void   qd_refine(const char* cname, const qd_desc_t* desc, int64_t ndesc, qd_colmap_t* cm);
ray_t* qd_result_to_table(int slot, duck_result* res, const qd_desc_t* desc, int64_t ndesc);
ray_t* qd_check_table(ray_t* tbl, qd_colmap_t* cms);
ray_t* qd_append_table(int slot, const char* tname, ray_t* tbl,
                       const qd_colmap_t* cms, ray_t* const* masks);
ray_t* qd_strip_companions(int slot, ray_t* tbl, ray_t** masks, bool* iskey);

/* ---- q_duckdb_schema.c: the sidecar, the catalog spellings, the DDL ---- */

bool        qd_reserved_name(const char* tname);
const char* qd_text_cell(ray_t* col, int64_t i, size_t* len);
int64_t     qd_fetch_desc(int slot, const char* tname, qd_desc_t* out, int64_t cap);
ray_t*      qd_write_desc(int slot, const char* tname, ray_t* tbl,
                          const qd_colmap_t* cms, const bool* iskey);
ray_t*      qd_rekey(ray_t* tbl, const qd_desc_t* desc, int64_t ndesc);
bool        qd_catalog_col(const char* dt, size_t n, qd_colmap_t* out);
void        qd_create_ddl(qd_buf* b, const char* tname, ray_t* tbl, const qd_colmap_t* cms);
ray_t*      qd_schema_check(int slot, const char* tname, ray_t* tbl, const qd_colmap_t* cms);

#endif /* Q_DUCKDB_INTERNAL_H */

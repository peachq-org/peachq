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
#if !defined(_WIN32)
#include <strings.h>          /* strcasecmp / strncasecmp */
#else
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

/* ---- q_duckdb.c: the loaded table, the connection slots, the message channel ---- */

extern duck_api_t qd_api;          /* the dlsym'd fn table; all-zero until the loader runs */
#define QAPI (qd_api)
#define QD_TEXT(s) ((s) ? (s) : "")

/* drop an owned value that may be an error: ray_release short-circuits on errors, which ray_error_free reclaims */
static inline void q_duckdb_drop(ray_t* x) { if (RAY_IS_ERR(x)) ray_error_free(x); else ray_release(x); }

duck_connection q_duckdb_con(int slot);
void   q_duckdb_err_stash(int slot, const char* fmt, ...);
size_t q_duckdb_err_mark(void);
void   q_duckdb_err_rewind(int slot, size_t mark);
ray_t* q_duckdb_fail(int slot, const char* what, const char* why);
ray_t* q_duckdb_run(int slot, const char* sql, duck_result* out);
ray_t* q_duckdb_run2(int slot, const char* sql, duck_result* out, int stash);
ray_t* q_duckdb_exec_stmt(int slot, const char* sql);
const char* q_duckdb_err_text(int slot);

/* One row of `.duckdb.sqllog`: t0 = q_dotz_now_ns(0) read before the work, rows = NULL_I64 where the count is not
 * the bridge's to know, err = NULL when it succeeded.  Every SQL statement is noted inside q_duckdb_run2; the
 * appender, which is not SQL at all, notes a `/ appender ...` line of its own so the timeline has no hole. */
void q_duckdb_stmt_note(int slot, const char* sql, int64_t t0, bool ok, int64_t rows, const char* err);

/* growable SQL text; `oom` latches and every put behind it is a no-op */
typedef struct { char* p; size_t len, cap; int oom; } qd_buf;
void q_duckdb_puts(qd_buf* b, const char* s);
void q_duckdb_putn(qd_buf* b, const char* s, size_t n);
void q_duckdb_put_ident(qd_buf* b, const char* s, size_t n);
void q_duckdb_put_strlit(qd_buf* b, const char* s, size_t n);
void q_duckdb_buf_free(qd_buf* b);
char* q_duckdb_text(const char* s, size_t n);
void* q_duckdb_cols(int64_t n, size_t per);

/* what the write door reads off an enum column before decaying it: the domain the sidecar records and the attribute
 * the symbols cannot carry (a symbol vector takes no attribute in q); dom 0 = not an enum */
typedef struct { int64_t dom; char attr; } qd_enum_t;

/* A table name as a door receives it, parsed into its identifier parts (catalog, schema, table). */
#define QD_NAME_PARTS 3
typedef struct {
    char part[QD_NAME_PARTS][256];
    int  n;
    char schema[256];               /* resolved: given, else the session's */
    char key[QD_NAME_PARTS * 258];  /* the sidecar's row key, the IDENTITY of those parts (q_duckdb_schema_name_resolve) */
} qd_name_t;

/* ---- column map: a manifest row under N levels of DuckDB LIST.  QD_TYPES[] is
 * an append-only contract (fidelity spec 2026-07-14), so nesting is code over
 * the flat table — never rows in it. */

#define QD_MAX_DEPTH 8

typedef struct qd_rec_s qd_rec_t;

typedef struct {
    const qd_tmap_t* leaf;    /* the flat row at the bottom of the nest (a record row when `rec`) */
    const qd_rec_t*  rec;     /* the record's fields, NULL for a flat column */
    int              depth;   /* DuckDB LIST levels; 0 = a flat column */
    /* ADR 15: an EMPTY 0h column votes for nothing, so `leaf` here is a placeholder only a declaration makes real */
    bool             undet;
} qd_colmap_t;

/* A record's fields (ADR 12): a STRUCT's children, a UNION's members, or a MAP's two `key`/`value` halves.
 * OWNED, names included — heap-built by whoever maps the column, freed with q_duckdb_codec_map_free. */
typedef struct { char* name; qd_colmap_t map; } qd_field_t;
struct qd_rec_s { int n; qd_field_t f[]; };

/* One column's declared refinement: a `_q_schema` row on the way in, an envelope schema row either way.  col and
 * dtype are OWNED (q_duckdb_schema_desc_free); logical is bounded by construction, QD_MAX_DEPTH `list(` wraps
 * around the longest manifest name. */
#define QD_LOGICAL_MAX 96
typedef struct {
    char* col;                     /* data column name */
    char* dtype;                   /* DuckDB's own declaration spelling ("" = the carrier's canonical type) */
    char  logical[QD_LOGICAL_MAX]; /* logical-type name (validated against the manifest) */
    bool  iskey;                   /* q keyed-table key column */
} qd_desc_t;

/* ---- q_duckdb_codec.c: the type contract, both directions ---- */

int8_t q_duckdb_codec_surface_of(const qd_colmap_t* cm);
char   q_duckdb_codec_meta_char(const qd_colmap_t* cm);
void   q_duckdb_codec_logical_name(const qd_colmap_t* cm, char* buf, size_t cap);
bool   q_duckdb_codec_parse_logical(const char* s, qd_colmap_t* out);
const qd_tmap_t* q_duckdb_codec_canon_leaf(int8_t ray_type);
bool   q_duckdb_codec_raw_is_text(int8_t ray_type);
bool   q_duckdb_codec_is_rec(const qd_tmap_t* tm);
void   q_duckdb_codec_map_free(qd_colmap_t* cm);
void   q_duckdb_codec_maps_free(qd_colmap_t* cms, int64_t n);
void   q_duckdb_codec_spell_map(const qd_colmap_t* cm, qd_buf* b);
ray_t* q_duckdb_codec_map_declared(int slot, ray_t* col, duck_logical_type lt, duck_type kind, qd_colmap_t* cm);
bool   q_duckdb_codec_declared_stage_map(duck_logical_type lt, qd_colmap_t* out);
bool   q_duckdb_codec_untyped(ray_t* col);
bool   q_duckdb_codec_map_bits(ray_t* col, int depth, qd_colmap_t* cm);
int64_t q_duckdb_codec_rec_rows(ray_t* col);

/* The companions a column can grow, `<c>_q_<name>` (ADR 1/2/4/21): the two booleans flag a group's rare state, the
 * two longs carry what the slot cannot — a temporal's raw count, a 128-bit word's high half — and tzoff carries a
 * zoned temporal's UTC offset in seconds, the one companion that rides BESIDE another and is always emitted. */
enum { QD_CO_NONE, QD_CO_ISNULL, QD_CO_NOTNULL, QD_CO_RAW, QD_CO_HI, QD_CO_TZOFF };
#define QD_CO_MAX 3                /* the shape companion, a nested column's element companion, a zoned type's offset */
const char* q_duckdb_codec_co_name(int kind);
void   q_duckdb_codec_companion_col(qd_buf* b, const char* parent, size_t n, int kind);
int    q_duckdb_codec_mask_kind(const qd_colmap_t* cm, ray_t* mask);
int    q_duckdb_codec_keep_kind(const qd_colmap_t* cm, ray_t* keep);
bool   q_duckdb_codec_companion_name(const char* s, size_t n);
bool   q_duckdb_codec_zoned(const qd_colmap_t* cm);
bool   q_duckdb_codec_tz_all_zero(ray_t* off);
int    q_duckdb_codec_companions_of(const qd_colmap_t* cm, const char* out[QD_CO_MAX]);
ray_t* q_duckdb_codec_stage_image(ray_t* tbl, const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* offs,
                                  ray_t* const* keeps, qd_colmap_t* scms, ray_t** smasks, ray_t** skeeps);
void   q_duckdb_codec_refine(const char* cname, const qd_desc_t* desc, int64_t ndesc, qd_colmap_t* cm);
ray_t* q_duckdb_codec_result_to_table(int slot, duck_result* res, const qd_desc_t* desc, int64_t ndesc, ray_t** schema);
ray_t* q_duckdb_codec_check_table(int slot, ray_t* tbl, qd_colmap_t* cms);
ray_t* q_duckdb_codec_check_fields(int slot, ray_t* tbl, int64_t c, duck_logical_type lt, const qd_colmap_t* cm);
ray_t* q_duckdb_codec_decay_enums(int slot, ray_t* tbl, qd_enum_t* en);
ray_t* q_duckdb_codec_append_table(int slot, const qd_name_t* nm, bool temp, ray_t* tbl,
                                   const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* keeps);
ray_t* q_duckdb_codec_strip_companions(int slot, ray_t* tbl, ray_t** masks, ray_t** offs, ray_t** keeps,
                                       bool* iskey, qd_enum_t* en, bool bare);

/* ---- q_duckdb_schema.c: the sidecar, the catalog spellings, the DDL, the envelope's schema ---- */

#define QD_STAGE_TBL "_q_stage"       /* the temp table a declared type is staged in, then CAST out of */
#define QD_NEED_LOGICAL 1
#define QD_NEED_DTYPE   2

bool        q_duckdb_schema_name_parse(const char* s, size_t n, qd_name_t* out);
void        q_duckdb_schema_name_resolve(int slot, qd_name_t* nm);
void        q_duckdb_schema_put_name(qd_buf* b, const qd_name_t* nm);
void        q_duckdb_schema_put_catalog_where(qd_buf* b, const qd_name_t* nm);
bool        q_duckdb_schema_reserved_name(const qd_name_t* nm);
const char* q_duckdb_schema_text_cell(ray_t* col, int64_t i, size_t* len);
const qd_desc_t* q_duckdb_schema_desc_find(const char* name, size_t len, const qd_desc_t* desc, int64_t ndesc);
ray_t*      q_duckdb_schema_check_names(int slot, ray_t* tbl);
int64_t     q_duckdb_schema_fetch_desc(int slot, const qd_name_t* nm, qd_desc_t** out);
void        q_duckdb_schema_desc_free(qd_desc_t* d, int64_t n);
ray_t*      q_duckdb_schema_write_desc(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms,
                                       const bool* iskey, const qd_enum_t* en, const char* const* dtypes);
ray_t*      q_duckdb_schema_rekey(ray_t* tbl, const qd_desc_t* desc, int64_t ndesc);
bool        q_duckdb_schema_catalog_col(const char* dt, size_t n, qd_colmap_t* out);
void        q_duckdb_schema_create_ddl(qd_buf* b, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms, bool temp);
ray_t*      q_duckdb_schema_check(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms,
                                  const char* const* dtypes, bool* missing);
void        q_duckdb_schema_spell_type(duck_logical_type lt, qd_buf* b);
int         q_duckdb_schema_needs(const qd_colmap_t* cm, const char* dtype, bool empty);
ray_t*      q_duckdb_schema_desc_of(const char* cname, duck_logical_type lt, const qd_colmap_t* cm,
                                    const qd_desc_t* desc, int64_t ndesc, bool empty, qd_desc_t* out, bool* wanted);
ray_t*      q_duckdb_schema_desc_table(const qd_desc_t* rows, int64_t n);
ray_t*      q_duckdb_schema_desc_parse(int slot, ray_t* schema, qd_desc_t** out, int64_t* n);
ray_t*      q_duckdb_schema_declare(int slot, ray_t* tbl, qd_colmap_t* cms, qd_desc_t* decl, int64_t ndecl,
                                    const char** dtypes);
ray_t*      q_duckdb_schema_cast_select(int slot, qd_buf* b, ray_t* tbl, const qd_colmap_t* cms, const char* const* dtypes,
                                        ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps, const char* from);
ray_t*      q_duckdb_schema_exact_check(int slot, ray_t* tbl, const qd_colmap_t* cms, const char* const* dtypes,
                                        ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps);

#endif /* Q_DUCKDB_INTERNAL_H */

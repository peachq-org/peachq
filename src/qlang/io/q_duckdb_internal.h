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
#if defined(_WIN32)
/* mingw's <string.h> declares strcasecmp/strncasecmp as inline wrappers over _stricmp/_strnicmp; a `#define
 * strcasecmp _stricmp` seen first turns that wrapper into `_stricmp(){return _stricmp();}` — a hang at -O2 */
#include <string.h>
#else
#include <strings.h>          /* strcasecmp / strncasecmp */
#endif

/* ---- q_duckdb.c: the loaded table, the connection slots, the message channel ---- */

extern duck_api_t qd_api;          /* the dlsym'd fn table; all-zero until the loader runs */
#define QAPI (qd_api)
#define QD_TEXT(s) ((s) ? (s) : "")

/* drop an owned value that may be an error: ray_release short-circuits on errors, which ray_error_free reclaims */
static inline void q_duckdb_drop(ray_t* x) { if (RAY_IS_ERR(x)) ray_error_free(x); else ray_release(x); }

duck_factory q_duckdb_factory(int slot);
void   q_duckdb_err_stash(int slot, const char* fmt, ...);
size_t q_duckdb_err_mark(void);
void   q_duckdb_err_rewind(int slot, size_t mark);
ray_t* q_duckdb_fail(int slot, const char* fmt, ...);
ray_t* q_duckdb_fail_err(int slot, duck_err* e);
ray_t* q_duckdb_fail_col(int slot, const char* name, size_t n, const char* fmt, ...);
const char* q_duckdb_col_name(ray_t* tbl, int64_t c, size_t* len);
const char* q_duckdb_err_text(int slot);

/* One row of `.duckdb.sqllog`: t0 = q_dotz_now_ns(0) read before the work, rows = the rows a query answered or a
 * DML statement changed (NULL_I64 where there is no count), err = NULL when it succeeded.  Every statement is noted
 * inside q_duckdb_run — the row move of a write included, now that it is an INSERT. */
void q_duckdb_stmt_note(int slot, const char* sql, int64_t t0, bool ok, int64_t rows, int64_t cols,
                        const char* err);

/* growable SQL text; `oom` latches and every put behind it is a no-op */
typedef struct { char* p; size_t len, cap; int oom; } qd_buf;

/* the statement seam.  run: NULL and *out filled (the caller frees it with q_duckdb_result_free), or an owned error
 * with nothing to free; stash=0 is a probe whose failure stays off err[].  exec: run, the result dropped.  query: the
 * result as an OWNED table, `::` for a column-less statement, an owned error otherwise — never NULL.  The _buf forms
 * consume the built buffer either way; one that lost its text answers 'wsfull. */
ray_t* q_duckdb_run(int slot, const char* sql, duck_result* out, int stash);
ray_t* q_duckdb_exec(int slot, const char* sql);
ray_t* q_duckdb_exec_buf(int slot, qd_buf* b);
ray_t* q_duckdb_query(int slot, const char* sql, int stash);
ray_t* q_duckdb_query_buf(int slot, qd_buf* b, int stash);
void   q_duckdb_result_free(duck_result* r);
duck_logical_type q_duckdb_result_col_type(const duck_result* r, duck_idx_t c);   /* borrowed */

/* the name a write's row move reads its collection under, through the connection's replacement scan; reserved at
 * the write door and probed in the catalog before every move (q_duckdb_schema_batch_clear) */
#define QD_BATCH_TBL "_q_batch"

void q_duckdb_puts(qd_buf* b, const char* s);
void q_duckdb_putn(qd_buf* b, const char* s, size_t n);
void q_duckdb_put_ident(qd_buf* b, const char* s, size_t n);
void q_duckdb_put_strlit(qd_buf* b, const char* s, size_t n);
int  q_duckdb_buf_free(qd_buf* b);   /* the latched oom, so `b->oom |= q_duckdb_buf_free(&sub)` hands it up */
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
    bool temp;                      /* one of the bridge's TEMP tables: in `temp`, which current_database() never names */
} qd_name_t;

/* the database and schema a bare name binds in (borrowed; "" where DuckDB refused the read), read once and kept
 * until a non-SELECT text runs */
void q_duckdb_ctx(int slot, const char** db, const char** schema);

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

/* The BOUND TABLE: what one metadata read of a table answers, held per connection until a door lets it go
 * (docs/duckdb-architecture.md): every column as the catalog spells and types it, refined by its sidecar row where
 * it has one, and those rows as the read path's descriptors.  OWNED throughout (q_duckdb_schema_bound_free). */
typedef struct {
    char* col;                      /* as the catalog spells it */
    char* dtype;                    /* the catalog's type spelling */
    char  logical[QD_LOGICAL_MAX];  /* the refined logical name; "" where the type has no manifest row */
    bool  iskey;
    bool  has;                      /* the sidecar has this column's row */
    char  t;                        /* the meta char; ' ' where the type has no manifest row */
} qd_bcol_t;
typedef struct {
    char*      key;                 /* qd_name_t.key: the identity */
    qd_bcol_t* col;
    int64_t    ncol;
    qd_desc_t* desc;                /* the sidecar's rows, as the read path refines by them */
    int64_t    ndesc;
    int64_t    at;                  /* bound when (ns) */
    int64_t    icol;                /* the row index column's position, -1 for none (docs/duckdb-architecture.md) */
    int64_t    rows;                /* counted at bind for a q-origin table, NULL_I64 for any other */
    bool       q;                   /* a q-origin table: the sidecar names it */
} qd_bound_t;

/* the connection's bound tables (q_duckdb.c): the entry for a name, read on the first touch and held — BORROWED,
 * good until the next door runs.  NULL with *err owned when the read failed. */
const qd_bound_t* q_duckdb_bound(int slot, const qd_name_t* nm, ray_t** err);

/* ---- q_duckdb_codec.c: the type contract, both directions ---- */

int8_t q_duckdb_codec_surface_of(const qd_colmap_t* cm);
char   q_duckdb_codec_meta_char(const qd_colmap_t* cm);
void   q_duckdb_codec_logical_name(const qd_colmap_t* cm, char* buf, size_t cap);
bool   q_duckdb_codec_parse_logical(const char* s, qd_colmap_t* out);
const qd_tmap_t* q_duckdb_codec_canon_leaf(int8_t ray_type);
bool   q_duckdb_codec_raw_is_text(int8_t ray_type);
bool   q_duckdb_codec_is_rec(const qd_tmap_t* tm);
extern const char QD_NO_SQL_MEANING[];
/* the logical-type ladder over the v2 parameters (q_duckdb_codec.c): the id; the parameter count; parameter i's
 * TYPE (owned) with its name (owned copy, NULL where positional); a flat type built from an id; a vector read as a
 * flat view (false where DuckDB refuses) */
duck_type  q_duckdb_codec_type_id(duck_logical_type lt);
duck_idx_t q_duckdb_codec_type_nparams(duck_logical_type lt);
duck_logical_type q_duckdb_codec_type_param(duck_logical_type lt, duck_idx_t i, char** name);
int64_t q_duckdb_codec_type_param_int(duck_logical_type lt, duck_idx_t i);    /* a width, scale or array size; -1 = none */
char*   q_duckdb_codec_type_param_text(duck_logical_type lt, duck_idx_t i);   /* an ENUM entry, owned */
bool    q_duckdb_codec_type_alias(duck_logical_type lt, char* buf, size_t cap);
duck_logical_type q_duckdb_codec_type_make(int slot, duck_type id);
bool   q_duckdb_codec_vec_view(duck_vector v, duck_view* out);
/* a record type's field count, and field i: its type OWNED, its name OWNED (a MAP's halves have none, so *name is
 * NULL there) */
int    q_duckdb_codec_rec_nfields(duck_logical_type lt, duck_type id);
duck_logical_type q_duckdb_codec_rec_child(duck_logical_type lt, duck_type id, int i, char** name);
void   q_duckdb_codec_map_free(qd_colmap_t* cm);
void   q_duckdb_codec_maps_free(qd_colmap_t* cms, int64_t n);
void   q_duckdb_codec_spell_map(const qd_colmap_t* cm, qd_buf* b);
ray_t* q_duckdb_codec_map_declared(int slot, ray_t* col, duck_logical_type lt, duck_type kind, qd_colmap_t* cm);
bool   q_duckdb_codec_declared_stage_map(duck_logical_type lt, qd_colmap_t* out);
bool   q_duckdb_codec_map_bits(ray_t* col, int depth, qd_colmap_t* cm);

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
bool   q_duckdb_codec_tz_all_zero(ray_t* off);
bool   q_duckdb_codec_zoned_decl(int slot, ray_t* col, qd_buf* dtype, char* logical, size_t cap);
int    q_duckdb_codec_companions_of(const qd_colmap_t* cm, const char* out[QD_CO_MAX]);
ray_t* q_duckdb_codec_stage_image(ray_t* tbl, const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* offs,
                                  ray_t* const* keeps, qd_colmap_t* scms, ray_t** smasks, ray_t** skeeps);
void   q_duckdb_codec_refine(const char* cname, size_t n, const qd_desc_t* desc, int64_t ndesc, qd_colmap_t* cm);
ray_t* q_duckdb_codec_result_to_table(int slot, duck_result* res, const qd_desc_t* desc, int64_t ndesc, ray_t** schema);
/* a q value as the value the write path stores for the one-row column ([] v:enlist v): an atom or string its type, a
 * vector or list a LIST of the codec's element type, a null a NULL of that type; *out owned (value_destroy).
 * 'type for a dict, a table or a byte vector; what the codec refuses ('duckdb, its reason) this refuses */
ray_t* q_duckdb_codec_bind_value(int slot, ray_t* v, duck_value* out);
ray_t* q_duckdb_codec_check_table(int slot, ray_t* tbl, qd_colmap_t* cms);
ray_t* q_duckdb_codec_check_fields(int slot, ray_t* tbl, int64_t c, duck_logical_type lt, const qd_colmap_t* cm);
ray_t* q_duckdb_codec_decay_enums(int slot, ray_t* tbl, qd_enum_t* en);
/* the rows of a q table as ONE column data collection (owned: column_data_collection_destroy), the write path's
 * unit of transfer */
ray_t* q_duckdb_codec_collect(int slot, ray_t* tbl, const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* keeps,
                              duck_collection* out);
ray_t* q_duckdb_codec_strip_companions(int slot, ray_t* tbl, ray_t** masks, ray_t** offs, ray_t** keeps,
                                       bool* iskey, qd_enum_t* en, bool bare);

/* ---- q_duckdb_schema.c: the sidecar, the catalog spellings, the DDL, the envelope's schema ---- */

#define QD_SCHEMA_TBL "_q_schema"     /* the sidecar: a q-written table's declared types, in the database's `main` */
#define QD_STAGE_TBL "_q_stage"       /* the temp table a declared type is staged in, then CAST out of */
#define QD_STAGING_TBL "_q_staging"   /* the reserved temp table `.parquet` stages a q table in; set may name it */
/* q's row index, stored on every q-origin table under the sidecar logical that marks it: the write door lays it dense,
 * the readers hide it, the pull orders by it */
#define QD_INDEX_COL "i"
#define QD_INDEX_LOGICAL "index"
/* what append found of the index on its target: no index to continue (a Duck-origin table), one to continue, or a
 * q-origin table from before the index, to backfill once */
enum { QD_IDX_NONE, QD_IDX_HAS, QD_IDX_BACKFILL };
#define QD_NEED_LOGICAL 1
#define QD_NEED_DTYPE   2

bool        q_duckdb_schema_name_parse(const char* s, size_t n, qd_name_t* out);
void        q_duckdb_schema_name_resolve(int slot, qd_name_t* nm);
void        q_duckdb_schema_put_name(qd_buf* b, const qd_name_t* nm);
ray_t*      q_duckdb_schema_catalog_cols(int slot, const qd_name_t* nm);
bool        q_duckdb_schema_reserved_name(const qd_name_t* nm);
ray_t*      q_duckdb_schema_batch_clear(int slot);   /* no catalog object stands under the batch name */
const char* q_duckdb_schema_text_cell(ray_t* tbl, int64_t col, int64_t row, size_t* len);
const qd_desc_t* q_duckdb_schema_desc_find(const char* name, size_t len, const qd_desc_t* desc, int64_t ndesc);
ray_t*      q_duckdb_schema_check_names(int slot, ray_t* tbl);
int64_t     q_duckdb_schema_fetch_desc(int slot, const qd_name_t* nm, qd_desc_t** out);
void        q_duckdb_schema_desc_free(qd_desc_t* d, int64_t n);
ray_t*      q_duckdb_schema_write_desc(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms,
                                       const bool* iskey, const qd_enum_t* en, const char* const* dtypes, int64_t icol);
ray_t*      q_duckdb_schema_index_backfill(int slot, const qd_name_t* nm);
void        q_duckdb_schema_drop_desc(int slot, const qd_name_t* nm);
ray_t*      q_duckdb_schema_rekey(ray_t* tbl, const qd_desc_t* desc, int64_t ndesc);
bool        q_duckdb_schema_catalog_col(const char* dt, size_t n, qd_colmap_t* out);
void        q_duckdb_schema_create_ddl(qd_buf* b, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms, bool temp);
ray_t*      q_duckdb_schema_check(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms,
                                  const char* const* dtypes, bool* missing, int* index);
int         q_duckdb_schema_needs(const qd_colmap_t* cm, const char* dtype, bool empty);
ray_t*      q_duckdb_schema_desc_of(const char* cname, duck_logical_type lt, const qd_colmap_t* cm,
                                    const qd_desc_t* desc, int64_t ndesc, bool empty, qd_desc_t* out, bool* wanted);
ray_t*      q_duckdb_schema_desc_table(const qd_desc_t* rows, int64_t n);
ray_t*      q_duckdb_schema_desc_parse(int slot, ray_t* schema, qd_desc_t** out, int64_t* n);
ray_t*      q_duckdb_schema_desc_zoned(int slot, ray_t* tbl, ray_t* const* offs, qd_desc_t** out, int64_t* n);
ray_t*      q_duckdb_schema_declare(int slot, ray_t* tbl, qd_colmap_t* cms, qd_desc_t* decl, int64_t ndecl,
                                    const char** dtypes);
ray_t*      q_duckdb_schema_cast_select(int slot, qd_buf* b, ray_t* tbl, const qd_colmap_t* cms, const char* const* dtypes,
                                        ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps, const char* from);
ray_t*      q_duckdb_schema_exact_check(int slot, ray_t* tbl, const qd_colmap_t* cms, const char* const* dtypes,
                                        ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps);
/* the bound table: read in one statement (an owned error on refusal, *out untouched), then what the doors answer from
 * it — kdb's meta; the push's schema `col dtype logical iskey`, () for a table the sidecar does not name; with info,
 * every column and its meta char beside */
ray_t*      q_duckdb_schema_table(ray_t** cols, const char* const* names, int64_t n);   /* owned columns in, released */
ray_t*      q_duckdb_schema_bound_read(int slot, const qd_name_t* nm, bool sidecar, qd_bound_t* out);
void        q_duckdb_schema_bound_free(qd_bound_t* b);
ray_t*      q_duckdb_schema_bound_meta(const qd_bound_t* b);
ray_t*      q_duckdb_schema_bound_schema(const qd_bound_t* b, bool info);

#endif /* Q_DUCKDB_INTERNAL_H */

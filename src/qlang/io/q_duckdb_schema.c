/* q_duckdb_schema — declared-type knowledge: the `_q_schema` descriptor sidecar,
 * the catalog data_type spellings, the CREATE TABLE DDL, append's schema check,
 * and the envelope's schema table (ADR 9a) in both directions — built where the
 * reader has the logical types in hand, honoured by the writer through a CAST.
 * All of it REFINES the physical catalog; none of it is a second source of truth.
 * Contract and decisions: docs/duckdb-api.md. */
#include "qlang/q_count.h"
#include "qlang/io/q_duckdb_internal.h"
#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_types.h"
#include "qlang/base/q_err.h"
#include "qlang/q_prim.h"     /* q_str_text_bytes, q_attr_letter */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>     /* atoi (the DECIMAL scale) */
#include <string.h>

/* ---- _q_schema descriptor sidecar: ONE table per database, rows keyed
 * (tbl;col), shape `tbl col logical iskey valid v dtype attr enumdom` — a
 * REFINEMENT of the physical catalog, never a second source of truth.  `valid`
 * is reserved for the wire arm (always '' here); v = format version 1;
 * append-only, so `dtype` (the declared type where not the carrier's canonical
 * one) and `attr`/`enumdom` (what q keeps beyond the value: the attribute
 * letter and the enum domain name, ADR 8 — WRITTEN here, never re-applied by
 * `get`) are ADDed to a sidecar that predates them, and readers ignore columns
 * they do not know.  One row per table has col = '' (the table itself) and
 * logical 'table': the MARK of a q-sourced table (2026-09-14), written in the
 * same statement as the column rows and read by the same SELECT; no column of
 * its own yet, so when it was written and by whom are not recorded. */
#define QD_SCHEMA_TABLE_ROW "table"

#define QD_SCHEMA_TBL "_q_schema"
#define QD_SCHEMA_REF "\"main\".\"" QD_SCHEMA_TBL "\""   /* pinned to DuckDB's default schema, whatever the session's */

static void qd_spell_canon(const qd_colmap_t* cm, qd_buf* b);

/* ---- THE table-name grammar (docs/duckdb-api.md, Table names): a symbol with NO double quote is one exact
 * name, dots and all; a symbol with one is SQL identifier syntax.  Every spelling a door needs — the SQL text,
 * the appender's parts, the catalog predicate, the sidecar key — is read off this ONE parse. ---- */

static bool qd_bare_ident(unsigned char c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

static bool qd_name_part(qd_name_t* out, const char* s, size_t len) {
    if (out->n >= QD_NAME_PARTS || len == 0 || len >= sizeof out->part[0]) return false;
    memcpy(out->part[out->n], s, len);
    out->part[out->n++][len] = '\0';
    return true;
}

bool q_duckdb_schema_name_parse(const char* s, size_t n, qd_name_t* out) {
    out->n = 0;
    if (n == 0) return false;
    if (!memchr(s, '"', n)) {
        if (!qd_name_part(out, s, n)) return false;
    } else {
        for (size_t i = 0;;) {
            if (s[i] == '"') {
                char un[256];
                size_t k = 0;
                for (i++; i < n; i++) {
                    if (s[i] != '"') { if (k + 1 >= sizeof un) return false; un[k++] = s[i]; continue; }
                    if (i + 1 < n && s[i + 1] == '"') { if (k + 1 >= sizeof un) return false; un[k++] = '"'; i++; }
                    else break;
                }
                if (i >= n || !qd_name_part(out, un, k)) return false;   /* unterminated, or empty/too long */
                i++;
            } else {
                size_t b = i;
                while (i < n && qd_bare_ident((unsigned char)s[i])) i++;
                if (!qd_name_part(out, s + b, i - b)) return false;
            }
            if (i == n) break;
            if (s[i] != '.') return false;
            if (++i == n) return false;
        }
    }
    out->key[0] = '\0';
    out->temp = out->n == 1 && (strcmp(out->part[0], QD_STAGE_TBL) == 0 || strcmp(out->part[0], QD_STAGING_TBL) == 0);
    return true;
}

static void qd_one_cell(ray_t* row, int64_t col, char* buf, size_t cap) {
    size_t n = 0;
    const char* p = row && row->type == RAY_TABLE && q_count(row) == 1
                        ? q_duckdb_schema_text_cell(ray_table_get_col_idx(row, col), 0, &n) : NULL;
    snprintf(buf, cap, "%.*s", (int)(p ? n : 0), p ? p : "");
}

/* DuckDB's identifier fold is ASCII-only (probed 2026-09-14: "ÄRGER" finds "Ärger", "ärger" does not); spelled
 * out rather than tolower so no locale can widen it */
static char qd_fold_c(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static void qd_fold(char* s) { for (; *s; s++) *s = qd_fold_c(*s); }
static bool qd_fold_eq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) if (qd_fold_c(a[i]) != qd_fold_c(b[i])) return false;
    return true;
}

/* THE key home: the name as DuckDB resolves it (a bare name in the session's schema), folded, with the session's
 * database and the default schema `main` implied — so `t, "main"."t" and "T" key one row and pre-2026-09-14 rows
 * still match; the parts stay as written, they are the address the SQL uses */
void q_duckdb_schema_name_resolve(int slot, qd_name_t* nm) {
    duck_result res;
    char db[256] = "";
    nm->schema[0] = '\0';
    if (!q_duckdb_run2(slot, "SELECT current_database() AS d, current_schema() AS s", &res, 0)) {
        ray_t* row = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
        QAPI.destroy_result(&res);
        qd_one_cell(row, 0, db, sizeof db);
        qd_one_cell(row, 1, nm->schema, sizeof nm->schema);
        if (row && !RAY_IS_ERR(row)) ray_release(row);
    }
    if (nm->n >= 2) memcpy(nm->schema, nm->part[nm->n - 2], strlen(nm->part[nm->n - 2]) + 1);
    if (nm->temp) memcpy(nm->schema, "main", 5);   /* the temp catalog has the one schema, whatever the session's */
    qd_buf b = {0};
    if (nm->n == 3 && !(strlen(nm->part[0]) == strlen(db) && qd_fold_eq(nm->part[0], db, strlen(db)))) {
        q_duckdb_put_ident(&b, nm->part[0], strlen(nm->part[0]));
        q_duckdb_puts(&b, ".");
    }
    if (b.len || !(strlen(nm->schema) == 4 && qd_fold_eq(nm->schema, "main", 4))) {
        q_duckdb_put_ident(&b, nm->schema, strlen(nm->schema));
        q_duckdb_puts(&b, ".");
        q_duckdb_put_ident(&b, nm->part[nm->n - 1], strlen(nm->part[nm->n - 1]));
    } else q_duckdb_puts(&b, nm->part[nm->n - 1]);
    if (!b.oom && b.len < sizeof nm->key) { memcpy(nm->key, b.p, b.len + 1); qd_fold(nm->key); }
    else nm->key[0] = '\0';
    q_duckdb_buf_free(&b);
}

void q_duckdb_schema_put_name(qd_buf* b, const qd_name_t* nm) {
    for (int i = 0; i < nm->n; i++) {
        if (i) q_duckdb_puts(b, ".");
        q_duckdb_put_ident(b, nm->part[i], strlen(nm->part[i]));
    }
}

/* The catalog predicate for one name: the last part is the table, the parts before it schema then database.
 * A one-part name pins the session's database and schema, which is where `SELECT * FROM "t"` resolves it —
 * without that, a same-named table in another schema doubles the columns the caller sees.  The bridge's own temp
 * tables pin `temp` instead: a bare name reaches it first, and current_database() never names it. */
void q_duckdb_schema_put_catalog_where(qd_buf* b, const qd_name_t* nm) {
    static const char* const col[QD_NAME_PARTS] = { "database_name", "schema_name", "table_name" };
    if (nm->n == 1) q_duckdb_puts(b, nm->temp ? "database_name = 'temp' AND schema_name = 'main' AND "
                                              : "database_name = current_database() AND schema_name = current_schema() AND ");
    for (int i = 0; i < nm->n; i++) {
        if (i) q_duckdb_puts(b, " AND ");
        q_duckdb_puts(b, col[QD_NAME_PARTS - nm->n + i]);
        q_duckdb_puts(b, " = ");
        q_duckdb_put_strlit(b, nm->part[i], strlen(nm->part[i]));
    }
}

bool q_duckdb_schema_reserved_name(const qd_name_t* nm) {
    const char* t = nm->part[nm->n - 1];
    return strcmp(t, QD_SCHEMA_TBL) == 0 || strcmp(t, QD_STAGE_TBL) == 0;
}

const qd_desc_t* q_duckdb_schema_desc_find(const char* name, size_t len, const qd_desc_t* desc, int64_t ndesc) {
    for (int64_t i = 0; i < ndesc; i++)
        if (strlen(desc[i].col) == len && qd_fold_eq(desc[i].col, name, len)) return &desc[i];
    return NULL;
}

/* a row's two owned strings; false = OOM, the row left empty */
static bool qd_desc_set(qd_desc_t* d, const char* col, size_t cl, const char* dtype, size_t dl) {
    d->col   = q_duckdb_text(col, cl);
    d->dtype = q_duckdb_text(dtype, dl);
    if (d->col && d->dtype) return true;
    free(d->col);
    free(d->dtype);
    d->col = d->dtype = NULL;
    return false;
}

void q_duckdb_schema_desc_free(qd_desc_t* d, int64_t n) {
    for (int64_t i = 0; i < n; i++) { free(d[i].col); free(d[i].dtype); }
    free(d);
}

const char* q_duckdb_schema_text_cell(ray_t* col, int64_t i, size_t* len) {
    if (!col || col->type != RAY_LIST) return NULL;
    void* pp = ray_vec_get(col, i);
    ray_t* cell = pp ? *(ray_t**)pp : NULL;
    if (!cell || cell->type != RAY_CHARV) return NULL;
    *len = (size_t) q_count(cell);
    return (const char*)ray_data(cell);
}

/* Fetch a table's descriptor rows into *out (owned, q_duckdb_schema_desc_free); ANY sidecar trouble degrades to 0 rows. */
int64_t q_duckdb_schema_fetch_desc(int slot, const qd_name_t* nm, qd_desc_t** out) {
    *out = NULL;
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT col, logical, iskey FROM " QD_SCHEMA_REF " WHERE tbl = ");
    q_duckdb_put_strlit(&b, nm->key, strlen(nm->key));
    if (b.oom) { q_duckdb_buf_free(&b); return 0; }
    duck_result res;
    size_t mark = q_duckdb_err_mark();
    ray_t* e = q_duckdb_run2(slot, b.p, &res, 0);   /* probe: no-sidecar is normal */
    q_duckdb_buf_free(&b);
    if (e) { ray_error_free(e); return 0; }
    ray_t* rows = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
    QAPI.destroy_result(&res);
    if (!rows || RAY_IS_ERR(rows) || rows->type != RAY_TABLE) {
        q_duckdb_drop(rows);
        q_duckdb_err_rewind(slot, mark);
        return 0;
    }
    int64_t    n = q_count(rows);
    qd_desc_t* d = n ? calloc((size_t)n, sizeof *d) : NULL;
    if (!d) n = 0;
    ray_t* cv = ray_table_get_col_idx(rows, 0);   /* borrowed text col */
    ray_t* lv = ray_table_get_col_idx(rows, 1);
    ray_t* kv = ray_table_get_col_idx(rows, 2);   /* borrowed BOOL */
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++) {
        size_t cl = 0, ll = 0;
        const char* cn = q_duckdb_schema_text_cell(cv, i, &cl);
        const char* ln = q_duckdb_schema_text_cell(lv, i, &ll);
        if (!cl) continue;   /* the table's own row; no column has that name (DuckDB refuses a zero-length identifier) */
        if (!qd_desc_set(&d[k], cn ? cn : "", cl, "", 0)) {
            q_duckdb_schema_desc_free(d, k);
            d = NULL;
            k = 0;
            break;
        }
        snprintf(d[k].logical, sizeof d[k].logical, "%.*s", (int)ll, ln ? ln : "");
        d[k].iskey = kv && kv->type == RAY_BOOL && *(uint8_t*)ray_vec_get(kv, i) != 0;
        k++;
    }
    ray_release(rows);
    *out = d;
    return k;
}

/* One descriptor row to write (name and enumdom are NOT NUL-terminated: ptr+len). */
typedef struct {
    const char* name;
    size_t      namelen;
    char        logical[96];
    bool        iskey;
    const char* dtype;
    char        attr[2];       /* the q attribute letter, "" = none */
    const char* enumdom;
    size_t      domlen;
} qd_descrow_t;

/* THE one spelling of "this table's descriptor rows, gone"; stash = 0 is the probe form (no sidecar is no rows). */
static ray_t* qd_desc_rows_delete(int slot, const char* tname, int stash) {
    qd_buf b = {0};
    q_duckdb_puts(&b, "DELETE FROM " QD_SCHEMA_REF " WHERE tbl = ");
    q_duckdb_put_strlit(&b, tname, strlen(tname));
    if (b.oom) { q_duckdb_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = q_duckdb_run2(slot, b.p, &res, stash);
    q_duckdb_buf_free(&b);
    if (!e) QAPI.destroy_result(&res);
    return e;
}

/* Drop a table's descriptor rows with the table (hdel): a catalog with no sidecar yet has none to drop. */
void q_duckdb_schema_drop_desc(int slot, const qd_name_t* nm) {
    ray_t* e = qd_desc_rows_delete(slot, nm->key, 0);
    if (e) ray_error_free(e);
}

/* Replace a table's descriptor rows (runs INSIDE the caller's transaction). */
static ray_t* qd_write_desc_rows(int slot, const char* tname,
                                 const qd_descrow_t* rows, int64_t n) {
    ray_t* e = q_duckdb_exec_stmt(slot,
        "CREATE TABLE IF NOT EXISTS " QD_SCHEMA_REF "("
        "tbl VARCHAR, col VARCHAR, logical VARCHAR, "
        "iskey BOOLEAN, valid VARCHAR, v BIGINT, dtype VARCHAR, attr VARCHAR, enumdom VARCHAR)");
    if (!e) e = q_duckdb_exec_stmt(slot, "ALTER TABLE " QD_SCHEMA_REF " ADD COLUMN IF NOT EXISTS dtype VARCHAR");
    if (!e) e = q_duckdb_exec_stmt(slot, "ALTER TABLE " QD_SCHEMA_REF " ADD COLUMN IF NOT EXISTS attr VARCHAR");
    if (!e) e = q_duckdb_exec_stmt(slot, "ALTER TABLE " QD_SCHEMA_REF " ADD COLUMN IF NOT EXISTS enumdom VARCHAR");
    if (!e) e = qd_desc_rows_delete(slot, tname, 1);
    if (e) return e;

    qd_buf ins = {0};
    q_duckdb_puts(&ins, "INSERT INTO " QD_SCHEMA_REF
                  "(tbl, col, logical, iskey, valid, v, dtype, attr, enumdom) VALUES (");
    q_duckdb_put_strlit(&ins, tname, strlen(tname));
    q_duckdb_puts(&ins, ", '', '" QD_SCHEMA_TABLE_ROW "', FALSE, '', 1, '', '', '')");
    for (int64_t i = 0; i < n; i++) {
        q_duckdb_puts(&ins, ", (");
        q_duckdb_put_strlit(&ins, tname, strlen(tname));
        q_duckdb_puts(&ins, ", ");
        q_duckdb_put_strlit(&ins, rows[i].name, rows[i].namelen);
        q_duckdb_puts(&ins, ", ");
        q_duckdb_put_strlit(&ins, rows[i].logical, strlen(rows[i].logical));
        q_duckdb_puts(&ins, rows[i].iskey ? ", TRUE, '', 1, " : ", FALSE, '', 1, ");
        q_duckdb_put_strlit(&ins, rows[i].dtype, strlen(rows[i].dtype));
        q_duckdb_puts(&ins, ", ");
        q_duckdb_put_strlit(&ins, rows[i].attr, strlen(rows[i].attr));
        q_duckdb_puts(&ins, ", ");
        q_duckdb_put_strlit(&ins, rows[i].enumdom, rows[i].domlen);
        q_duckdb_puts(&ins, ")");
    }
    if (ins.oom) { q_duckdb_buf_free(&ins); return q_err(QE_WSFULL); }
    e = q_duckdb_exec_stmt(slot, ins.p);
    q_duckdb_buf_free(&ins);
    return e;
}

/* Every column gets a row — identity refinements included (set path); dtypes[c] = the declared type where it is
 * not the carrier's canonical one, NULL otherwise.  `attr` and `enumdom` are RECORDED, never re-applied (ADR 8):
 * off the column itself, or off en[c] for a column the door decayed from an enum. */
ray_t* q_duckdb_schema_write_desc(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms,
                                  const bool* iskey, const qd_enum_t* en, const char* const* dtypes) {
    int64_t ncols = ray_table_ncols(tbl);
    qd_descrow_t* rows = q_duckdb_cols(ncols, sizeof *rows);
    if (!rows) return q_err(QE_WSFULL);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);            /* borrowed */
        ray_t* cn  = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        ray_t* dn  = en[c].dom ? ray_sym_str(en[c].dom) : NULL;   /* borrowed */
        char*  fold = q_duckdb_text(cn ? ray_str_ptr(cn) : "?", cn ? ray_str_len(cn) : 1);
        if (!fold) { for (int64_t k = 0; k < c; k++) free((char*)rows[k].name); free(rows); return q_err(QE_WSFULL); }
        qd_fold(fold);
        rows[c].name    = fold;
        rows[c].namelen = strlen(fold);
        rows[c].iskey   = iskey[c];
        rows[c].dtype   = dtypes[c] ? dtypes[c] : "";
        rows[c].attr[0] = en[c].dom ? en[c].attr : q_attr_letter(col);
        rows[c].attr[1] = '\0';
        rows[c].enumdom = dn ? ray_str_ptr(dn) : "";
        rows[c].domlen  = dn ? (size_t)ray_str_len(dn) : 0;
        q_duckdb_codec_logical_name(&cms[c], rows[c].logical, sizeof rows[c].logical);
    }
    ray_t* e = qd_write_desc_rows(slot, nm->key, rows, ncols);
    for (int64_t c = 0; c < ncols; c++) free((char*)rows[c].name);
    free(rows);
    return e;
}

/* two q columns DuckDB reads as one name: refused before any DDL, naming both, where DuckDB would name one */
ray_t* q_duckdb_schema_check_names(int slot, ray_t* tbl) {
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t a = 0; a < ncols; a++)
        for (int64_t b = a + 1; b < ncols; b++) {
            ray_t* x = ray_sym_str(ray_table_col_name(tbl, a));   /* borrowed */
            ray_t* y = ray_sym_str(ray_table_col_name(tbl, b));
            if (!x || !y || ray_str_len(x) != ray_str_len(y) ||
                !qd_fold_eq(ray_str_ptr(x), ray_str_ptr(y), ray_str_len(x))) continue;
            q_duckdb_err_stash(slot, "columns %.*s and %.*s are one column to DuckDB", (int)ray_str_len(x),
                               ray_str_ptr(x), (int)ray_str_len(y), ray_str_ptr(y));
            return q_err(QE_DUCKDB);
        }
    return NULL;
}

/* Rebuild the keyed shape from VALIDATING iskey rows (keyedness has no
 * physical truth — no DuckDB PRIMARY KEY); all-key/no-key stay plain.
 * Consumes tbl. */
ray_t* q_duckdb_schema_rekey(ray_t* tbl, const qd_desc_t* desc, int64_t ndesc) {
    if (!tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    bool*   iskey = q_duckdb_cols(ncols, sizeof *iskey);
    if (!iskey) { ray_release(tbl); return q_err(QE_WSFULL); }
    int64_t nkey = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm  = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        ray_t* col = ray_table_get_col_idx(tbl, c);             /* borrowed */
        const qd_desc_t* d = nm && col ? q_duckdb_schema_desc_find(ray_str_ptr(nm), ray_str_len(nm), desc, ndesc) : NULL;
        qd_colmap_t refined;
        iskey[c] = d && d->iskey && q_duckdb_codec_parse_logical(d->logical, &refined) &&
                   q_duckdb_codec_surface_of(&refined) == col->type;
        if (iskey[c]) nkey++;
    }
    if (nkey == 0 || nkey == ncols) { free(iskey); return tbl; }
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(ncols - nkey);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t** dst = iskey[c] ? &kt : &vt;
        *dst = ray_table_add_col(*dst, ray_table_col_name(tbl, c),
                                 ray_table_get_col_idx(tbl, c));  /* retains */
    }
    free(iskey);
    ray_release(tbl);
    if (!kt || RAY_IS_ERR(kt) || !vt || RAY_IS_ERR(vt)) {
        q_duckdb_drop(kt);
        q_duckdb_drop(vt);
        return q_err(QE_WSFULL);
    }
    return ray_dict_new(kt, vt);   /* consumes both */
}
/* Catalog data_type string ('[]' / '[n]' suffixes = LIST / fixed ARRAY levels, base name before '(')
 * -> column map — THE one owner of catalog spellings, meta and schema-check
 * both ride it. */
bool q_duckdb_schema_catalog_col(const char* dt, size_t n, qd_colmap_t* out) {
    int depth = 0;
    while (n >= 2 && dt[n - 1] == ']' && depth < QD_MAX_DEPTH) {   /* [] or [n] */
        size_t k = n - 1;
        while (k > 0 && isdigit((unsigned char)dt[k - 1])) k--;
        if (k == 0 || dt[k - 1] != '[') break;
        n = k - 1;
        depth++;
    }
    size_t base = 0;
    while (base < n && dt[base] != '(') base++;
    for (size_t i = 0; i < QD_NTYPES; i++) {
        if (!QD_TYPES[i].read_canon) continue;
        if (strlen(QD_TYPES[i].sql) == base &&
            strncmp(QD_TYPES[i].sql, dt, base) == 0) {
            out->leaf  = &QD_TYPES[i];
            out->rec   = NULL;
            out->depth = depth;
            out->undet = false;
            return true;
        }
    }
    return false;
}
/* The store table's CREATE OR REPLACE spelling: one column per map, `[]` per LIST level; temp = the staging table. */
void q_duckdb_schema_create_ddl(qd_buf* b, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms, bool temp) {
    int64_t ncols = ray_table_ncols(tbl);
    q_duckdb_puts(b, temp ? "CREATE OR REPLACE TEMP TABLE " : "CREATE OR REPLACE TABLE ");
    q_duckdb_schema_put_name(b, nm);
    q_duckdb_puts(b, "(");
    for (int64_t c = 0; c < ncols; c++) {
        if (c) q_duckdb_puts(b, ", ");
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        q_duckdb_put_ident(b, nm ? ray_str_ptr(nm) : "?", nm ? ray_str_len(nm) : 1);
        q_duckdb_puts(b, " ");
        q_duckdb_codec_spell_map(&cms[c], b);
    }
    q_duckdb_puts(b, ")");
}

/* The one column the two sides do not share, named: an extra column the table lacks, else one the batch omits. */
static ray_t* qd_column_set_fail(int slot, ray_t* tbl, ray_t* names, int64_t nrows) {
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* qn = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        bool seen = false;
        for (int64_t i = 0; qn && i < nrows && !seen; i++) {
            size_t nl = 0;
            const char* cn = q_duckdb_schema_text_cell(names, i, &nl);
            seen = cn && nl == ray_str_len(qn) && memcmp(cn, ray_str_ptr(qn), nl) == 0;
        }
        if (!seen && qn) {
            char what[300];
            snprintf(what, sizeof what, "column %.*s", (int)ray_str_len(qn), ray_str_ptr(qn));
            return q_duckdb_fail(slot, what, "the table has no such column");
        }
    }
    for (int64_t i = 0; i < nrows; i++) {
        size_t nl = 0;
        const char* cn = q_duckdb_schema_text_cell(names, i, &nl);
        int64_t id = ray_sym_intern_runtime(cn ? cn : "", nl);
        bool seen = false;
        for (int64_t c = 0; c < ncols && !seen; c++) seen = ray_table_col_name(tbl, c) == id;
        if (!seen) {
            char what[300];
            snprintf(what, sizeof what, "column %.*s", (int)nl, cn ? cn : "");
            return q_duckdb_fail(slot, what, "the appended table does not carry it");
        }
    }
    return q_err(QE_DUCKDB);
}

/* Append's schema check: names, order and canonical types must match the
 * catalog — a DECLARED type (dtypes[c]) must match the catalog's spelling
 * verbatim instead; a VALID descriptor row pins the LOGICAL type too (a string
 * column can't append into a symbol column).  `missing` ASKS instead of refusing
 * when the table is not there at all — the create-on-first-append law. */
ray_t* q_duckdb_schema_check(int slot, const qd_name_t* nm, ray_t* tbl,
                             const qd_colmap_t* cms, const char* const* dtypes, bool* missing) {
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT column_name, data_type FROM duckdb_columns() WHERE ");
    q_duckdb_schema_put_catalog_where(&b, nm);
    q_duckdb_puts(&b, " ORDER BY column_index");
    if (b.oom) { q_duckdb_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (e) return e;
    ray_t* cat = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
    QAPI.destroy_result(&res);
    if (!cat || RAY_IS_ERR(cat)) return cat;

    int64_t nrows = q_count(cat);
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* names  = ray_table_get_col_idx(cat, 0);   /* borrowed text cols */
    if (nrows == 0 || nrows != ncols) {
        ray_t* f = nrows ? qd_column_set_fail(slot, tbl, names, nrows) : NULL;
        ray_release(cat);
        if (nrows) return f;
        if (missing) { *missing = true; return NULL; }
        return q_err(QE_DUCKDB);
    }
    ray_t* catdt  = ray_table_get_col_idx(cat, 1);
    qd_desc_t* desc  = NULL;
    int64_t    ndesc = q_duckdb_schema_fetch_desc(slot, nm, &desc);
    for (int64_t c = 0; !e && c < ncols; c++) {
        size_t nl = 0, dl = 0;
        const char* nm = q_duckdb_schema_text_cell(names, c, &nl);
        const char* dt = q_duckdb_schema_text_cell(catdt, c, &dl);
        ray_t* qn = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        size_t ql = qn ? ray_str_len(qn) : 0;
        if (!nm || !qn || ql != nl || !qd_fold_eq(nm, ray_str_ptr(qn), nl)) {
            char what[300], why[300];
            snprintf(what, sizeof what, "column %.*s", (int)ql, qn ? ray_str_ptr(qn) : "?");
            snprintf(why, sizeof why, "the table holds %.*s in that position", (int)nl, nm ? nm : "");
            e = q_duckdb_fail(slot, what, why);
            continue;
        }
        qd_colmap_t cr;
        bool known = q_duckdb_schema_catalog_col(dt ? dt : "", dl, &cr);
        if (dtypes[c] ? strlen(dtypes[c]) != dl || memcmp(dtypes[c], dt, dl) != 0
                      : !known || cr.leaf->dk_type != cms[c].leaf->dk_type || cr.depth != cms[c].depth) {
            char what[300], why[600];
            qd_buf canon = {0};
            qd_spell_canon(&cms[c], &canon);
            snprintf(what, sizeof what, "column %.*s", (int)nl, nm);
            snprintf(why, sizeof why, "%s %s, the table holds %.*s", dtypes[c] ? "declared" : "the q type derives",
                     dtypes[c] ? dtypes[c] : QD_TEXT(canon.p), (int)dl, dt ? dt : "");
            q_duckdb_buf_free(&canon);
            e = q_duckdb_fail(slot, what, why);
            continue;
        }
        const qd_desc_t* d = known ? q_duckdb_schema_desc_find(nm, nl, desc, ndesc) : NULL;
        qd_colmap_t refined;
        if (d && q_duckdb_codec_parse_logical(d->logical, &refined) &&
            refined.leaf->dk_type == cr.leaf->dk_type &&
            refined.depth == cr.depth &&
            (refined.leaf != cms[c].leaf || refined.depth != cms[c].depth)) {
            char what[300], why[300], have[QD_LOGICAL_MAX];
            q_duckdb_codec_logical_name(&cms[c], have, sizeof have);
            snprintf(what, sizeof what, "column %.*s", (int)nl, nm);
            snprintf(why, sizeof why, "the q column is %s, the table's row says %s", have, d->logical);
            e = q_duckdb_fail(slot, what, why);
        }
    }
    q_duckdb_schema_desc_free(desc, ndesc);
    ray_release(cat);
    return e;
}

/* ---- the envelope (ADR 9a): `(data;schema)`, schema = one row per column that NEEDS one, columns
 * `col dtype logical iskey`.  dtype is DuckDB's own declaration spelling; a row exists only where that is not
 * the canonical type of the column's q carrier, or where the logical is a non-canon refinement (symbol, month…),
 * so an EMPTY schema means the bare table is enough. ---- */

/* The declaration spelling of a logical type, as `typeof`/DESCRIBE print it: an alias (JSON) wins outright,
 * LIST/ARRAY suffix their child, DECIMAL and ENUM carry their parameters, everything else is its bare name. */
void q_duckdb_schema_spell_type(duck_logical_type lt, qd_buf* b) {
    char* alias = QAPI.logical_type_get_alias(lt);
    if (alias) { q_duckdb_puts(b, alias); QAPI.duck_free(alias); return; }
    duck_type id = QAPI.get_type_id(lt);
    char num[48];
    duck_logical_type child = NULL;
    switch (id) {
        case QDUCK_TYPE_LIST:
        case QDUCK_TYPE_ARRAY:
            child = id == QDUCK_TYPE_LIST ? QAPI.list_type_child_type(lt) : QAPI.array_type_child_type(lt);
            if (child) { q_duckdb_schema_spell_type(child, b); QAPI.destroy_logical_type(&child); }
            if (id == QDUCK_TYPE_LIST) q_duckdb_puts(b, "[]");
            else { snprintf(num, sizeof num, "[%llu]", (unsigned long long)QAPI.array_type_array_size(lt)); q_duckdb_puts(b, num); }
            return;
        case QDUCK_TYPE_DECIMAL:
            snprintf(num, sizeof num, "DECIMAL(%u,%u)", QAPI.decimal_width(lt), QAPI.decimal_scale(lt));
            q_duckdb_puts(b, num);
            return;
        case QDUCK_TYPE_STRUCT:
        case QDUCK_TYPE_MAP:
        case QDUCK_TYPE_UNION: {   /* ADR 12's three records: a MAP declares two types, the others named fields */
            bool named = id != QDUCK_TYPE_MAP;
            uint32_t n = id == QDUCK_TYPE_UNION ? (uint32_t)QAPI.union_type_member_count(lt)
                       : named                  ? (uint32_t)QAPI.struct_type_child_count(lt) : 2;
            q_duckdb_puts(b, q_duckdb_type_name(id));
            q_duckdb_puts(b, "(");
            for (uint32_t i = 0; i < n; i++) {
                char* nm = !named ? NULL
                         : id == QDUCK_TYPE_UNION ? QAPI.union_type_member_name(lt, i)
                                                  : QAPI.struct_type_child_name(lt, i);
                child = !named ? (i ? QAPI.map_type_value_type(lt) : QAPI.map_type_key_type(lt))
                      : id == QDUCK_TYPE_UNION ? QAPI.union_type_member_type(lt, i)
                                               : QAPI.struct_type_child_type(lt, i);
                if (i) q_duckdb_puts(b, ", ");
                if (nm) { q_duckdb_put_ident(b, nm, strlen(nm)); q_duckdb_puts(b, " "); QAPI.duck_free(nm); }
                if (child) { q_duckdb_schema_spell_type(child, b); QAPI.destroy_logical_type(&child); }
            }
            q_duckdb_puts(b, ")");
            return;
        }
        case QDUCK_TYPE_ENUM: {
            uint32_t n = QAPI.enum_dictionary_size(lt);
            q_duckdb_puts(b, "ENUM(");
            for (uint32_t i = 0; i < n; i++) {
                char* v = QAPI.enum_dictionary_value(lt, i);
                if (i) q_duckdb_puts(b, ", ");
                q_duckdb_put_strlit(b, QD_TEXT(v), strlen(QD_TEXT(v)));
                if (v) QAPI.duck_free(v);
            }
            q_duckdb_puts(b, ")");
            return;
        }
        default:
            q_duckdb_puts(b, q_duckdb_type_name(id));
    }
}

/* the canonical spelling for a column map: what a write derives from the q carrier alone — a record's fields
 * are its own, since no other q value spells them */
static void qd_spell_canon(const qd_colmap_t* cm, qd_buf* b) {
    if (cm->rec) { q_duckdb_codec_spell_map(cm, b); return; }
    q_duckdb_puts(b, q_duckdb_type_name(q_duckdb_codec_canon_leaf(cm->leaf->ray_type)->dk_type));
    for (int l = 0; l < cm->depth; l++) q_duckdb_puts(b, "[]");
}

/* THE one home of "does this column need a schema row": QD_NEED_LOGICAL where the refinement is a non-canon row,
 * QD_NEED_DTYPE where the declared spelling is not the carrier's canonical one.  `.duckdb.types[]` reports it
 * per manifest row; the reader decides per result column with it.  `empty` is the column's own emptiness, which
 * only a reader that has fetched its rows knows — hence asked, not derived. */
int q_duckdb_schema_needs(const qd_colmap_t* cm, const char* dtype, bool empty) {
    /* ADR 12: one dict carrier, three spellings — a record is reversible only through its declaration */
    if (q_duckdb_codec_is_rec(cm->leaf)) return QD_NEED_DTYPE;
    /* ADR 15: emptied, a 0h column is the bare (), which spells nothing — the row is the only thing left that can */
    if (empty && q_duckdb_codec_surface_of(cm) == RAY_LIST) return QD_NEED_DTYPE;
    qd_buf canon = {0};
    qd_spell_canon(cm, &canon);
    int need = (cm->leaf->read_canon ? 0 : QD_NEED_LOGICAL) |
               (canon.oom || strcmp(canon.p, dtype) != 0 ? QD_NEED_DTYPE : 0);
    q_duckdb_buf_free(&canon);
    return need;
}

/* One result column's schema row into *out when it needs one (*wanted), iskey from its _q_schema row. */
ray_t* q_duckdb_schema_desc_of(const char* cname, duck_logical_type lt, const qd_colmap_t* cm,
                               const qd_desc_t* desc, int64_t ndesc, bool empty, qd_desc_t* out, bool* wanted) {
    qd_buf sp = {0};
    q_duckdb_schema_spell_type(lt, &sp);
    if (sp.oom) { q_duckdb_buf_free(&sp); return q_err(QE_WSFULL); }
    *wanted = q_duckdb_schema_needs(cm, sp.p, empty) != 0;
    /* filled either way: the reader only learns a streamed column is empty after the fetch, and by then `lt` is
     * gone, so the spelling has to already be here for it to change its mind */
    bool ok = qd_desc_set(out, cname, strlen(cname), sp.p, sp.len);
    if (ok) {
        const qd_desc_t* d = q_duckdb_schema_desc_find(cname, strlen(cname), desc, ndesc);
        q_duckdb_codec_logical_name(cm, out->logical, sizeof out->logical);
        out->iskey = d && d->iskey;
    }
    q_duckdb_buf_free(&sp);
    return ok ? NULL : q_err(QE_WSFULL);
}

static ray_t* qd_desc_release(ray_t* a, ray_t* b, ray_t* c, ray_t* d, ray_t* err) {
    if (a != err) q_duckdb_drop(a);
    if (b != err) q_duckdb_drop(b);
    if (c != err) q_duckdb_drop(c);
    if (d != err) q_duckdb_drop(d);
    return err;
}

static ray_t* qd_first_bad(ray_t* a, ray_t* b, ray_t* c, ray_t* d) {
    ray_t* v[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) if (!v[i] || RAY_IS_ERR(v[i])) return v[i] ? v[i] : q_err(QE_WSFULL);
    return NULL;
}

/* rows -> the schema table `col dtype logical iskey` (an empty one for n = 0) */
ray_t* q_duckdb_schema_desc_table(const qd_desc_t* rows, int64_t n) {
    ray_t* col = ray_sym_vec_new(RAY_SYM_W64, n ? n : 1);
    ray_t* dt  = ray_list_new(n ? n : 1);
    ray_t* lg  = ray_sym_vec_new(RAY_SYM_W64, n ? n : 1);
    ray_t* ik  = ray_vec_new(RAY_BOOL, n ? n : 1);
    ray_t* err = qd_first_bad(col, dt, lg, ik);
    for (int64_t i = 0; i < n && !err; i++) {
        int64_t cid = ray_sym_intern_runtime(rows[i].col, strlen(rows[i].col));
        int64_t lid = ray_sym_intern_runtime(rows[i].logical, strlen(rows[i].logical));
        uint8_t k   = rows[i].iskey;
        ray_t* cell = ray_charv(rows[i].dtype, (int64_t)strlen(rows[i].dtype));
        if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : q_err(QE_WSFULL); break; }
        col = ray_vec_append(col, &cid);
        dt  = ray_list_append(dt, cell);
        ray_release(cell);
        lg  = ray_vec_append(lg, &lid);
        ik  = ray_vec_append(ik, &k);
        err = qd_first_bad(col, dt, lg, ik);
    }
    if (err) return qd_desc_release(col, dt, lg, ik, err);
    ray_t* tbl = ray_table_new(4);
    if (tbl && !RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("col", 3), col);
    if (tbl && !RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("dtype", 5), dt);
    if (tbl && !RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("logical", 7), lg);
    if (tbl && !RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("iskey", 5), ik);
    qd_desc_release(col, dt, lg, ik, NULL);   /* add_col retains its own refs */
    return tbl ? tbl : q_err(QE_WSFULL);
}

/* a schema cell as text: a symbol vector's cell, or a list cell that is a string or a symbol atom */
static bool qd_schema_text(ray_t* colv, int64_t i, const char** p, int64_t* n) {
    ray_t* s = NULL;
    if (colv->type == RAY_SYM) s = ray_sym_vec_cell(colv, i);   /* borrowed */
    else {
        if (colv->type != RAY_LIST) return false;
        ray_t* cell = ray_list_get(colv, i);
        if (cell && cell->type != -RAY_SYM) return q_str_text_bytes(cell, p, n);
        s = cell ? ray_sym_str(cell->i64) : NULL;
    }
    *p = s ? ray_str_ptr(s) : "";
    *n = s ? (int64_t)ray_str_len(s) : 0;
    return true;
}

static ray_t* qd_schema_fail(int slot, const char* why) { return q_duckdb_fail(slot, "envelope", why); }

/* A declared dtype is spliced into a CAST verbatim, so it must be ONE type declaration and nothing else: a name,
 * balanced (…) parameters and […] suffixes, and inside them quoted literals (ENUM('a', 'b')) or quoted field
 * names (STRUCT("Bird Name" INTEGER)) — never a quote left open, a top-level comma, a `;` or a comment start,
 * which is what would let text escape the CAST.  What passes here and is still no type fails inside the CAST,
 * where DuckDB's parser judges it.  `q` is the quote a run opened; a doubled one is that character escaped. */
static bool qd_dtype_wellformed(const char* s) {
    int depth = 0;
    char q = 0;
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_')) return false;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (q) { if (ch == (unsigned char)q) { if (s[1] == q) s++; else q = 0; } continue; }
        if (ch == '\'' || ch == '"') { if (depth == 0) return false; q = (char)ch; continue; }
        if (ch == '(' || ch == '[') { depth++; continue; }
        if (ch == ')' || ch == ']') { if (--depth < 0) return false; continue; }
        if (ch == ',' && depth == 0) return false;
        if (!(isalnum(ch) || ch == '_' || ch == ' ' || ch == ',')) return false;
    }
    return depth == 0 && !q;
}

/* the schema half of an incoming pair -> rows (col, dtype, logical): every row declares a dtype (a blank one would
 * be a silent no-op), the logical is optional; iskey is the data's keyed shape, not read */
ray_t* q_duckdb_schema_desc_parse(int slot, ray_t* schema, qd_desc_t** out, int64_t* n) {
    *out = NULL;
    *n   = 0;
    if (!schema || schema->type != RAY_TABLE) return qd_schema_fail(slot, "schema is not a table");
    ray_t* cv = ray_table_get_col(schema, ray_sym_intern_runtime("col", 3));       /* borrowed */
    ray_t* dv = ray_table_get_col(schema, ray_sym_intern_runtime("dtype", 5));
    ray_t* lv = ray_table_get_col(schema, ray_sym_intern_runtime("logical", 7));
    if (!cv || !dv) return qd_schema_fail(slot, "schema needs col and dtype columns");
    int64_t    rows = q_count(schema);
    qd_desc_t* d    = q_duckdb_cols(rows, sizeof *d);
    if (!d) return q_err(QE_WSFULL);
    ray_t* e = NULL;
    for (int64_t i = 0; !e && i < rows; i++) {
        const char *cp = "", *dp = "", *lp = "";
        int64_t     cl = 0,   dl = 0,   ll = 0;
        if (!qd_schema_text(cv, i, &cp, &cl) || cl == 0) e = qd_schema_fail(slot, "schema col is not a column name");
        else if (!qd_schema_text(dv, i, &dp, &dl) || dl == 0) e = qd_schema_fail(slot, "schema dtype is not text");
        else if (!qd_desc_set(&d[i], cp, (size_t)cl, dp, (size_t)dl)) e = q_err(QE_WSFULL);
        else if (!qd_dtype_wellformed(d[i].dtype)) e = qd_schema_fail(slot, "schema dtype is not a type declaration");
        else if (lv && !qd_schema_text(lv, i, &lp, &ll)) e = qd_schema_fail(slot, "schema logical is not text");
        else if (ll >= (int64_t)sizeof d[i].logical) e = qd_schema_fail(slot, "schema logical is not a logical type");
        else snprintf(d[i].logical, sizeof d[i].logical, "%.*s", (int)ll, lp);
    }
    if (e) { q_duckdb_schema_desc_free(d, rows); return e; }
    *out = d;
    *n   = rows;
    return NULL;
}

/* the record kind a declaration names, STRUCT where it names none (which is what a bare dict column stages as) */
static duck_type qd_rec_kind(const char* dt) {
    return strncasecmp(dt, "MAP(", 4) == 0   ? QDUCK_TYPE_MAP
         : strncasecmp(dt, "UNION(", 6) == 0 ? QDUCK_TYPE_UNION : QDUCK_TYPE_STRUCT;
}

static ray_t* qd_declare_fail(int slot, const qd_desc_t* d, const char* why) {
    char what[300];
    snprintf(what, sizeof what, "schema row %s", d->col);
    return q_duckdb_fail(slot, what, why);
}

/* A declared dtype is respelled as DuckDB itself spells it (typeof of a NULL cast to it), ONCE, before anything
 * reads it: every alias, case and whitespace variant then meets the cast-leg predicates, the sidecar and append's
 * catalog check as one spelling, and a type DuckDB does not know fails here with its catalog error. */
static ray_t* qd_canon_dtype(int slot, qd_desc_t* d) {
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT typeof(CAST(NULL AS ");
    q_duckdb_puts(&b, d->dtype);
    q_duckdb_puts(&b, ")) AS t");
    if (b.oom) { q_duckdb_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (e) return e;
    ray_t* got = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
    QAPI.destroy_result(&res);
    if (!got || RAY_IS_ERR(got)) return got ? got : q_err(QE_WSFULL);
    size_t n = 0;
    const char* t  = got->type == RAY_TABLE ? q_duckdb_schema_text_cell(ray_table_get_col_idx(got, 0), 0, &n) : NULL;
    bool        had = t != NULL;
    char*       nd  = had ? q_duckdb_text(t, n) : NULL;
    ray_release(got);
    if (!nd) return had ? q_err(QE_WSFULL) : qd_declare_fail(slot, d, "DuckDB spelled no type for it");
    free(d->dtype);
    d->dtype = nd;
    return NULL;
}

/* The declared type as DuckDB itself reads it: the same NULL cast qd_canon_dtype spells with, kept as a logical
 * type this time, which is what a RECORD needs — its field list lives in the declaration and nowhere else.  DuckDB
 * has already parsed this dtype once (qd_canon_dtype), so the statement stands or the connection is gone. */
static ray_t* qd_declared_logical(int slot, const char* dtype, duck_logical_type* out) {
    *out = NULL;
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT CAST(NULL AS ");
    q_duckdb_puts(&b, dtype);
    q_duckdb_puts(&b, ") AS t");
    if (b.oom) { q_duckdb_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (e) return e;
    *out = QAPI.column_logical_type(&res, 0);
    QAPI.destroy_result(&res);
    return NULL;
}

/* Honour an incoming schema against the store table (cms = the q-derived maps): a row naming a column the table
 * lacks is ignored; a companion, a duplicate, a logical that does not fit the column is 'duckdb; dtypes[c] = the
 * declared type where it differs from the carrier's canonical one (the CAST the write must perform), else NULL. */
ray_t* q_duckdb_schema_declare(int slot, ray_t* tbl, qd_colmap_t* cms, qd_desc_t* decl, int64_t ndecl,
                               const char** dtypes) {
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < ncols; c++) dtypes[c] = NULL;
    for (int64_t i = 0; i < ndecl; i++) {
        qd_desc_t* d = &decl[i];
        size_t dn = strlen(d->col);
        if (q_duckdb_codec_companion_name(d->col, dn)) return qd_declare_fail(slot, d, "a companion is not a store column");
        int64_t c = -1, id = ray_sym_intern_runtime(d->col, dn);
        for (int64_t k = 0; k < ncols && c < 0; k++) if (ray_table_col_name(tbl, k) == id) c = k;
        if (c < 0) continue;
        for (int64_t j = 0; j < i; j++)
            if (strcmp(decl[j].col, d->col) == 0) return qd_declare_fail(slot, d, "declared twice");
        qd_colmap_t ref;
        if (d->logical[0] && !q_duckdb_codec_parse_logical(d->logical, &ref))
            return qd_declare_fail(slot, d, "unknown logical type");
        bool bits = d->logical[0] && ref.leaf->dk_type == QDUCK_TYPE_BIT;
        if (d->logical[0] && !bits) {
            if (!cms[c].undet && (ref.depth != cms[c].depth || ref.leaf->ray_type != cms[c].leaf->ray_type))
                return qd_declare_fail(slot, d, "logical type does not fit the column");
            /* ADR 15: an empty 0h column has no carrier to fit AGAINST — the row IS the carrier, so it is adopted.
             * What it adopts is the q type's CANONICAL row, never the declared row itself: a column with rows
             * stages its carrier and reaches the declaration by the cast leg, and an empty one must take the same
             * route or a DECIMAL would stage as one.  A RECORD is the one it cannot adopt at all: its fields live
             * in cells there are none of, and only its dtype spells them, so it stays the placeholder. */
            if (cms[c].undet && !q_duckdb_codec_is_rec(ref.leaf)) {
                const qd_tmap_t* canon = q_duckdb_codec_canon_leaf(ref.leaf->ray_type);
                cms[c].leaf = canon ? canon : ref.leaf; cms[c].depth = ref.depth; cms[c].undet = false;
            }
        }
        ray_t* e = qd_canon_dtype(slot, d);
        if (e) return e;
        /* ADR 20: BIT is the one declaration that REMAPS instead of casting — a boolean-list column derives
         * BOOLEAN[] and DuckDB implements no cast either way, so the appender must write BIT directly.  The
         * remap is per LEVEL: a BIT[] is one LIST over the bitstring the innermost booleans spell. */
        if (bits) {
            qd_buf sp = {0};
            q_duckdb_codec_spell_map(&(const qd_colmap_t){ ref.leaf, NULL, ref.depth, false }, &sp);
            bool same = !sp.oom && strcmp(d->dtype, sp.p) == 0;
            q_duckdb_buf_free(&sp);
            if (!same) return qd_declare_fail(slot, d, "a bitstring column declares BIT and nothing else");
            if (!q_duckdb_codec_map_bits(ray_table_get_col_idx(tbl, c), ref.depth, &cms[c]))
                return qd_declare_fail(slot, d, "a BIT declares a boolean-list column only");
            cms[c].undet = false;
            continue;
        }
        /* ADR 15, the dtype leg: with no logical to adopt, the declared type's own shape is the carrier — walked as a
         * declared record's is, its leaves canonical, so the cast leg below stays the one every full column takes.
         * Over ROWS every cell is a (), a LIST, so the declaration must be one: a flat carrier would read the cells
         * as atoms, and a record's fields live in cells a () spells none of.  Over no rows a record keeps the
         * placeholder, whose leg binds CAST(NULL AS ...), which is sound exactly there. */
        if (cms[c].undet && d->dtype[0]) {
            duck_logical_type lt = NULL;
            if ((e = qd_declared_logical(slot, d->dtype, &lt))) return e;
            qd_colmap_t stage = { NULL, NULL, 0, false };
            bool mapped = lt && q_duckdb_codec_declared_stage_map(lt, &stage);
            if (lt) QAPI.destroy_logical_type(&lt);
            bool rows = q_count(tbl) > 0, rec = mapped && q_duckdb_codec_is_rec(stage.leaf);
            const char* why = !mapped       ? "the declared type has no mapping"
                            : rec           ? "a () cell spells no record: an empty record cell is an empty table"
                            : !stage.depth  ? "a () cell is a list, so the declared type must be a LIST" : NULL;
            bool adopt = mapped && !rec && !(rows && why);
            if (adopt) { q_duckdb_codec_map_free(&cms[c]); cms[c] = stage; }
            else if (mapped) q_duckdb_codec_map_free(&stage);
            if (rows && why) return qd_declare_fail(slot, d, why);
        }
        /* a record still undetermined asks as the empty column it is, so the declaration is carried even where the
         * placeholder happens to spell it — which is what makes dtypes[c] below a sound "was it declared" */
        if (d->dtype[0] && (q_duckdb_schema_needs(&cms[c], d->dtype, cms[c].undet) & QD_NEED_DTYPE)) dtypes[c] = d->dtype;
        /* ADR 12: a dict spells a STRUCT by itself, so a declared record's fields — which of the three it is, in
         * what order, and the ones no row is live in — are read off the DECLARATION, never off the rows. */
        if (dtypes[c] && !cms[c].undet && q_duckdb_codec_is_rec(cms[c].leaf)) {
            duck_logical_type lt = NULL;
            if ((e = qd_declared_logical(slot, d->dtype, &lt))) return e;
            e = q_duckdb_codec_map_declared(slot, ray_table_get_col_idx(tbl, c), lt, qd_rec_kind(d->dtype), &cms[c]);
            if (!e) e = q_duckdb_codec_check_fields(slot, tbl, c, lt, &cms[c]);
            if (lt) QAPI.destroy_logical_type(&lt);
            if (e) return e;
        }
    }
    /* ADR 15: whatever is still undetermined got no declaration, and nothing else can supply one — guessing a
     * default is the silent wrong answer the envelope exists to prevent, so the write is refused, naming it */
    for (int64_t c = 0; c < ncols; c++) {
        if (!cms[c].undet || dtypes[c]) continue;
        ray_t* cn = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        char what[300];
        snprintf(what, sizeof what, "column %.*s", (int)(cn ? ray_str_len(cn) : 1), cn ? ray_str_ptr(cn) : "?");
        return q_duckdb_fail(slot, what, "an empty untyped column needs a schema row to say what type it is");
    }
    /* undeclared, a dict spells itself: the STRUCT of the fields its cells agree on, which is all a row can say */
    for (int64_t c = 0; c < ncols; c++) {
        if (!q_duckdb_codec_is_rec(cms[c].leaf) || cms[c].undet || dtypes[c]) continue;
        ray_t* e = q_duckdb_codec_map_declared(slot, ray_table_get_col_idx(tbl, c), NULL, QDUCK_TYPE_STRUCT, &cms[c]);
        if (!e) e = q_duckdb_codec_check_fields(slot, tbl, c, NULL, &cms[c]);
        if (e) return e;
    }
    return NULL;
}

/* ---- the cast legs of stage-then-cast.  DuckDB judges every declared conversion, but two of its facts need a
 * spelling beyond CAST(c AS dtype): a zoned target reads a naive timestamp in the SESSION zone, so the instant is
 * fixed as UTC first (AT TIME ZONE 'UTC', which strips the zone again on the way back); and 1.5 implements no column
 * cast between TIMESTAMP_NS and TIMESTAMP_S / TIMESTAMP_MS, so those hop through TIMESTAMP.  ONE spelling serves both
 * legs, which is what lets the exactness check hold whatever DuckDB narrows in between. ---- */

static bool qd_zoned(const char* t) {
    return strcasecmp(t, "TIMESTAMPTZ") == 0 || strcasecmp(t, "TIMESTAMP WITH TIME ZONE") == 0;
}

/* the scale of a FLAT DECIMAL declaration, -1 if it is not one; a bare DECIMAL never reaches here (qd_canon_dtype
 * has already had DuckDB spell it DECIMAL(18,3)) */
static int qd_decimal_scale(const char* dt) {
    if (strncasecmp(dt, "DECIMAL", 7) != 0 || strchr(dt, '[')) return -1;
    const char* c = strchr(dt, ',');
    return c ? atoi(c + 1) : 0;
}

/* ADR 21: TIMETZ is the one declared target built in TWO steps — the wall time, then the offset — so every leg that
 * judges the VALUE judges its naive half and only the final projection wears the zone. */
static bool qd_timetz(const char* t) {
    return strcasecmp(t, "TIMETZ") == 0 || strcasecmp(t, "TIME WITH TIME ZONE") == 0;
}

static bool qd_any_zoned(const char* t) { return qd_zoned(t) || qd_timetz(t); }

#define QD_BASE_MAX 128    /* the longest base type a lift spells; past it there is no lift and DuckDB judges the cast */

/* The `[]` / `[n]` suffixes a declared type wears: the count, `base` (optional) the type under them, *fixed
 * (optional) whether any of them names a WIDTH.  -1 = no base this bridge can hold. */
static int qd_base_of(const char* t, char* base, size_t cap, bool* fixed) {
    size_t n = strlen(t);
    int d = 0;
    if (fixed) *fixed = false;
    while (n && t[n - 1] == ']') {
        size_t end = n - 1;
        while (n && t[n - 1] != '[') n--;
        if (!n) return -1;
        if (fixed && end > n) *fixed = true;
        n--;
        d++;
    }
    if (base) {
        if (n >= cap) return -1;
        memcpy(base, t, n);
        base[n] = '\0';
    }
    return d;
}

/* ADR 21: the naive type a declaration's legs judge — TIMETZ is built in two steps, so every leg that judges the
 * VALUE judges its wall time and only the final projection wears the zone.  The suffixes ride along. */
static int qd_naive_of(qd_buf* b, const char* t, bool* tz) {
    char base[QD_BASE_MAX];
    int  d = qd_base_of(t, base, sizeof base, NULL);
    bool z = d >= 0 && qd_timetz(base);
    if (tz) *tz = z;
    if (d < 0) { q_duckdb_puts(b, t); return 0; }
    q_duckdb_puts(b, z ? "TIME" : base);
    q_duckdb_puts(b, t + strlen(base));
    return d;
}

static bool qd_naive_ts(const char* t) { return strncasecmp(t, "TIMESTAMP", 9) == 0 && !qd_zoned(t); }

static bool qd_coarse_ts(const char* t) { return strcasecmp(t, "TIMESTAMP_S") == 0 || strcasecmp(t, "TIMESTAMP_MS") == 0; }

static bool qd_hops(const char* from, const char* to) {
    return (strcasecmp(from, "TIMESTAMP_NS") == 0 && qd_coarse_ts(to)) ||
           (qd_coarse_ts(from) && strcasecmp(to, "TIMESTAMP_NS") == 0);
}

/* ADR 4: a DECIMAL column IS its unscaled integer, so the crossing MOVES A DECIMAL POINT — DuckDB's own CAST
 * would read the unscaled long as the value.  The point is spelled into the digits rather than as an `e-s`
 * exponent, which DuckDB refuses at the full 38-digit width; NULL propagates through every leg. */
static void qd_put_scaled(qd_buf* b, const char* v, int s, const char* to) {
    if (s <= 0) { q_duckdb_puts(b, "CAST("); q_duckdb_puts(b, v); q_duckdb_puts(b, " AS "); q_duckdb_puts(b, to); q_duckdb_puts(b, ")"); return; }
    char n[32];
    qd_buf d = {0};
    /* s leading zeros, so the digits are never fewer than the point needs; lpad would TRUNCATE a longer value */
    q_duckdb_puts(&d, "('");
    for (int i = 0; i < s; i++) q_duckdb_puts(&d, "0");
    q_duckdb_puts(&d, "' || CAST(abs("); q_duckdb_puts(&d, v); q_duckdb_puts(&d, ") AS VARCHAR))");
    snprintf(n, sizeof n, "%d", s);
    q_duckdb_puts(b, "CAST((CASE WHEN "); q_duckdb_puts(b, v); q_duckdb_puts(b, " < 0 THEN '-' ELSE '' END) || left(");
    q_duckdb_puts(b, QD_TEXT(d.p)); q_duckdb_puts(b, ", length("); q_duckdb_puts(b, QD_TEXT(d.p)); q_duckdb_puts(b, ") - ");
    q_duckdb_puts(b, n); q_duckdb_puts(b, ") || '.' || right("); q_duckdb_puts(b, QD_TEXT(d.p)); q_duckdb_puts(b, ", ");
    q_duckdb_puts(b, n); q_duckdb_puts(b, ") AS "); q_duckdb_puts(b, to); q_duckdb_puts(b, ")");
    if (d.oom) b->oom = 1;
    q_duckdb_buf_free(&d);
}

/* the unscaled integer of a DECIMAL: its own text with the point taken out */
static void qd_put_unscaled(qd_buf* b, const char* v, const char* to) {
    q_duckdb_puts(b, "CAST(replace(CAST("); q_duckdb_puts(b, v); q_duckdb_puts(b, " AS VARCHAR), '.', '') AS ");
    q_duckdb_puts(b, to); q_duckdb_puts(b, ")");
}

/* THE µs count -> TIME spelling, and the ONE home of DuckDB's 0..24h domain: the top of it is INCLUSIVE, and that
 * count is the one make_timestamp wraps to midnight, so it is spelled as the literal; past it the store would hold
 * characters below '0', so the count is refused where it stands rather than written. */
static void qd_put_time_us(qd_buf* b, const char* us) {
    q_duckdb_puts(b, "CASE WHEN "); q_duckdb_puts(b, us); q_duckdb_puts(b, " IS NULL THEN NULL WHEN ");
    q_duckdb_puts(b, us); q_duckdb_puts(b, " = 86400000000 THEN TIME '24:00:00' WHEN ");
    q_duckdb_puts(b, us); q_duckdb_puts(b, " BETWEEN 0 AND 86399999999 THEN CAST(make_timestamp(");
    q_duckdb_puts(b, us); q_duckdb_puts(b, ") AS TIME) ELSE error('count outside the TIME domain') END");
}

/* ADR 19: 1.5.5 implements NO cast either way between INTERVAL and TIME, nor between INTERVAL and BIGINT, and the
 * two are exactly where the q durations and the q timespan meet a DECLARED type — so the bridge spells them, over
 * DuckDB's own µs constructor and epoch_us, which is exact for the micros-only intervals this leg ever writes.
 * The TIME leg carries the domain (the caller asked for TIME, so the refusal is right); the ns leg needs no
 * refusal of its own — the exactness check compares the value against (v // 1000) * 1000 and refuses what moved. */
static bool qd_put_span_cast(qd_buf* b, const char* inner, const char* from, const char* to) {
    bool iv_to = strcasecmp(to, "INTERVAL") == 0, iv_from = strcasecmp(from, "INTERVAL") == 0;
    if (iv_to == iv_from) return false;
    const char* other = iv_to ? from : to;
    bool tm = strcasecmp(other, "TIME") == 0;
    if (!tm && strcasecmp(other, "BIGINT") != 0) return false;
    if (!iv_to) {                                        /* INTERVAL -> TIME, or -> BIGINT ns */
        qd_buf us = {0};
        q_duckdb_puts(&us, "epoch_us("); q_duckdb_puts(&us, inner); q_duckdb_puts(&us, ")");
        if (us.oom) b->oom = 1;
        else if (tm) qd_put_time_us(b, us.p);
        else { q_duckdb_puts(b, "("); q_duckdb_puts(b, us.p); q_duckdb_puts(b, " * 1000)"); }
        q_duckdb_buf_free(&us);
        return true;
    }
    q_duckdb_puts(b, tm ? "to_microseconds(epoch_us(" : "to_microseconds((");
    q_duckdb_puts(b, inner);
    q_duckdb_puts(b, tm ? "))" : ") // 1000)");
    return true;
}

/* 1.5.5 implements NO cast either way between TIME_NS and BIGINT, and a q timespan stages as BIGINT, so the bridge
 * spells both legs.  VARCHAR is TIME_NS's only lossless door, and the clock text is built with INTEGER divide (`/`
 * is float divide, which printf %d refuses).  `make_timestamp_ns` then a text cast looks tidier and is wrong: it
 * renders 86400000000000 as the next DAY at midnight, silently losing the 24:00:00 that test_all_types() holds. */
static bool qd_put_time_ns_cast(qd_buf* b, const char* inner, const char* from, const char* to) {
    static const char* const field[] = { " // 3600000000000", " // 60000000000 % 60", " // 1000000000 % 60",
                                         " % 1000000000" };
    if (strcasecmp(to, "TIME_NS") == 0 && strcasecmp(from, "BIGINT") == 0) {
        q_duckdb_puts(b, "CAST(printf('%02d:%02d:%02d.%09d'");
        for (size_t i = 0; i < sizeof field / sizeof *field; i++) {
            q_duckdb_puts(b, ", ");
            q_duckdb_puts(b, inner);
            q_duckdb_puts(b, field[i]);
        }
        q_duckdb_puts(b, ") AS TIME_NS)");
        return true;
    }
    if (strcasecmp(from, "TIME_NS") != 0 || strcasecmp(to, "BIGINT") != 0) return false;
    q_duckdb_puts(b, "epoch_ns("); q_duckdb_puts(b, inner); q_duckdb_puts(b, ")");
    return true;
}

/* CAST(inner AS to) at depth 0, inner an expression of type `from`; the UTC leg belongs to a naive-timestamp <->
 * zoned move only — text or anything else declared zoned is DuckDB's own parse */
static void qd_put_cast_flat(qd_buf* b, const char* inner, const char* from, const char* to) {
    if (qd_put_time_ns_cast(b, inner, from, to)) return;
    if (qd_put_span_cast(b, inner, from, to)) return;
    int ts = qd_decimal_scale(to), fs = qd_decimal_scale(from);
    if (ts >= 0 && fs < 0) { qd_put_scaled(b, inner, ts, to); return; }
    if (fs >= 0 && ts < 0) { qd_put_unscaled(b, inner, to); return; }
    bool zoned = (qd_zoned(to) && qd_naive_ts(from)) || (qd_zoned(from) && qd_naive_ts(to)), hop = qd_hops(from, to);
    q_duckdb_puts(b, hop ? "CAST(CAST(" : "CAST(");
    if (zoned) q_duckdb_puts(b, "(");
    q_duckdb_puts(b, inner);
    if (zoned) q_duckdb_puts(b, " AT TIME ZONE 'UTC')");
    if (hop) q_duckdb_puts(b, " AS TIMESTAMP)");
    q_duckdb_puts(b, " AS ");
    q_duckdb_puts(b, to);
    q_duckdb_puts(b, ")");
}

/* ---- THE elementwise lift.  A declared cast at depth d is the flat leg above under d levels of list_transform,
 * every column that feeds it indexed at the same position — one law over every carrier, no per-type arm.  Two
 * facts of DuckDB's make it work: the lambda takes an INDEX beside its element, which is what pairs a value with
 * the companion standing beside it; and a NULL cell, an empty one and a NULL element all pass through untouched.
 * One fact of DuckDB's needs undoing — list_transform DEGRADES a fixed ARRAY to a LIST, so a declaration naming a
 * WIDTH gets it back with a trailing cast; without that the values round-trip and the TYPE silently narrows. ---- */

#define QD_LIFT_COLS 3     /* a slot, its hi/raw companion, and the zone offset beside it (ADR 21) */

typedef void (*qd_leaf_fn)(qd_buf* b, const char* const* e, void* ctx);

/* the element of `col` inside d lambdas: col[qdi1]...[qdid], DuckDB's list index being 1-based as the lambda's is */
static void qd_put_elem(qd_buf* b, const char* col, int d) {
    q_duckdb_puts(b, col);
    for (int l = 1; l <= d; l++) { char ix[16]; snprintf(ix, sizeof ix, "[qdi%d]", l); q_duckdb_puts(b, ix); }
}

static void qd_lift(qd_buf* b, int d, const char* full, const char* const* cols, qd_leaf_fn f, void* ctx) {
    const char* e[QD_LIFT_COLS] = { cols[0], cols[1], cols[2] };
    if (d <= 0) { f(b, e, ctx); return; }
    qd_buf el[QD_LIFT_COLS];
    char   var[16];
    bool   fixed = false;
    qd_base_of(full, NULL, 0, &fixed);
    snprintf(var, sizeof var, "qdx%d", d);
    e[0] = var;
    for (int k = 1; k < QD_LIFT_COLS; k++) {
        el[k] = (qd_buf){0};
        if (!cols[k]) continue;
        qd_put_elem(&el[k], cols[k], d);
        e[k] = el[k].p;
    }
    if (fixed) q_duckdb_puts(b, "CAST(");
    for (int l = 1; l <= d; l++) {
        char lv[16], li[16], src[16] = "";
        snprintf(lv, sizeof lv, "qdx%d", l);
        snprintf(li, sizeof li, "qdi%d", l);
        if (l > 1) snprintf(src, sizeof src, "qdx%d", l - 1);
        q_duckdb_puts(b, "list_transform(");
        q_duckdb_puts(b, l == 1 ? cols[0] : src);
        q_duckdb_puts(b, ", (");
        q_duckdb_puts(b, lv); q_duckdb_puts(b, ", "); q_duckdb_puts(b, li);
        q_duckdb_puts(b, ") -> ");
    }
    f(b, e, ctx);
    for (int l = 0; l < d; l++) q_duckdb_puts(b, ")");
    if (fixed) { q_duckdb_puts(b, " AS "); q_duckdb_puts(b, full); q_duckdb_puts(b, ")"); }
    for (int k = 1; k < QD_LIFT_COLS; k++) { if (el[k].oom) b->oom = 1; q_duckdb_buf_free(&el[k]); }
}

static void qd_leaf_cast(qd_buf* b, const char* const* e, void* ctx) {
    const char* const* t = ctx;
    qd_put_cast_flat(b, e[0], t[0], t[1]);
}

/* THE cast leg at whatever depth `from` and `to` agree on.  Depths that DISAGREE are no cast this bridge spells:
 * they go through as the flat CAST they are, for DuckDB to refuse in its own words. */
static void qd_put_cast(qd_buf* b, const char* inner, const char* from, const char* to) {
    char fb[QD_BASE_MAX], tb[QD_BASE_MAX];
    int  d = qd_base_of(to, tb, sizeof tb, NULL);
    if (d <= 0 || d != qd_base_of(from, fb, sizeof fb, NULL)) { qd_put_cast_flat(b, inner, from, to); return; }
    const char* cols[QD_LIFT_COLS] = { inner, NULL, NULL };
    const char* t[2] = { fb, tb };
    qd_lift(b, d, to, cols, qd_leaf_cast, t);
}

/* The wall time `w` and the offset `o` back into one TIMETZ, spelled as the TEXT DuckDB parses.  The instant-shifting
 * spelling (timezone(o, (w - o)::TIMETZ)) is exact everywhere EXCEPT 24:00:00, which TIME arithmetic wraps to
 * midnight — and 24:00:00 is a TIMETZ DuckDB itself constructs, so the leg that loses it is the wrong one.
 * NO companion (`o` NULL) means every offset is zero, which is what a bare TIME means.  One that EXISTS answers for
 * every row: the reader writes 0Ni only where the value itself is NULL, so a null offset beside a real wall time is
 * a value the caller invented, refused where it stands rather than written as +00.  An offset outside ±15:59:59 is
 * DuckDB's own refusal on the cast. */
static void qd_put_timetz(qd_buf* b, const char* w, const char* o) {
    if (!o) {
        q_duckdb_puts(b, "CAST(CAST("); q_duckdb_puts(b, w);
        q_duckdb_puts(b, " AS VARCHAR) || '+00:00:00' AS TIME WITH TIME ZONE)");
        return;
    }
    qd_buf s = {0};
    q_duckdb_puts(&s, "abs("); q_duckdb_puts(&s, o); q_duckdb_puts(&s, ")");
    q_duckdb_puts(b, "CASE WHEN "); q_duckdb_puts(b, w); q_duckdb_puts(b, " IS NOT NULL AND "); q_duckdb_puts(b, o);
    q_duckdb_puts(b, " IS NULL THEN error('a wall time with no offset is no TIMETZ') ELSE CAST(CAST(");
    q_duckdb_puts(b, w); q_duckdb_puts(b, " AS VARCHAR) || (CASE WHEN ");
    q_duckdb_puts(b, o); q_duckdb_puts(b, " < 0 THEN '-' ELSE '+' END) || lpad(CAST(");
    q_duckdb_puts(b, QD_TEXT(s.p)); q_duckdb_puts(b, " // 3600 AS VARCHAR),2,'0') || ':' || lpad(CAST((");
    q_duckdb_puts(b, QD_TEXT(s.p)); q_duckdb_puts(b, " // 60) % 60 AS VARCHAR),2,'0') || ':' || lpad(CAST(");
    q_duckdb_puts(b, QD_TEXT(s.p)); q_duckdb_puts(b, " % 60 AS VARCHAR),2,'0') AS TIME WITH TIME ZONE) END");
    if (s.oom) b->oom = 1;
    q_duckdb_buf_free(&s);
}

/* the type a staged column holds — the same speller q_duckdb_schema_create_ddl declared it with */
static void qd_spell_stage(const qd_colmap_t* cm, qd_buf* b) { q_duckdb_codec_spell_map(cm, b); }

/* ---- the long companions on the way back (ADR 4/11).  A raw count rebuilds its temporal through DuckDB's own
 * constructors — CAST has no integer -> temporal arm — one spelling per family; a TIME count outside the day is
 * refused, never wrapped.  A 128-bit word is composed unsigned from its two halves (every 64-bit pattern is a legal
 * half, so each is lifted to [0, 2^64) first) and, for a signed target, the top bit read as -2^128 by subtracting
 * from the edge, since DuckDB refuses the shift and the product that would land on HUGEINT's minimum. ---- */

/* a coarse grain's storable range IS the µs range (DuckDB parses no TIMESTAMP_S past 9223372036854), so the scaled
 * product below cannot overflow a value that came from DuckDB, and a lie about the count meets its checked multiply */
static bool qd_put_raw(qd_buf* b, const char* raw, const char* to) {
    const char *open, *close;
    if      (strcasecmp(to, "TIMESTAMP") == 0)    { open = "make_timestamp(";           close = ")"; }
    else if (strcasecmp(to, "TIMESTAMP_S") == 0)  { open = "CAST(make_timestamp(";      close = " * 1000000::BIGINT) AS TIMESTAMP_S)"; }
    else if (strcasecmp(to, "TIMESTAMP_MS") == 0) { open = "CAST(make_timestamp(";      close = " * 1000::BIGINT) AS TIMESTAMP_MS)"; }
    else if (strcasecmp(to, "TIMESTAMP_NS") == 0) { open = "make_timestamp_ns(";        close = ")"; }
    else if (qd_zoned(to))                        { open = "make_timestamptz(";         close = ")"; }
    else if (strcasecmp(to, "DATE") == 0)         { open = "(DATE '1970-01-01' + CAST("; close = " AS INTEGER))"; }
    else if (strcasecmp(to, "INTERVAL") == 0)     { open = "to_microseconds(";           close = ")"; }
    else if (strcasecmp(to, "TIME") == 0)         { open = NULL;                        close = NULL; }
    else return false;
    if (!open) { qd_put_time_us(b, raw); return true; }
    /* ADR 17 made the raw FULL, so it now carries the infinities too — and neither make_timestamp nor date
     * arithmetic accepts the carrier's extreme (both error).  The same bits spell both sides' infinity, so the
     * sentinel is read off the count and spelled as DuckDB's own literal, cast where the grain needs it.  Only the
     * two families that HAVE an infinity get the arms: INTERVAL has none, and a CASE mixing it with a timestamp
     * literal would not even bind. */
    bool date = strcasecmp(to, "DATE") == 0;
    const char* lit = date ? "DATE" : qd_zoned(to) ? "TIMESTAMPTZ" : strncasecmp(to, "TIMESTAMP", 9) == 0 ? "TIMESTAMP" : NULL;
    if (!lit) { q_duckdb_puts(b, open); q_duckdb_puts(b, raw); q_duckdb_puts(b, close); return true; }
    const char* edge = date ? "2147483647" : "9223372036854775807";
    bool coarse = qd_coarse_ts(to) || strcasecmp(to, "TIMESTAMP_NS") == 0;
    q_duckdb_puts(b, "CASE WHEN "); q_duckdb_puts(b, raw); q_duckdb_puts(b, " = "); q_duckdb_puts(b, edge);
    q_duckdb_puts(b, " THEN "); if (coarse) q_duckdb_puts(b, "CAST(");
    q_duckdb_puts(b, lit); q_duckdb_puts(b, " 'infinity'");
    if (coarse) { q_duckdb_puts(b, " AS "); q_duckdb_puts(b, to); q_duckdb_puts(b, ")"); }
    q_duckdb_puts(b, " WHEN "); q_duckdb_puts(b, raw); q_duckdb_puts(b, " = -"); q_duckdb_puts(b, edge);
    q_duckdb_puts(b, " THEN "); if (coarse) q_duckdb_puts(b, "CAST(");
    q_duckdb_puts(b, lit); q_duckdb_puts(b, " '-infinity'");
    if (coarse) { q_duckdb_puts(b, " AS "); q_duckdb_puts(b, to); q_duckdb_puts(b, ")"); }
    q_duckdb_puts(b, " ELSE ");
    q_duckdb_puts(b, open); q_duckdb_puts(b, raw); q_duckdb_puts(b, close);
    q_duckdb_puts(b, " END");
    return true;
}

/* x, a BIGINT holding one half's bits, as the HUGEINT in [0, 2^64) */
static void qd_put_u64(qd_buf* b, const char* x) {
    q_duckdb_puts(b, "(CAST("); q_duckdb_puts(b, x); q_duckdb_puts(b, " AS HUGEINT) + CASE WHEN "); q_duckdb_puts(b, x);
    q_duckdb_puts(b, " < 0 THEN 18446744073709551616::HUGEINT ELSE 0::HUGEINT END)");
}

/* The word a hi pair composes for its target: the two halves carry no sign of their own — (-1;-1) is UHUGEINT's
 * maximum and HUGEINT's -1 — so only the integer family, whose name fixes the sign, may be declared over one:
 * UHUGEINT for the unsigned half, HUGEINT for the signed; any other target is NULL, refused rather than read
 * under a guessed sign (DECIMAL's spelling is PR 4's). */
static const char* qd_word_type(const char* to) {
    static const duck_type sgn[] = { QDUCK_TYPE_TINYINT, QDUCK_TYPE_SMALLINT, QDUCK_TYPE_INTEGER, QDUCK_TYPE_BIGINT,
                                     QDUCK_TYPE_HUGEINT };
    static const duck_type uns[] = { QDUCK_TYPE_UTINYINT, QDUCK_TYPE_USMALLINT, QDUCK_TYPE_UINTEGER, QDUCK_TYPE_UBIGINT,
                                     QDUCK_TYPE_UHUGEINT };
    for (size_t i = 0; i < sizeof sgn / sizeof *sgn; i++) {
        if (strcasecmp(to, q_duckdb_type_name(sgn[i])) == 0) return "HUGEINT";
        if (strcasecmp(to, q_duckdb_type_name(uns[i])) == 0) return "UHUGEINT";
    }
    return qd_decimal_scale(to) >= 0 ? "HUGEINT" : NULL;   /* an unscaled integer is signed (ADR 4) */
}

/* the 128-bit word of the halves lo and hi, as a value of qd_word_type(to); false = no word for that target */
static bool qd_put_word(qd_buf* b, const char* lo, const char* hi, const char* to) {
    const char* w = qd_word_type(to);
    if (!w) return false;
    qd_buf bits = {0};
    q_duckdb_puts(&bits, "((CAST("); qd_put_u64(&bits, hi); q_duckdb_puts(&bits, " AS UHUGEINT) << 64) | CAST(");
    qd_put_u64(&bits, lo); q_duckdb_puts(&bits, " AS UHUGEINT))");
    if (bits.oom) b->oom = 1;
    else if (w[0] == 'U') q_duckdb_puts(b, bits.p);
    else {
        q_duckdb_puts(b, "CASE WHEN "); q_duckdb_puts(b, hi);
        q_duckdb_puts(b, " < 0 THEN -CAST(340282366920938463463374607431768211455::UHUGEINT - "); q_duckdb_puts(b, bits.p);
        q_duckdb_puts(b, " AS HUGEINT) - 1::HUGEINT ELSE CAST("); q_duckdb_puts(b, bits.p); q_duckdb_puts(b, " AS HUGEINT) END");
    }
    q_duckdb_buf_free(&bits);
    return true;
}

/* the pair (0N;0N) is a hi column's NULL */
static void qd_put_hi_null(qd_buf* b, const char* lo, const char* hi) {
    q_duckdb_puts(b, "("); q_duckdb_puts(b, lo); q_duckdb_puts(b, " = (-9223372036854775807-1) AND "); q_duckdb_puts(b, hi);
    q_duckdb_puts(b, " = (-9223372036854775807-1))");
}

static bool qd_put_hi(qd_buf* b, const char* lo, const char* hi, const char* to) {
    qd_buf w = {0};
    bool ok = qd_put_word(&w, lo, hi, to);
    q_duckdb_puts(b, "CASE WHEN "); qd_put_hi_null(b, lo, hi); q_duckdb_puts(b, " THEN NULL ELSE ");
    if (ok) qd_put_cast(b, QD_TEXT(w.p), QD_TEXT(qd_word_type(to)), to);
    q_duckdb_puts(b, " END");
    if (w.oom) b->oom = 1;
    q_duckdb_buf_free(&w);
    return ok;
}

/* ADR 17: the raw is a FULL value, so the whole of it is rebuilt from ITS companion — one rule over one column,
 * never a per-row choice between the slot and the companion.  ADR 19: a raw of DuckDB's own text is its own parse.
 * An ABSENT raw (0N, or "" for a text one) is the NULL, as (0N;0N) is a hi pair's — but only where the slot is NULL
 * too: a raw that answers for nothing its slot answers for is a pair the reader never emits, refused rather than
 * read as a NULL. */
static bool qd_put_raw_leg(qd_buf* x, const char* slot, const char* raw, bool text, const char* to) {
    qd_buf gone = {0};
    q_duckdb_puts(&gone, raw); q_duckdb_puts(&gone, " IS NULL");
    if (text) { q_duckdb_puts(&gone, " OR "); q_duckdb_puts(&gone, raw); q_duckdb_puts(&gone, " = ''"); }
    q_duckdb_puts(x, "CASE WHEN ("); q_duckdb_puts(x, QD_TEXT(gone.p)); q_duckdb_puts(x, ") AND ");
    q_duckdb_puts(x, slot);
    q_duckdb_puts(x, " IS NOT NULL THEN error('a raw companion answers for every row') WHEN ");
    q_duckdb_puts(x, QD_TEXT(gone.p)); q_duckdb_puts(x, " THEN NULL ELSE ");
    if (gone.oom) x->oom = 1;
    q_duckdb_buf_free(&gone);
    bool ok = true;
    if (text) qd_put_cast(x, raw, "VARCHAR", to);
    else      ok = qd_put_raw(x, raw, to);
    q_duckdb_puts(x, " END");
    return ok;
}

/* the word a hi pair composes, its own NULL pair answered as NULL; ctx = the target whose name fixes the sign,
 * which the caller has already had qd_word_type answer for */
static void qd_leaf_word(qd_buf* b, const char* const* e, void* ctx) {
    q_duckdb_puts(b, "CASE WHEN "); qd_put_hi_null(b, e[0], e[1]); q_duckdb_puts(b, " THEN NULL ELSE ");
    if (!qd_put_word(b, e[0], e[1], (const char*)ctx)) b->oom = 1;
    q_duckdb_puts(b, " END");
}

/* WHICH companion rebuilds this column: a nested parent keeps its shape mirror in masks[] and its hi or raw
 * count one level in, in keeps[] — the cast leg's either way, so it asks for the count first. */
static int qd_co_of(const qd_colmap_t* cm, ray_t* mask, ray_t* keep) {
    int k = q_duckdb_codec_keep_kind(cm, keep);
    return k >= QD_CO_RAW ? k : q_duckdb_codec_mask_kind(cm, mask);
}

typedef struct { int kind; bool tz, text, ok; const char *stage, *to; } qd_leg_t;

/* ONE value's rebuild: the slot e[0], its hi/raw companion e[1], the zone offset e[2] beside it.  ADR 21: the
 * naive half is built first and the zone goes on last, so both halves meet at the same element. */
static void qd_leaf_leg(qd_buf* b, const char* const* e, void* ctx) {
    qd_leg_t* g = ctx;
    qd_buf n = {0};
    if (g->kind == QD_CO_HI)       g->ok = qd_put_hi(&n, e[0], e[1], g->to);
    else if (g->kind == QD_CO_RAW) g->ok = qd_put_raw_leg(&n, e[0], e[1], g->text, g->to);
    else                           qd_put_cast(&n, e[0], g->stage, g->to);
    if (g->tz) qd_put_timetz(b, QD_TEXT(n.p), e[2]);
    else       q_duckdb_puts(b, QD_TEXT(n.p));
    if (n.oom) b->oom = 1;
    q_duckdb_buf_free(&n);
}

/* The staging projection, SELECT ... FROM <from>: a declared column CAST to its type, a column with a long companion
 * rebuilt from both stage columns (its target the declared or the canonical type), every other column as it is.
 * DuckDB judges each conversion; q_duckdb_schema_exact_check has already refused what it would narrow. */
ray_t* q_duckdb_schema_cast_select(int slot, qd_buf* b, ray_t* tbl, const qd_colmap_t* cms, const char* const* dtypes,
                                   ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps, const char* from) {
    int64_t ncols = ray_table_ncols(tbl);
    q_duckdb_puts(b, "SELECT ");
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        const char* np = nm ? ray_str_ptr(nm) : "?";
        size_t      nl = nm ? ray_str_len(nm) : 1;
        int kind = qd_co_of(&cms[c], masks[c], keeps[c]);
        if (c) q_duckdb_puts(b, ", ");
        if (dtypes[c] || kind >= QD_CO_RAW) {
            qd_buf ident = {0}, stage = {0}, to = {0}, co = {0}, cn = {0}, tn = {0}, tc = {0}, x = {0};
            char tbase[QD_BASE_MAX] = "", sbase[QD_BASE_MAX] = "";
            bool tz = false;
            q_duckdb_put_ident(&ident, np, nl);
            qd_spell_stage(&cms[c], &stage);
            if (dtypes[c]) qd_naive_of(&to, dtypes[c], &tz); else qd_spell_canon(&cms[c], &to);
            if (kind >= QD_CO_RAW) {
                q_duckdb_codec_companion_col(&cn, np, nl, kind);
                if (!cn.oom) q_duckdb_put_ident(&co, cn.p, cn.len);
            }
            if (tz && offs[c]) {
                q_duckdb_codec_companion_col(&tn, np, nl, QD_CO_TZOFF);
                if (!tn.oom) q_duckdb_put_ident(&tc, tn.p, tn.len);
            }
            bool ok = !(ident.oom || stage.oom || to.oom || co.oom || cn.oom || tn.oom || tc.oom);
            /* the lift is the COMPANION legs' and the zone's: a plain cast carries its own depth (qd_put_cast) */
            int d  = ok ? qd_base_of(to.p, tbase, sizeof tbase, NULL) : 0;
            int sd = ok ? qd_base_of(stage.p, sbase, sizeof sbase, NULL) : 0;
            int lift = d > 0 && (kind >= QD_CO_RAW || (tz && sd == d)) ? d : 0;
            if (!ok) x.oom = 1;
            /* ADR 15: an empty placeholder column holds no value, so its leg only has to BIND as the declared
             * type — and CAST(NULL AS x) binds where CAST(<placeholder> AS x) need not (BLOB reaches no UNION) */
            else if (cms[c].undet) {
                q_duckdb_puts(&x, "CAST(NULL AS "); q_duckdb_puts(&x, dtypes[c] ? dtypes[c] : to.p); q_duckdb_puts(&x, ")");
            } else {
                qd_leg_t g = { kind, tz, q_duckdb_codec_raw_is_text(cms[c].leaf->ray_type), true,
                               lift ? sbase : stage.p, lift ? tbase : to.p };
                const char* cols[QD_LIFT_COLS] = { ident.p, co.p, tc.p };
                qd_lift(&x, lift, tz && dtypes[c] ? dtypes[c] : to.p, cols, qd_leaf_leg, &g);
                ok = g.ok;
            }
            char why[400];
            if (!ok && !x.oom) snprintf(why, sizeof why, "a %s companion rebuilds no %s", q_duckdb_codec_co_name(kind), to.p);
            if (ok && !x.oom) q_duckdb_puts(b, x.p);
            if (x.oom) b->oom = 1;
            q_duckdb_buf_free(&ident); q_duckdb_buf_free(&stage); q_duckdb_buf_free(&to); q_duckdb_buf_free(&co);
            q_duckdb_buf_free(&cn); q_duckdb_buf_free(&tn); q_duckdb_buf_free(&tc); q_duckdb_buf_free(&x);
            if (!ok && !b->oom) {
                char what[300];
                snprintf(what, sizeof what, "column %.*s", (int)nl, np);
                return q_duckdb_fail(slot, what, why);
            }
            q_duckdb_puts(b, " AS ");
        }
        q_duckdb_put_ident(b, np, nl);
    }
    q_duckdb_puts(b, " FROM ");
    q_duckdb_put_ident(b, from, strlen(from));
    return NULL;
}

/* THE exactness law of a declared cast: a value DuckDB would narrow on the way to its declared type (sub-second
 * bits declared TIMESTAMP_S, sub-µs declared TIMESTAMP or TIMESTAMPTZ) is refused, never truncated — every staged
 * column cast out and back must be what it was.  Runs against the staging table before the move. */
ray_t* q_duckdb_schema_exact_check(int slot, ray_t* tbl, const qd_colmap_t* cms, const char* const* dtypes,
                                   ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps) {
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < ncols; c++) {

        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        const char* np = nm ? ray_str_ptr(nm) : "?";
        size_t      nl = nm ? ray_str_len(nm) : 1;
        /* ADR 21, the two ways an offset companion can be a lie.  Only a ZONED declaration has anywhere to put one,
         * and the cast leg would otherwise project the naive column and drop the offset without a word; and a
         * TIMESTAMPTZ is the instant and nothing else, so the only offset it takes back is zero. */
        if (offs[c]) {
            char zb[QD_BASE_MAX];
            bool zoned = dtypes[c] && qd_base_of(dtypes[c], zb, sizeof zb, NULL) >= 0 && qd_any_zoned(zb);
            const char* why = !zoned ? "an offset companion needs a zoned declaration; nothing else stores one"
                              : qd_zoned(zb) && !q_duckdb_codec_tz_all_zero(offs[c])
                                  ? "a zoned timestamp stores a UTC instant, so its offset is 0"
                                  : NULL;
            if (why) {
                char what[300];
                snprintf(what, sizeof what, "column %.*s", (int)nl, np);
                return q_duckdb_fail(slot, what, why);
            }
        }
        if (!dtypes[c]) continue;
        /* ADR 15: exactness is a property of VALUES, and an empty placeholder column has none to hold */
        if (cms[c].undet) continue;
        /* ADR 21: the zone is put on after the fact, so exactness judges the wall time the cast leg builds */
        qd_buf dt = {0};
        char   dbase[QD_BASE_MAX];
        qd_naive_of(&dt, dtypes[c], NULL);
        int  d  = dt.oom ? -1 : qd_base_of(dt.p, dbase, sizeof dbase, NULL);
        bool hi = qd_co_of(&cms[c], masks[c], keeps[c]) == QD_CO_HI;
        const char* word = d >= 0 ? qd_word_type(dbase) : NULL;
        if (d < 0 || (hi && !word)) { q_duckdb_buf_free(&dt); continue; }   /* the cast leg refuses it, with the reason */
        qd_buf ident = {0}, cn = {0}, co = {0}, val = {0}, stage = {0}, out = {0}, back = {0}, q = {0};
        q_duckdb_put_ident(&ident, np, nl);
        /* the staged value and its type: the column, or — a hi pair — the word its halves compose, its own NULL
         * pair answered as NULL so the round-trip below compares NULL with NULL there and needs no guard */
        if (hi) {
            q_duckdb_codec_companion_col(&cn, np, nl, QD_CO_HI);
            if (!cn.oom) q_duckdb_put_ident(&co, cn.p, cn.len);
            q_duckdb_puts(&stage, word);
            q_duckdb_puts(&stage, dt.p + strlen(dbase));
            if (!ident.oom && !co.oom && !stage.oom) {
                const char* cols[QD_LIFT_COLS] = { ident.p, co.p, NULL };
                qd_lift(&val, d, stage.p, cols, qd_leaf_word, (void*)dbase);
            }
        } else {
            q_duckdb_puts(&val, QD_TEXT(ident.p));
            qd_spell_stage(&cms[c], &stage);
        }
        bool ok = !(ident.oom || cn.oom || co.oom || val.oom || stage.oom);
        if (ok) { qd_put_cast(&out, val.p, stage.p, dt.p); ok = !out.oom; }
        if (ok) { qd_put_cast(&back, out.p, dt.p, stage.p); ok = !back.oom; }
        if (ok) {
            q_duckdb_puts(&q, "SELECT count(*) AS n FROM \"" QD_STAGE_TBL "\" WHERE ");
            q_duckdb_puts(&q, back.p);
            q_duckdb_puts(&q, " IS DISTINCT FROM ");
            q_duckdb_puts(&q, val.p);
            ok = !q.oom;
        }
        q_duckdb_buf_free(&ident); q_duckdb_buf_free(&cn); q_duckdb_buf_free(&co); q_duckdb_buf_free(&val); q_duckdb_buf_free(&stage);
        q_duckdb_buf_free(&out); q_duckdb_buf_free(&back);
        if (!ok) { q_duckdb_buf_free(&q); q_duckdb_buf_free(&dt); return q_err(QE_WSFULL); }
        duck_result res;
        ray_t* e = q_duckdb_run(slot, q.p, &res);
        q_duckdb_buf_free(&q);
        if (e) { q_duckdb_buf_free(&dt); return e; }
        ray_t* got = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
        QAPI.destroy_result(&res);
        if (!got || RAY_IS_ERR(got)) { q_duckdb_buf_free(&dt); return got ? got : q_err(QE_WSFULL); }
        ray_t* cnt = got->type == RAY_TABLE ? ray_table_get_col_idx(got, 0) : NULL;   /* borrowed */
        int64_t n = cnt && cnt->type == RAY_I64 && q_count(cnt) ? *(int64_t*)ray_vec_get(cnt, 0) : -1;
        ray_release(got);
        if (n == 0) { q_duckdb_buf_free(&dt); continue; }
        char what[300], why[400];
        snprintf(what, sizeof what, "column %.*s", (int)nl, np);
        if (n < 0) snprintf(why, sizeof why, "the exactness check of %s answered no count", dt.p);
        else snprintf(why, sizeof why, "declared %s does not hold the value exactly in %lld row%s, never truncated",
                      dt.p, (long long)n, n == 1 ? "" : "s");
        q_duckdb_buf_free(&dt);
        return q_duckdb_fail(slot, what, why);
    }
    return NULL;
}

/* q_duckdb_schema — declared-type knowledge: the `_q_schema` descriptor sidecar,
 * the catalog data_type spellings, the CREATE TABLE DDL and append's schema check.
 * All of it REFINES the physical catalog; none of it is a second source of truth.
 * Contract and decisions: docs/duckdb-api.md. */
#include "qlang/io/q_duckdb_internal.h"
#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_types.h"
#include "qlang/base/q_err.h"
#include <rayforce.h>
#include <stdio.h>
#include <string.h>

/* ---- _q_schema descriptor sidecar: ONE table per database, rows keyed
 * (tbl;col), shape `tbl col logical iskey valid v` — a REFINEMENT of the
 * physical catalog, never a second source of truth.  `valid` is reserved
 * for the wire arm (always '' here); v = format version 1; append-only. */

#define QD_SCHEMA_TBL "_q_schema"

bool qd_reserved_name(const char* tname) {
    return strcmp(tname, QD_SCHEMA_TBL) == 0;
}

const char* qd_text_cell(ray_t* col, int64_t i, size_t* len) {
    if (!col || col->type != RAY_LIST) return NULL;
    void* pp = ray_vec_get(col, i);
    ray_t* cell = pp ? *(ray_t**)pp : NULL;
    if (!cell || cell->type != RAY_CHARV) return NULL;
    *len = (size_t)ray_len(cell);
    return (const char*)ray_data(cell);
}

/* Fetch a table's descriptor rows; ANY sidecar trouble degrades to 0 rows. */
int64_t qd_fetch_desc(int slot, const char* tname,
                             qd_desc_t* out, int64_t cap) {
    qd_buf b = {0};
    qd_puts(&b, "SELECT col, logical, iskey FROM \"" QD_SCHEMA_TBL
                "\" WHERE tbl = ");
    qd_put_strlit(&b, tname, strlen(tname));
    if (b.oom) { qd_buf_free(&b); return 0; }
    duck_result res;
    ray_t* e = qd_run2(slot, b.p, &res, 0);   /* probe: no-sidecar is normal */
    qd_buf_free(&b);
    if (e) { ray_release(e); return 0; }
    ray_t* rows = qd_result_to_table(slot, &res, NULL, 0);
    QAPI.destroy_result(&res);
    if (!rows || RAY_IS_ERR(rows) || rows->type != RAY_TABLE) {
        if (rows) ray_release(rows);
        return 0;
    }
    int64_t n = ray_table_nrows(rows);
    if (n > cap) n = 0;
    ray_t* cv = ray_table_get_col_idx(rows, 0);   /* borrowed text col */
    ray_t* lv = ray_table_get_col_idx(rows, 1);
    ray_t* kv = ray_table_get_col_idx(rows, 2);   /* borrowed BOOL */
    for (int64_t i = 0; i < n; i++) {
        size_t cl = 0, ll = 0;
        const char* cn = qd_text_cell(cv, i, &cl);
        const char* ln = qd_text_cell(lv, i, &ll);
        snprintf(out[i].col, sizeof out[i].col, "%.*s", (int)cl, cn ? cn : "");
        snprintf(out[i].logical, sizeof out[i].logical, "%.*s", (int)ll, ln ? ln : "");
        out[i].iskey = kv && kv->type == RAY_BOOL &&
                       *(uint8_t*)ray_vec_get(kv, i) != 0;
    }
    ray_release(rows);
    return n;
}

/* One descriptor row to write (name is NOT NUL-terminated: ptr+len). */
typedef struct {
    const char* name;
    size_t      namelen;
    char        logical[96];
    bool        iskey;
} qd_descrow_t;

/* Replace a table's descriptor rows (runs INSIDE the caller's transaction). */
static ray_t* qd_write_desc_rows(int slot, const char* tname,
                                 const qd_descrow_t* rows, int64_t n) {
    ray_t* e = qd_exec_stmt(slot,
        "CREATE TABLE IF NOT EXISTS \"" QD_SCHEMA_TBL "\"("
        "tbl VARCHAR, col VARCHAR, logical VARCHAR, "
        "iskey BOOLEAN, valid VARCHAR, v BIGINT)");
    if (e) return e;

    qd_buf b = {0};
    qd_puts(&b, "DELETE FROM \"" QD_SCHEMA_TBL "\" WHERE tbl = ");
    qd_put_strlit(&b, tname, strlen(tname));
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }
    e = qd_exec_stmt(slot, b.p);
    qd_buf_free(&b);
    if (e) return e;

    if (n == 0) return NULL;
    qd_buf ins = {0};
    qd_puts(&ins, "INSERT INTO \"" QD_SCHEMA_TBL "\" VALUES ");
    for (int64_t i = 0; i < n; i++) {
        if (i) qd_puts(&ins, ", ");
        qd_puts(&ins, "(");
        qd_put_strlit(&ins, tname, strlen(tname));
        qd_puts(&ins, ", ");
        qd_put_strlit(&ins, rows[i].name, rows[i].namelen);
        qd_puts(&ins, ", ");
        qd_put_strlit(&ins, rows[i].logical, strlen(rows[i].logical));
        qd_puts(&ins, rows[i].iskey ? ", TRUE, '', 1)" : ", FALSE, '', 1)");
    }
    if (ins.oom) { qd_buf_free(&ins); return q_err(QE_WSFULL); }
    e = qd_exec_stmt(slot, ins.p);
    qd_buf_free(&ins);
    return e;
}

/* Every column gets a row — identity refinements included (set path). */
ray_t* qd_write_desc(int slot, const char* tname, ray_t* tbl,
                            const qd_colmap_t* cms, const bool* iskey) {
    int64_t ncols = ray_table_ncols(tbl);
    qd_descrow_t rows[256];
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        rows[c].name    = nm ? ray_str_ptr(nm) : "?";
        rows[c].namelen = nm ? ray_str_len(nm) : 1;
        rows[c].iskey   = iskey[c];
        qd_logical_name(&cms[c], rows[c].logical, sizeof rows[c].logical);
    }
    return qd_write_desc_rows(slot, tname, rows, ncols);
}

/* Rebuild the keyed shape from VALIDATING iskey rows (keyedness has no
 * physical truth — no DuckDB PRIMARY KEY); all-key/no-key stay plain.
 * Consumes tbl. */
ray_t* qd_rekey(ray_t* tbl, const qd_desc_t* desc, int64_t ndesc) {
    if (!tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols > 256) return tbl;
    bool iskey[256];
    int64_t nkey = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm  = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        ray_t* col = ray_table_get_col_idx(tbl, c);             /* borrowed */
        iskey[c] = false;
        for (int64_t i = 0; nm && col && i < ndesc; i++) {
            if (strlen(desc[i].col) != ray_str_len(nm) ||
                memcmp(desc[i].col, ray_str_ptr(nm), ray_str_len(nm)) != 0)
                continue;
            qd_colmap_t refined;
            iskey[c] = desc[i].iskey && qd_parse_logical(desc[i].logical, &refined) &&
                       qd_surface_of(&refined) == col->type;
            break;
        }
        if (iskey[c]) nkey++;
    }
    if (nkey == 0 || nkey == ncols) return tbl;
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(ncols - nkey);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t** dst = iskey[c] ? &kt : &vt;
        *dst = ray_table_add_col(*dst, ray_table_col_name(tbl, c),
                                 ray_table_get_col_idx(tbl, c));  /* retains */
    }
    ray_release(tbl);
    if (!kt || RAY_IS_ERR(kt) || !vt || RAY_IS_ERR(vt)) {
        if (kt && !RAY_IS_ERR(kt)) ray_release(kt);
        if (vt && !RAY_IS_ERR(vt)) ray_release(vt);
        return q_err(QE_WSFULL);
    }
    return ray_dict_new(kt, vt);   /* consumes both */
}
/* Catalog data_type string ('[]' suffixes = LIST levels, base name before '(')
 * -> column map — THE one owner of catalog spellings, meta and schema-check
 * both ride it. */
bool qd_catalog_col(const char* dt, size_t n, qd_colmap_t* out) {
    int depth = 0;
    while (n >= 2 && dt[n - 1] == ']' && dt[n - 2] == '[' && depth < QD_MAX_DEPTH)
        { n -= 2; depth++; }
    size_t base = 0;
    while (base < n && dt[base] != '(') base++;
    /* DuckDB spells REAL as FLOAT in catalog output */
    if (base == 5 && strncmp(dt, "FLOAT", 5) == 0) { dt = "REAL"; base = 4; }
    for (size_t i = 0; i < QD_NTYPES; i++) {
        if (!QD_TYPES[i].read_canon) continue;
        if (strlen(QD_TYPES[i].sql) == base &&
            strncmp(QD_TYPES[i].sql, dt, base) == 0) {
            out->leaf  = &QD_TYPES[i];
            out->depth = depth;
            return true;
        }
    }
    return false;
}
/* The store table's CREATE OR REPLACE spelling: one column per map, `[]` per LIST level. */
void qd_create_ddl(qd_buf* b, const char* tname, ray_t* tbl, const qd_colmap_t* cms) {
    int64_t ncols = ray_table_ncols(tbl);
    qd_puts(b, "CREATE OR REPLACE TABLE ");
    qd_put_ident(b, tname, strlen(tname));
    qd_puts(b, "(");
    for (int64_t c = 0; c < ncols; c++) {
        if (c) qd_puts(b, ", ");
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        qd_put_ident(b, nm ? ray_str_ptr(nm) : "?", nm ? ray_str_len(nm) : 1);
        qd_puts(b, " ");
        qd_puts(b, cms[c].leaf->sql);
        for (int l = 0; l < cms[c].depth; l++) qd_puts(b, "[]");
    }
    qd_puts(b, ")");
}

/* Append's schema check: names, order and canonical types must match the
 * catalog; a VALID descriptor row pins the LOGICAL type too (a string column
 * can't append into a symbol column). */
ray_t* qd_schema_check(int slot, const char* tname, ray_t* tbl,
                              const qd_colmap_t* cms) {
    qd_desc_t desc[256];
    int64_t ndesc = qd_fetch_desc(slot, tname, desc, 256);
    qd_buf b = {0};
    qd_puts(&b, "SELECT column_name, data_type FROM duckdb_columns() "
                "WHERE table_name = ");
    qd_put_strlit(&b, tname, strlen(tname));
    qd_puts(&b, " ORDER BY column_index");
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = qd_run(slot, b.p, &res);
    qd_buf_free(&b);
    if (e) return e;
    ray_t* cat = qd_result_to_table(slot, &res, NULL, 0);
    QAPI.destroy_result(&res);
    if (!cat || RAY_IS_ERR(cat)) return cat;

    int64_t nrows = ray_table_nrows(cat);
    int64_t ncols = ray_table_ncols(tbl);
    if (nrows == 0 || nrows != ncols) {
        ray_release(cat);
        return q_err(QE_DUCKDB);
    }
    ray_t* names  = ray_table_get_col_idx(cat, 0);   /* borrowed text cols */
    ray_t* dtypes = ray_table_get_col_idx(cat, 1);
    for (int64_t c = 0; c < ncols; c++) {
        size_t nl = 0, dl = 0;
        const char* nm = qd_text_cell(names, c, &nl);
        const char* dt = qd_text_cell(dtypes, c, &dl);
        ray_t* qn = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        size_t ql = qn ? ray_str_len(qn) : 0;
        if (!nm || !qn || ql != nl || strncmp(nm, ray_str_ptr(qn), nl) != 0) {
            ray_release(cat);
            return q_err(QE_DUCKDB);
        }
        qd_colmap_t cr;
        if (!qd_catalog_col(dt ? dt : "", dl, &cr) ||
            cr.leaf->dk_type != cms[c].leaf->dk_type || cr.depth != cms[c].depth) {
            ray_release(cat);
            return q_err(QE_DUCKDB);
        }
        for (int64_t i = 0; i < ndesc; i++) {
            if (strlen(desc[i].col) != nl || memcmp(desc[i].col, nm, nl) != 0)
                continue;
            qd_colmap_t refined;
            if (qd_parse_logical(desc[i].logical, &refined) &&
                refined.leaf->dk_type == cr.leaf->dk_type &&
                refined.depth == cr.depth &&
                (refined.leaf != cms[c].leaf || refined.depth != cms[c].depth)) {
                ray_release(cat);
                return q_err(QE_DUCKDB);
            }
            break;
        }
    }
    ray_release(cat);
    return NULL;
}

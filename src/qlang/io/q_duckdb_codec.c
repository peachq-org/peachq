/* q_duckdb_codec — the type contract in BOTH directions: the column map over
 * QD_TYPES[], the null law and its boolean companions, and the read/write cell
 * codecs.  Read and write share the map and the null law, so they share a home.
 * Contract and decisions: docs/duckdb-api.md. */
#include "qlang/io/q_duckdb_internal.h"
#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_types.h"
#include "qlang/base/q_err.h"
#include "qlang/q_prim.h"     /* q_str_text_bytes (text cells, both directions) */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <stdio.h>
#include <string.h>

static const qd_tmap_t* qd_map_logical(const char* name, size_t n) {
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (strlen(QD_TYPES[i].logical) == n &&
            memcmp(QD_TYPES[i].logical, name, n) == 0)
            return &QD_TYPES[i];
    return NULL;
}

static bool qd_cell_is_text(ray_t* cell) {
    const char* p; int64_t n;
    return cell && q_str_text_bytes(cell, &p, &n);
}

/* the loaders' text null: a 0n atom cell in a string or nested column is NULL and casts no type vote */
static bool qd_cell_is_0n(ray_t* cell) {
    return cell && cell->type == -RAY_F64 && RAY_ATOM_IS_NULL(cell);
}

static const qd_tmap_t* qd_map_read(duck_type t) {
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (QD_TYPES[i].dk_type == t && QD_TYPES[i].read_canon) return &QD_TYPES[i];
    return NULL;
}

/* The q column tag a row SURFACES as (utf8/bytes are 0h lists at q-space). */
static int8_t qd_surface_type(const qd_tmap_t* tm) {
    return (tm->ray_type == RAY_STR || tm->ray_type == RAY_LIST)
               ? RAY_LIST : tm->ray_type;
}
int8_t qd_surface_of(const qd_colmap_t* cm) {
    return cm->depth ? RAY_LIST : qd_surface_type(cm->leaf);
}

static ray_t* qd_new_col(const qd_colmap_t* cm, int64_t cap) {
    if (cap < 1) cap = 1;
    if (qd_surface_of(cm) == RAY_LIST) return ray_list_new(cap);
    if (cm->leaf->ray_type == RAY_SYM) return ray_sym_vec_new(RAY_SYM_W64, cap);
    return ray_vec_new(cm->leaf->ray_type, cap);
}

/* kdb spells a compound column with the UPPERCASE child char; a cell that is
 * itself a list (nested strings/bytes, depth >= 2) has no char at all. */
char qd_meta_char(const qd_colmap_t* cm) {
    char c = cm->leaf->meta_ch;
    if (cm->depth == 0) return c;
    if (cm->depth > 1 || qd_surface_type(cm->leaf) == RAY_LIST) return ' ';
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

/* The parameterized-name grammar: list(list(utf8)). */
void qd_logical_name(const qd_colmap_t* cm, char* buf, size_t cap) {
    size_t off = 0;
    for (int i = 0; i < cm->depth && off + 5 < cap; i++, off += 5) memcpy(buf + off, "list(", 5);
    size_t ln = strlen(cm->leaf->logical);
    if (off + ln < cap) { memcpy(buf + off, cm->leaf->logical, ln); off += ln; }
    for (int i = 0; i < cm->depth && off + 1 < cap; i++) buf[off++] = ')';
    buf[off] = '\0';
}

bool qd_parse_logical(const char* s, qd_colmap_t* out) {
    size_t n = strlen(s);
    int depth = 0;
    while (depth < QD_MAX_DEPTH && n > 6 && s[n - 1] == ')' &&
           memcmp(s, "list(", 5) == 0) { s += 5; n -= 6; depth++; }
    const qd_tmap_t* leaf = qd_map_logical(s, n);
    if (!leaf) return false;
    out->leaf  = leaf;
    out->depth = depth;
    return true;
}

/* q value -> column map (write direction).  A generic list is utf8 when EVERY
 * cell is text and bytes when every cell is a byte vector — those base cases
 * consume the level; otherwise it nests, and every cell must agree. */
static bool qd_map_write(ray_t* v, int depth, qd_colmap_t* out) {
    if (!v) return false;
    if (v->type != RAY_LIST) {
        for (size_t i = 0; i < QD_NTYPES; i++)
            if (QD_TYPES[i].ray_type == v->type) {
                out->leaf = &QD_TYPES[i];
                out->depth = depth;
                return true;
            }
        return false;
    }
    bool all_bytes = true, all_text = true;
    int64_t voted = 0;
    for (int64_t i = 0; i < v->len && (all_bytes || all_text); i++) {
        ray_t* cell = ray_list_get(v, i);
        if (!cell) return false;
        if (qd_cell_is_0n(cell)) continue;
        voted++;
        if (cell->type != RAY_BYTE_ONLY) all_bytes = false;
        if (!qd_cell_is_text(cell))      all_text  = false;
    }
    if (all_bytes || all_text || v->len == 0) {   /* all-null classifies as VARCHAR */
        int8_t want = (v->len == 0 || (voted && all_bytes)) ? RAY_LIST : RAY_STR;
        for (size_t i = 0; i < QD_NTYPES; i++)
            if (QD_TYPES[i].ray_type == want && QD_TYPES[i].read_canon) {
                out->leaf = &QD_TYPES[i];
                out->depth = depth;
                return true;
            }
        return false;
    }
    if (depth >= QD_MAX_DEPTH) return false;
    bool have = false;
    for (int64_t i = 0; i < v->len; i++) {
        ray_t* cell = ray_list_get(v, i);
        if (qd_cell_is_0n(cell)) continue;
        if (!cell || cell->type < 0) return false;      /* atom-bearing: fail loud */
        if (cell->type == RAY_LIST && cell->len == 0) continue;   /* () carries no type */
        qd_colmap_t cm;
        if (!qd_map_write(cell, depth + 1, &cm)) return false;
        if (!have) { *out = cm; have = true; }
        else if (cm.leaf != out->leaf || cm.depth != out->depth) return false;
    }
    return have;
}

/* false = no mapping, with *miss the DuckDB type id that had none */
static bool qd_map_read_logical(duck_logical_type lt, qd_colmap_t* out, duck_type* miss) {
    duck_logical_type cur = lt, owned = NULL;
    int depth = 0;
    while (cur && QAPI.get_type_id(cur) == QDUCK_TYPE_LIST && depth < QD_MAX_DEPTH) {
        duck_logical_type child = QAPI.list_type_child_type(cur);
        if (owned) QAPI.destroy_logical_type(&owned);
        cur = owned = child;
        depth++;
    }
    *miss = cur ? QAPI.get_type_id(cur) : 0;
    const qd_tmap_t* leaf = cur ? qd_map_read(*miss) : NULL;
    if (owned) QAPI.destroy_logical_type(&owned);
    if (!leaf) return false;
    out->leaf  = leaf;
    out->depth = depth;
    return true;
}

static duck_logical_type qd_make_logical(const qd_colmap_t* cm) {
    duck_logical_type t = QAPI.create_logical_type(cm->leaf->dk_type);
    for (int i = 0; i < cm->depth && t; i++) {
        duck_logical_type nested = QAPI.create_list_type(t);   /* borrows the child */
        QAPI.destroy_logical_type(&t);
        t = nested;
    }
    return t;
}
/* ---- UUID <-> hugeint (upper word's sign bit flipped for ordering;
 * bytes are RFC-4122 big-endian) ---- */

static void qd_uuid_to_bytes(duck_hugeint h, uint8_t* b) {
    uint64_t hi = (uint64_t)h.upper ^ 0x8000000000000000ULL;
    for (int i = 0; i < 8; i++) b[i]     = (uint8_t)(hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) b[8 + i] = (uint8_t)(h.lower >> (56 - 8 * i));
}

static duck_hugeint qd_bytes_to_uuid(const uint8_t* b) {
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; i++) hi = (hi << 8) | b[i];
    for (int i = 0; i < 8; i++) lo = (lo << 8) | b[8 + i];
    duck_hugeint h;
    h.upper = (int64_t)(hi ^ 0x8000000000000000ULL);
    h.lower = lo;
    return h;
}

static bool qd_guid_is_null(const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (b[i]) return false;
    return true;
}

/* placeholder elem + set_null (sentinel + HAS_NULLS attr) */
static ray_t* qd_append_null(ray_t* vec) {
    int64_t zero[2] = { 0, 0 };   /* covers up to 16-byte (guid) elems */
    vec = ray_vec_append(vec, zero);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    ray_vec_set_null(vec, vec->len - 1, true);
    return vec;
}

/* Each group flags its RARE state through a boolean companion named after the parent (ADR 2): a null-less
 * carrier (bool/byte/symbol/string/blob, and every LIST cell) has no in-band null, so its companion is
 * <c>_q_isnull; a sentinel carrier nulls in-band and flags the data value whose bits ARE the null pattern
 * (the int minimum, NaN, the all-zero UUID, the shifted temporal minimum) through <c>_q_notnull. */
#define QD_ISNULL_SUFFIX  "_q_isnull"
#define QD_NOTNULL_SUFFIX "_q_notnull"

static bool qd_nullless(const qd_tmap_t* tm) {
    return tm->ray_type == RAY_BOOL || tm->ray_type == RAY_BYTE_ONLY || tm->ray_type == RAY_SYM ||
           tm->ray_type == RAY_STR  || tm->ray_type == RAY_LIST;
}

static bool qd_cm_nullless(const qd_colmap_t* cm) { return cm->depth > 0 || qd_nullless(cm->leaf); }

/* Born on the first flagged row, zeros behind it, so a column with none costs nothing.
 * NULL = "none yet"; errors propagate as values. */
static ray_t* qd_mask_row(ray_t* mask, int64_t row, bool flag) {
    if (!mask) {
        if (!flag) return NULL;
        mask = ray_vec_new(RAY_BOOL, row + 1);
        uint8_t z = 0;
        for (int64_t i = 0; i < row && mask && !RAY_IS_ERR(mask); i++) mask = ray_vec_append(mask, &z);
    }
    uint8_t f = flag;
    return mask && !RAY_IS_ERR(mask) ? ray_vec_append(mask, &f) : mask;
}
/* n elements of one DuckDB vector from row `from` -> the accumulating q column
 * (moved), or error.  THE read cell codec — every LIST child level lands here
 * too, so the epoch shifts and the null law have one home: a sentinel carrier
 * nulls in-band and a data value on the null pattern is a `hit`; a null-less one
 * (bool/byte/symbol/string/blob) takes the fill on NULL.  Either group's rare
 * state flags the row in *mask — NULL mask = no row to flag (a LIST child), 'duckdb. */
static ray_t* qd_read_fail(int slot, ray_t* col, const qd_tmap_t* tm, const char* why) {
    ray_release(col);
    return qd_fail(slot, tm->logical, why);
}

static ray_t* qd_read_leaf(int slot, ray_t* col, const qd_tmap_t* tm,
                           duck_vector dv, duck_idx_t from, duck_idx_t n, ray_t** mask) {
    void*     data     = QAPI.vector_get_data(dv);
    uint64_t* validity = QAPI.vector_get_validity(dv);
    const bool nullless = qd_nullless(tm);
    for (duck_idx_t r = from; r < from + n; r++) {
        bool ok = q_duckdb_validity_ok(validity, r), hit = false;
        switch (tm->ray_type) {
            case RAY_BOOL:
            case RAY_BYTE_ONLY: {   /* fills 0b / 0x00 */
                uint8_t v = ok ? ((uint8_t*)data)[r] : 0;
                if (tm->ray_type == RAY_BOOL) v = v ? 1 : 0;
                col = ray_vec_append(col, &v);
                break;
            }
            case RAY_I16: {      /* TINYINT widens losslessly to q short */
                int16_t v = tm->dk_type == QDUCK_TYPE_TINYINT
                                ? (int16_t)((int8_t*)data)[r]
                                : ((int16_t*)data)[r];
                if (!ok || (hit = v == NULL_I16)) col = qd_append_null(col);
                else                              col = ray_vec_append(col, &v);
                break;
            }
            case RAY_I32:
            case RAY_MONTH:
            case RAY_MINUTE:
            case RAY_SECOND: {   /* month/minute/second: raw int32 counts */
                int32_t v = ((int32_t*)data)[r];
                if (!ok || (hit = v == NULL_I32)) col = qd_append_null(col);
                else                              col = ray_vec_append(col, &v);
                break;
            }
            case RAY_I64:
            case RAY_TIMESPAN: { /* timespan: raw ns rides BIGINT */
                int64_t v = ((int64_t*)data)[r];
                if (!ok || (hit = v == NULL_I64)) col = qd_append_null(col);
                else                              col = ray_vec_append(col, &v);
                break;
            }
            case RAY_F32: {      /* live-infinity: ONLY NaN is null */
                float v = ((float*)data)[r];
                if (!ok || (hit = v != v)) col = qd_append_null(col);
                else                       col = ray_vec_append(col, &v);
                break;
            }
            case RAY_F64:
            case RAY_DATETIME: { /* datetime: raw float days ride DOUBLE */
                double v = ((double*)data)[r];
                if (!ok || (hit = v != v)) col = qd_append_null(col);
                else                       col = ray_vec_append(col, &v);
                break;
            }
            case RAY_SYM: {      /* descriptor-refined VARCHAR: intern; the fill is ` */
                const duck_string_t* s = &((const duck_string_t*)data)[r];
                int64_t id = ok ? ray_sym_intern_runtime(q_duckdb_string_data(s),
                                                         q_duckdb_string_len(s))
                                : ray_sym_intern_runtime("", 0);
                col = ray_vec_append(col, &id);
                break;
            }
            case RAY_STR:        /* VARCHAR -> charv cell, BLOB -> byte-vector cell; the fill is the empty one */
            case RAY_LIST: {
                const duck_string_t* s = ok ? &((const duck_string_t*)data)[r] : NULL;
                const char* p   = s ? q_duckdb_string_data(s) : "";
                int64_t     len = s ? (int64_t)q_duckdb_string_len(s) : 0;
                ray_t* cell = tm->ray_type == RAY_STR ? ray_charv(p, len)
                            : len ? ray_vec_from_raw(RAY_BYTE_ONLY, p, len) : ray_vec_new(RAY_BYTE_ONLY, 1);
                if (!cell || RAY_IS_ERR(cell)) { ray_release(col);
                    return cell ? cell : q_err(QE_WSFULL); }
                col = ray_list_append(col, cell);   /* retains */
                ray_release(cell);
                break;
            }
            case RAY_DATE: {
                int32_t v = ((int32_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                if (v < INT32_MIN + QD_EPOCH_DAYS)
                    return qd_read_fail(slot, col, tm, "below the q epoch shift");
                int32_t q = v - QD_EPOCH_DAYS;
                col = (hit = q == NULL_I32) ? qd_append_null(col) : ray_vec_append(col, &q);
                break;
            }
            case RAY_TIME: {     /* µs -> ms; sub-ms is unrepresentable, not
                                  * truncatable — "exact else error" (codex) */
                int64_t v = ((int64_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                if (v % 1000) return qd_read_fail(slot, col, tm, "sub-millisecond value");
                int32_t q = (int32_t)(v / 1000);
                col = ray_vec_append(col, &q);
                break;
            }
            case RAY_TIMESTAMP: {  /* int64 ns epoch 1970 -> epoch 2000 */
                int64_t v = ((int64_t*)data)[r];
                if (!ok || v == NULL_I64) { col = qd_append_null(col); break; }
                if (tm->dk_type == QDUCK_TYPE_TIMESTAMP) {   /* µs: exact else error */
                    if (v > INT64_MAX / 1000 || v < INT64_MIN / 1000)
                        return qd_read_fail(slot, col, tm, "outside the nanosecond window");
                    v *= 1000;
                }
                if (v < INT64_MIN + QD_EPOCH_NS)
                    return qd_read_fail(slot, col, tm, "below the q epoch shift");
                int64_t q = v - QD_EPOCH_NS;
                col = (hit = q == NULL_I64) ? qd_append_null(col) : ray_vec_append(col, &q);
                break;
            }
            case RAY_GUID: {
                if (!ok) { col = qd_append_null(col); break; }   /* NULL -> 0Ng */
                uint8_t b[16];
                qd_uuid_to_bytes(((const duck_hugeint*)data)[r], b);
                hit = qd_guid_is_null(b);
                col = ray_vec_append(col, b);
                break;
            }
            default:
                ray_release(col);
                return q_err(QE_DUCKDB);
        }
        if (!col || RAY_IS_ERR(col))
            return col ? col : q_err(QE_WSFULL);
        bool flag = nullless ? !ok : hit;
        if (!mask && flag)
            return qd_read_fail(slot, col, tm, nullless ? "NULL element has no row to flag"
                                                        : "element on the null pattern has no row to flag");
        if (!mask) continue;
        if (*mask || flag) *mask = qd_mask_row(*mask, col->len - 1, flag);
        if (*mask && RAY_IS_ERR(*mask)) { ray_release(col); ray_t* e = *mask; *mask = NULL; return e; }
    }
    return col;
}

/* A NULL LIST cell takes the empty fill and flags the row (a NULL child list has no row: 'duckdb),
 * so it and an EMPTY list survive the crossing as the distinct values they are. */
static ray_t* qd_read_col(int slot, ray_t* col, const qd_colmap_t* cm,
                          duck_vector dv, duck_idx_t from, duck_idx_t n, ray_t** mask) {
    if (cm->depth == 0) return qd_read_leaf(slot, col, cm->leaf, dv, from, n, mask);
    const qd_colmap_t child = { cm->leaf, cm->depth - 1 };
    const duck_list_entry* ent = (const duck_list_entry*)QAPI.vector_get_data(dv);
    uint64_t*   validity = QAPI.vector_get_validity(dv);
    duck_vector cv       = QAPI.list_vector_get_child(dv);
    if (!ent || !cv) { ray_release(col); return q_err(QE_DUCKDB); }
    for (duck_idx_t r = from; r < from + n; r++) {
        bool       ok  = q_duckdb_validity_ok(validity, r);
        if (!ok && !mask) return qd_read_fail(slot, col, cm->leaf, "NULL list element has no row to flag");
        duck_idx_t len = ok ? (duck_idx_t)ent[r].length : 0;
        ray_t*     cell = qd_new_col(&child, (int64_t)len);
        if (ok && cell && !RAY_IS_ERR(cell))
            cell = qd_read_col(slot, cell, &child, cv, (duck_idx_t)ent[r].offset, len, NULL);
        if (!cell || RAY_IS_ERR(cell)) {
            ray_release(col);
            return cell ? cell : q_err(QE_WSFULL);
        }
        col = ray_list_append(col, cell);   /* retains */
        ray_release(cell);
        if (!col || RAY_IS_ERR(col)) return col ? col : q_err(QE_WSFULL);
        if (mask && (*mask || !ok)) *mask = qd_mask_row(*mask, col->len - 1, !ok);
        if (mask && *mask && RAY_IS_ERR(*mask)) { ray_release(col); ray_t* e = *mask; *mask = NULL; return e; }
    }
    return col;
}
/* Descriptor refinement: a row refines the physical map IFF its logical exists
 * AND agrees on both carrier and LIST depth; anything else DEGRADES. */
void qd_refine(const char* cname, const qd_desc_t* desc, int64_t ndesc,
                      qd_colmap_t* cm) {
    for (int64_t i = 0; i < ndesc; i++) {
        if (strcmp(desc[i].col, cname) != 0) continue;
        qd_colmap_t ref;
        if (qd_parse_logical(desc[i].logical, &ref) && ref.depth == cm->depth &&
            ref.leaf->dk_type == cm->leaf->dk_type)
            *cm = ref;
        return;
    }
}

/* duck_result -> q table (does NOT destroy the result); desc = _q_schema rows. */
ray_t* qd_result_to_table(int slot, duck_result* res, const qd_desc_t* desc,
                                 int64_t ndesc) {
    int64_t ncols = (int64_t)QAPI.column_count(res);
    if (ncols == 0) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    if (ncols > 256) return q_err(QE_DUCKDB);

    qd_colmap_t cms[256];
    for (int64_t c = 0; c < ncols; c++) {
        duck_logical_type lt = QAPI.column_logical_type(res, (duck_idx_t)c);
        duck_type miss = 0;
        bool ok = lt && qd_map_read_logical(lt, &cms[c], &miss);
        if (lt) QAPI.destroy_logical_type(&lt);
        if (!ok) {
            qd_err_stash(slot, "column %s: %s has no mapping", QAPI.column_name(res, (duck_idx_t)c), q_duckdb_type_name(miss));
            return q_err(QE_DUCKDB);
        }
        qd_refine(QAPI.column_name(res, (duck_idx_t)c), desc, ndesc, &cms[c]);
    }

    ray_t* cols[256], *masks[256] = { NULL };
    for (int64_t c = 0; c < ncols; c++) {
        cols[c] = qd_new_col(&cms[c], 8);
        if (!cols[c] || RAY_IS_ERR(cols[c])) {
            for (int64_t k = 0; k < c; k++) ray_release(cols[k]);
            return cols[c] ? cols[c] : q_err(QE_WSFULL);
        }
    }

    ray_t* err = NULL;
    duck_data_chunk chunk;
    while (!err && (chunk = QAPI.fetch_chunk(*res)) != NULL) {
        duck_idx_t n = QAPI.data_chunk_get_size(chunk);
        for (int64_t c = 0; c < ncols && !err; c++) {
            duck_vector dv = QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c);
            cols[c] = qd_read_col(slot, cols[c], &cms[c], dv, 0, n, &masks[c]);
            if (!cols[c] || RAY_IS_ERR(cols[c])) { err = cols[c] ? cols[c] : q_err(QE_WSFULL); cols[c] = NULL; }
        }
        QAPI.destroy_data_chunk(&chunk);
    }

    ray_t* tbl = err ? NULL : ray_table_new(ncols);
    for (int64_t c = 0; c < ncols; c++) {
        const char* nm = QAPI.column_name(res, (duck_idx_t)c);
        if (tbl && !RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(nm, strlen(nm)), cols[c]);
        if (tbl && !RAY_IS_ERR(tbl) && masks[c]) {
            char cn[600];
            int  cl = snprintf(cn, sizeof cn, "%s%s", nm, qd_cm_nullless(&cms[c]) ? QD_ISNULL_SUFFIX : QD_NOTNULL_SUFFIX);
            if (cl >= (int)sizeof cn) { ray_release(tbl); tbl = NULL; err = q_err(QE_DUCKDB); }
            else tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(cn, (size_t)cl), masks[c]);
        }
        if (cols[c])  ray_release(cols[c]);    /* add_col retains its own ref */
        if (masks[c]) ray_release(masks[c]);
    }
    if (err) return err;
    return tbl ? tbl : q_err(QE_WSFULL);
}
static void qd_set_invalid(duck_vector dv, duck_idx_t r) {
    QAPI.vector_ensure_validity_writable(dv);
    QAPI.validity_set_row_invalid(QAPI.vector_get_validity(dv), r);
}

/* THE write cell codec: n elements of col from `base` into dv rows from `dst`.  A sentinel
 * carrier's null-patterned cell writes NULL unless `keep` (its _q_notnull companion) flags the
 * row, when the raw bits go through as the value they were. */
static ray_t* qd_write_leaf(int slot, duck_vector dv, ray_t* col, const qd_tmap_t* tm,
                            int64_t base, duck_idx_t dst, int64_t n, ray_t* keep) {
    void* data = QAPI.vector_get_data(dv);
    for (int64_t i = 0; i < n; i++) {
        int64_t src = base + i;
        duck_idx_t r = dst + (duck_idx_t)i;
        bool kp = keep && *(uint8_t*)ray_vec_get(keep, src);
        switch (tm->ray_type) {
            case RAY_BOOL:
                ((uint8_t*)data)[r] = *(uint8_t*)ray_vec_get(col, src) ? 1 : 0;
                break;
            case RAY_BYTE_ONLY:
                ((uint8_t*)data)[r] = *(uint8_t*)ray_vec_get(col, src);
                break;
            case RAY_I16: {
                int16_t v = *(int16_t*)ray_vec_get(col, src);
                if (v == NULL_I16 && !kp) qd_set_invalid(dv, r);
                else                      ((int16_t*)data)[r] = v;
                break;
            }
            case RAY_I32:
            case RAY_MONTH:
            case RAY_MINUTE:
            case RAY_SECOND: {
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32 && !kp) qd_set_invalid(dv, r);
                else                      ((int32_t*)data)[r] = v;
                break;
            }
            case RAY_I64:
            case RAY_TIMESPAN: {
                int64_t v = *(int64_t*)ray_vec_get(col, src);
                if (v == NULL_I64 && !kp) qd_set_invalid(dv, r);
                else                      ((int64_t*)data)[r] = v;
                break;
            }
            case RAY_F32: {
                float v = *(float*)ray_vec_get(col, src);
                if (v != v && !kp) qd_set_invalid(dv, r);       /* 0Ne -> NULL */
                else               ((float*)data)[r] = v;
                break;
            }
            case RAY_F64:
            case RAY_DATETIME: {
                double v = *(double*)ray_vec_get(col, src);
                if (v != v && !kp) qd_set_invalid(dv, r);       /* 0n/0Nz -> NULL */
                else               ((double*)data)[r] = v;
                break;
            }
            case RAY_SYM: {      /* ` (sym 0 / empty) writes '' — a value */
                ray_t* s = ray_sym_vec_cell(col, src);   /* borrowed */
                QAPI.vector_assign_string_element_len(dv, r,
                        s ? ray_str_ptr(s) : "", s ? (duck_idx_t)ray_str_len(s) : 0);
                break;
            }
            case RAY_STR: {      /* string column: text cells, or physical STR */
                if (col->type == RAY_LIST) {
                    ray_t* cell = ray_list_get(col, src);
                    const char* tp; int64_t tn;
                    if (qd_cell_is_0n(cell)) { qd_set_invalid(dv, r); break; }
                    if (!cell || !q_str_text_bytes(cell, &tp, &tn))
                        return qd_fail(slot, tm->logical, "cell is not text");
                    QAPI.vector_assign_string_element_len(dv, r, tp ? tp : "",
                                                          (duck_idx_t)tn);
                    break;
                }
                size_t len = 0;
                const char* sp = ray_str_vec_get(col, src, &len);
                QAPI.vector_assign_string_element_len(dv, r, sp ? sp : "",
                                                      (duck_idx_t)len);
                break;
            }
            case RAY_LIST: {     /* byte-vector cell -> BLOB */
                ray_t* cell = ray_list_get(col, src);
                if (qd_cell_is_0n(cell)) { qd_set_invalid(dv, r); break; }
                if (!cell || cell->type != RAY_BYTE_ONLY)
                    return qd_fail(slot, tm->logical, "cell is not a byte vector");
                const char* bp = cell->len ? (const char*)ray_vec_get(cell, 0) : "";
                QAPI.vector_assign_string_element_len(dv, r, bp,
                                                      (duck_idx_t)cell->len);
                break;
            }
            case RAY_DATE: {
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32 && !kp) { qd_set_invalid(dv, r); break; }
                if (v > INT32_MAX - QD_EPOCH_DAYS)
                    return qd_fail(slot, tm->logical, "above the q epoch shift");
                ((int32_t*)data)[r] = v + QD_EPOCH_DAYS;
                break;
            }
            case RAY_TIME: {     /* 0Nt has no TIME value behind it: a kept one is 'duckdb, never a bogus µs */
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32 && kp) return qd_fail(slot, tm->logical, "a kept 0Nt has no TIME value");
                if (v == NULL_I32) qd_set_invalid(dv, r);
                else               ((int64_t*)data)[r] = (int64_t)v * 1000;  /* ms -> µs */
                break;
            }
            case RAY_TIMESTAMP: {
                int64_t v = *(int64_t*)ray_vec_get(col, src);
                if (v == NULL_I64 && !kp) { qd_set_invalid(dv, r); break; }
                if (v > INT64_MAX - QD_EPOCH_NS)
                    return qd_fail(slot, tm->logical, "above the q epoch shift");
                ((int64_t*)data)[r] = v + QD_EPOCH_NS;
                break;
            }
            case RAY_GUID: {
                const uint8_t* b = (const uint8_t*)ray_vec_get(col, src);
                if (qd_guid_is_null(b) && !kp) qd_set_invalid(dv, r);   /* 0Ng -> NULL */
                else ((duck_hugeint*)data)[r] = qd_bytes_to_uuid(b);
                break;
            }
            default:
                return q_err(QE_DUCKDB);
        }
    }
    return NULL;
}

/* One vector per LIST level, plus how much of it is already filled. */
typedef struct { duck_vector vec; duck_idx_t used; } qd_level_t;

/* count[l] += the flattened element count level l takes from this run. */
static ray_t* qd_count_levels(ray_t* col, int depth, int64_t base, int64_t n,
                              int64_t* count) {
    for (int64_t i = 0; i < n; i++) {
        ray_t* cell = ray_list_get(col, base + i);
        if (qd_cell_is_0n(cell)) continue;
        if (!cell || cell->type < 0) return q_err(QE_DUCKDB);
        int64_t len = ray_len(cell);
        count[depth - 1] += len;
        if (depth > 1) {
            ray_t* e = qd_count_levels(cell, depth - 1, 0, len, count);
            if (e) return e;
        }
    }
    return NULL;
}

static ray_t* qd_write_nested(int slot, qd_level_t* lv, const qd_colmap_t* cm, int level,
                              ray_t* col, int64_t base, duck_idx_t dst, int64_t n) {
    duck_list_entry* ent = (duck_list_entry*)QAPI.vector_get_data(lv[level].vec);
    if (!ent) return q_err(QE_DUCKDB);
    for (int64_t i = 0; i < n; i++) {
        ray_t* cell = ray_list_get(col, base + i);
        if (!cell) return q_err(QE_DUCKDB);
        duck_idx_t off = lv[level - 1].used;
        if (qd_cell_is_0n(cell)) {
            ent[dst + (duck_idx_t)i].offset = off;
            ent[dst + (duck_idx_t)i].length = 0;
            qd_set_invalid(lv[level].vec, dst + (duck_idx_t)i);
            continue;
        }
        int64_t    len = ray_len(cell);
        ent[dst + (duck_idx_t)i].offset = off;
        ent[dst + (duck_idx_t)i].length = (uint64_t)len;
        lv[level - 1].used += (duck_idx_t)len;
        ray_t* e = level > 1
                       ? qd_write_nested(slot, lv, cm, level - 1, cell, 0, off, len)
                       : qd_write_leaf(slot, lv[0].vec, cell, cm->leaf, 0, off, len, NULL);
        if (e) return e;
    }
    return NULL;
}

/* Every LIST level is sized BEFORE any data pointer is taken: reserve
 * reallocates the child, so a half-written spine would dangle. */
static ray_t* qd_write_data(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm,
                            int64_t base, int64_t n, ray_t* keep) {
    if (cm->depth == 0) return qd_write_leaf(slot, dv, col, cm->leaf, base, 0, n, keep);
    int64_t count[QD_MAX_DEPTH] = { 0 };
    ray_t*  e = qd_count_levels(col, cm->depth, base, n, count);
    if (e) return e;
    qd_level_t lv[QD_MAX_DEPTH + 1];
    lv[cm->depth].vec  = dv;
    lv[cm->depth].used = 0;
    for (int l = cm->depth - 1; l >= 0; l--) {
        if (QAPI.list_vector_reserve(lv[l + 1].vec, (duck_idx_t)count[l]) != QDuckSuccess ||
            QAPI.list_vector_set_size(lv[l + 1].vec, (duck_idx_t)count[l]) != QDuckSuccess)
            return q_err(QE_DUCKDB);
        lv[l].vec  = QAPI.list_vector_get_child(lv[l + 1].vec);
        lv[l].used = 0;
        if (!lv[l].vec) return q_err(QE_DUCKDB);
    }
    return qd_write_nested(slot, lv, cm, cm->depth, col, base, 0, n);
}

/* The companion's meaning follows the column's group (the strip has matched them): a null-less
 * column's mask spells NULL over the written fill, a sentinel carrier's rides into the leaf as `keep`. */
static ray_t* qd_write_col(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm,
                           int64_t base, int64_t n, ray_t* mask) {
    bool isnull = mask && qd_cm_nullless(cm);
    ray_t* e = qd_write_data(slot, dv, col, cm, base, n, isnull ? NULL : mask);
    for (int64_t i = 0; !e && isnull && i < n; i++)
        if (*(uint8_t*)ray_vec_get(mask, base + i)) qd_set_invalid(dv, (duck_idx_t)i);
    return e;
}

/* Append a q table through the appender in vector-size chunks (cms[] pre-validated,
 * masks[] = the consumed companions, NULL where a column has none). */
ray_t* qd_append_table(int slot, const char* tname, ray_t* tbl,
                              const qd_colmap_t* cms, ray_t* const* masks) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    duck_appender app = NULL;
    if (QAPI.appender_create_ext(qd_con(slot), NULL, "main", tname, &app)
            != QDuckSuccess) {
        qd_err_stash(slot, "%s", QD_TEXT(QAPI.appender_error(app)));
        QAPI.appender_destroy(&app);
        return q_err(QE_DUCKDB);
    }

    duck_logical_type ltypes[256];
    for (int64_t c = 0; c < ncols; c++)
        ltypes[c] = qd_make_logical(&cms[c]);
    duck_data_chunk chunk = QAPI.create_data_chunk(ltypes, (duck_idx_t)ncols);

    ray_t* err = NULL;
    int64_t vecsz = (int64_t)QAPI.vector_size();
    if (vecsz <= 0) vecsz = 2048;
    for (int64_t base = 0; base < nrows && !err; base += vecsz) {
        int64_t n = nrows - base < vecsz ? nrows - base : vecsz;
        QAPI.data_chunk_reset(chunk);
        for (int64_t c = 0; c < ncols && !err; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
            err = qd_write_col(slot, QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c),
                               col, &cms[c], base, n, masks[c]);
        }
        if (!err) {
            QAPI.data_chunk_set_size(chunk, (duck_idx_t)n);
            if (QAPI.append_data_chunk(app, chunk) != QDuckSuccess) {
                qd_err_stash(slot, "%s", QD_TEXT(QAPI.appender_error(app)));
                err = q_err(QE_DUCKDB);
            }
        }
    }

    QAPI.destroy_data_chunk(&chunk);
    for (int64_t c = 0; c < ncols; c++) QAPI.destroy_logical_type(&ltypes[c]);

    /* explicit flush first: destroy frees the error text (codex round 3) */
    if (!err && QAPI.appender_flush(app) != QDuckSuccess) {
        qd_err_stash(slot, "%s", QD_TEXT(QAPI.appender_error(app)));
        err = q_err(QE_DUCKDB);
    }
    if (QAPI.appender_destroy(&app) != QDuckSuccess && !err)
        err = q_err(QE_DUCKDB);
    return err;
}

/* Validate every column against the write manifest; fills tms.  Overlong names
 * would silently truncate in qd_desc_t and degrade refinement — rejected. */
ray_t* qd_check_table(ray_t* tbl, qd_colmap_t* cms) {
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols > 256) return q_err(QE_DUCKDB);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
        ray_t* nm  = ray_sym_str(ray_table_col_name(tbl, c));
        if (nm && ray_str_len(nm) >= 256) return q_err(QE_DUCKDB);
        if (!col || !qd_map_write(col, 0, &cms[c])) return q_err(QE_DUCKDB);
    }
    return NULL;
}

static ray_t* qd_reject(int slot, ray_t* nm, const char* why) {
    char what[300];
    snprintf(what, sizeof what, "companion %.*s", (int)ray_str_len(nm), ray_str_ptr(nm));
    return qd_fail(slot, what, why);
}

/* Peel the companions off a q table (ADR 1–2): the store table (owned) with masks[] (borrowed
 * from tbl, NULL = none) and iskey compacted alongside.  A companion is a boolean column whose
 * suffix is its parent's group's, the parent not itself one — anything else is 'duckdb, reason stashed. */
ray_t* qd_strip_companions(int slot, ray_t* tbl, ray_t** masks, bool* iskey) {
    int64_t ncols = ray_table_ncols(tbl), nkeep = 0, parent[256], at[256];
    if (ncols > 256) return q_err(QE_DUCKDB);
    static const char* const sfx[2] = { QD_ISNULL_SUFFIX, QD_NOTNULL_SUFFIX };
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        size_t n  = nm ? ray_str_len(nm) : 0, sl = 0;
        int    notnull = -1;
        for (int k = 0; k < 2 && notnull < 0; k++) {
            sl = strlen(sfx[k]);
            if (n >= sl && memcmp(ray_str_ptr(nm) + n - sl, sfx[k], sl) == 0) notnull = k;
        }
        parent[c] = -1;
        if (notnull < 0) { nkeep++; continue; }
        int64_t pid = ray_sym_intern_runtime(ray_str_ptr(nm), n - sl);
        for (int64_t p = 0; p < ncols; p++) if (ray_table_col_name(tbl, p) == pid) parent[c] = p;
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (parent[c] < 0) return qd_reject(slot, nm, "no parent column");
        if (!col || col->type != RAY_BOOL) return qd_reject(slot, nm, "not a boolean column");
        ray_t* pcol = ray_table_get_col_idx(tbl, parent[c]);
        if (!pcol || pcol->len != col->len) return qd_reject(slot, nm, "row count differs from its parent");
        qd_colmap_t pcm;
        if (!qd_map_write(pcol, 0, &pcm)) return qd_reject(slot, nm, "parent has no mapping");
        if (qd_cm_nullless(&pcm) == (bool)notnull)
            return qd_reject(slot, nm, notnull ? "parent is null-less, its companion is _q_isnull"
                                               : "parent nulls in-band, its companion is _q_notnull");
    }
    ray_t* out = ray_table_new(nkeep);
    for (int64_t c = 0, k = 0; c < ncols && out && !RAY_IS_ERR(out); c++) {
        if (parent[c] >= 0) continue;
        at[c] = k;
        masks[k] = NULL;
        if (iskey) iskey[k] = iskey[c];
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), ray_table_get_col_idx(tbl, c));  /* retains */
        k++;
    }
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_WSFULL);
    for (int64_t c = 0; c < ncols; c++) {
        if (parent[c] < 0) continue;
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));
        if (parent[parent[c]] >= 0) { ray_release(out); return qd_reject(slot, nm, "parent is itself a companion"); }
        if (masks[at[parent[c]]])   { ray_release(out); return qd_reject(slot, nm, "parent already has a companion"); }
        masks[at[parent[c]]] = ray_table_get_col_idx(tbl, c);
    }
    return out;
}

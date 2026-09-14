/* q_duckdb_codec — the type contract in BOTH directions: the column map over
 * QD_TYPES[], the null law and its boolean companions, and the read/write cell
 * codecs.  Read and write share the map and the null law, so they share a home.
 * Contract and decisions: docs/duckdb-api.md. */
#include "qlang/io/q_duckdb_internal.h"
#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_types.h"
#include "qlang/base/q_err.h"
#include "qlang/q_prim.h"     /* q_str_text_bytes, q_enum_resolve, q_table_row_at + q_list_collapse (record cells) */
#include "qlang/base/q_calendar.h"  /* q_calendar_month_from_days — the DATE a q month stores as */
#include "qlang/q_dotz.h"           /* q_dotz_now_ns — the appender's sqllog clock */
#include "lang/internal.h"          /* month_payload_as_days — THE forward half, base's own */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <math.h>       /* isinf / llround — the datetime <-> TIMESTAMP leg */
#include <stdio.h>
#include <stdlib.h>
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
int8_t q_duckdb_codec_surface_of(const qd_colmap_t* cm) {
    return cm->depth ? RAY_LIST : qd_surface_type(cm->leaf);
}

/* The three nested RECORDS (ADR 12): one carrier — a q DICT per cell — three spellings, and only the declared
 * type says which of them a dict was.  A field's NULL rides the parent's SHAPE MIRROR (ADR 16), never a key
 * inside the data dict, so a nested null needs no new spelling and no field name is reserved. */
bool q_duckdb_codec_is_rec(const qd_tmap_t* tm) {
    return tm && (tm->dk_type == QDUCK_TYPE_STRUCT || tm->dk_type == QDUCK_TYPE_MAP ||
                  tm->dk_type == QDUCK_TYPE_UNION);
}

void q_duckdb_codec_maps_free(qd_colmap_t* cms, int64_t n) {
    for (int64_t c = 0; c < n; c++) q_duckdb_codec_map_free(&cms[c]);
}

void q_duckdb_codec_map_free(qd_colmap_t* cm) {
    if (!cm || !cm->rec) return;
    qd_rec_t* r = (qd_rec_t*)cm->rec;
    for (int i = 0; i < r->n; i++) { free(r->f[i].name); q_duckdb_codec_map_free(&r->f[i].map); }
    free(r);
    cm->rec = NULL;
}

static qd_rec_t* qd_rec_alloc(int n) {
    qd_rec_t* r = calloc(1, sizeof *r + (size_t)n * sizeof(qd_field_t));
    if (r) r->n = n;
    return r;
}

/* The DuckDB declaration a column map spells — a leaf's DDL name, a record's fields inside it, `[]` per LIST
 * level.  THE speller of a staged column's type: the CREATE DDL, the cast leg's `from` and the error text. */
void q_duckdb_codec_spell_map(const qd_colmap_t* cm, qd_buf* b) {
    q_duckdb_puts(b, cm->leaf->sql);
    if (cm->rec) {
        bool named = cm->leaf->dk_type != QDUCK_TYPE_MAP;   /* a MAP declares two types, not two names */
        q_duckdb_puts(b, "(");
        for (int i = 0; i < cm->rec->n; i++) {
            if (i) q_duckdb_puts(b, ", ");
            if (named) {
                q_duckdb_put_ident(b, cm->rec->f[i].name, strlen(cm->rec->f[i].name));
                q_duckdb_puts(b, " ");
            }
            q_duckdb_codec_spell_map(&cm->rec->f[i].map, b);
        }
        q_duckdb_puts(b, ")");
    }
    for (int l = 0; l < cm->depth; l++) q_duckdb_puts(b, "[]");
}

static ray_t* qd_rec_shell(const qd_colmap_t* cm, int kind);

static ray_t* qd_new_col(const qd_colmap_t* cm, int64_t cap) {
    if (cap < 1) cap = 1;
    if (cm->rec && !cm->depth) return qd_rec_shell(cm, QD_CO_NONE);
    if (q_duckdb_codec_surface_of(cm) == RAY_LIST) return ray_list_new(cap);
    if (cm->leaf->ray_type == RAY_SYM) return ray_sym_vec_new(RAY_SYM_W64, cap);
    return ray_vec_new(cm->leaf->ray_type, cap);
}

/* kdb spells a compound column with the UPPERCASE child char; a cell that is
 * itself a list (nested strings/bytes, depth >= 2) has no char at all. */
char q_duckdb_codec_meta_char(const qd_colmap_t* cm) {
    char c = cm->leaf->meta_ch;
    if (cm->depth == 0) return c;
    if (cm->depth > 1 || qd_surface_type(cm->leaf) == RAY_LIST) return ' ';
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

/* The parameterized-name grammar: list(list(utf8)). */
void q_duckdb_codec_logical_name(const qd_colmap_t* cm, char* buf, size_t cap) {
    size_t off = 0;
    for (int i = 0; i < cm->depth && off + 5 < cap; i++, off += 5) memcpy(buf + off, "list(", 5);
    size_t ln = strlen(cm->leaf->logical);
    if (off + ln < cap) { memcpy(buf + off, cm->leaf->logical, ln); off += ln; }
    for (int i = 0; i < cm->depth && off + 1 < cap; i++) buf[off++] = ')';
    buf[off] = '\0';
}

bool q_duckdb_codec_parse_logical(const char* s, qd_colmap_t* out) {
    size_t n = strlen(s);
    int depth = 0;
    while (depth < QD_MAX_DEPTH && n > 6 && s[n - 1] == ')' &&
           memcmp(s, "list(", 5) == 0) { s += 5; n -= 6; depth++; }
    const qd_tmap_t* leaf = qd_map_logical(s, n);
    if (!leaf) return false;
    /* a row written before 2026-09-14 says `enum`; the store held its symbols then as now (ADR 8) */
    if (leaf->ray_type == RAY_ENUM) leaf = q_duckdb_codec_canon_leaf(RAY_SYM);
    out->leaf  = leaf;
    out->rec   = NULL;
    out->depth = depth;
    out->undet = false;
    return true;
}

/* The canonical row for a q carrier — what a write derives from the q type alone (the first manifest row). */
const qd_tmap_t* q_duckdb_codec_canon_leaf(int8_t ray_type) {
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (QD_TYPES[i].ray_type == ray_type) return &QD_TYPES[i];
    return NULL;
}

/* ADR 19: a raw companion over a q carrier is DuckDB's own TEXT where no single count carries the value.  An
 * INTERVAL is three independent fields, so a months-bearing one — no fixed duration, and no q carrier at all —
 * rides its spelling; every other raw stays the long its temporal's count is. */
/* Which raw companions are DuckDB's own TEXT rather than a count: an INTERVAL bearing months (ADR 19) and a BIGNUM,
 * whose digits no fixed-width carrier holds (ADR 21).  A `j` carrier's OTHER companion is the 128-bit high word,
 * which wears the `hi` name, so the two never contend for a spelling. */
bool q_duckdb_codec_raw_is_text(int8_t ray_type) { return ray_type == RAY_TIMESPAN || ray_type == RAY_I64; }

static bool qd_rec_map(int slot, ray_t* v, int depth, qd_colmap_t* out);
static ray_t* qd_rec_spine(int slot, ray_t* v, int lev);

/* q value -> column map (write direction).  A generic list is utf8 when EVERY
 * cell is text and bytes when every cell is a byte vector — those base cases
 * consume the level; a DICT or TABLE cell makes it a record; otherwise it
 * nests, and every cell must agree. */
static bool qd_map_write(int slot, ray_t* v, int depth, qd_colmap_t* out, const char** why) {
    if (!v) return false;
    out->rec = NULL;
    out->undet = false;
    /* q collapses a run of like dicts into a TABLE, so a table-valued column is a dict per row and the SAME
     * record carrier — at depth 0 the column itself, under a LIST one cell of it */
    if (v->type == RAY_TABLE) return qd_rec_map(slot, v, depth, out);
    if (v->type != RAY_LIST) {
        out->leaf  = q_duckdb_codec_canon_leaf(v->type);
        out->depth = depth;
        return out->leaf != NULL;
    }
    bool all_bytes = true, all_text = true, all_dict = v->len > 0, atom = false, hole = false;
    int64_t voted = 0;
    for (int64_t i = 0; i < v->len && (all_bytes || all_text || all_dict); i++) {
        ray_t* cell = ray_list_get(v, i);
        if (!cell) return false;
        if (cell->type != RAY_DICT) all_dict = false;
        if (qd_cell_is_0n(cell)) continue;
        voted++;
        if (cell->type != RAY_BYTE_ONLY) all_bytes = false;
        if (!qd_cell_is_text(cell))      all_text  = false;
        atom |= cell->type == -RAY_CHARV;
        hole |= cell->type == RAY_LIST && cell->len == 0;
    }
    if (all_dict) return qd_rec_map(slot, v, depth, out);
    /* a char ATOM is text to the vote but no string comes back for it, and a bare () beside strings would land as
     * VARCHAR[] of single characters: both are refused, never guessed (2026-09-14) */
    if (all_text && atom) { if (why) *why = "a char atom among strings"; return false; }
    if (!all_text && hole && voted) {
        bool strings = false;
        for (int64_t i = 0; i < v->len && !strings; i++) strings = qd_cell_is_text(ray_list_get(v, i));
        if (strings) { if (why) *why = "an empty list among strings"; return false; }
    }
    if (all_bytes || all_text || v->len == 0) {   /* all-null classifies as VARCHAR */
        int8_t want = (v->len == 0 || (voted && all_bytes)) ? RAY_LIST : RAY_STR;
        for (size_t i = 0; i < QD_NTYPES; i++)
            if (QD_TYPES[i].ray_type == want && QD_TYPES[i].read_canon) {
                out->leaf = &QD_TYPES[i];
                out->depth = depth;
                /* an emptied string, nested-list and list-of-strings column are the SAME value, (): the BLOB
                 * this lands on is a placeholder for whatever declares it, never an answer of its own */
                out->undet = v->len == 0 && depth == 0;
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
        if (!qd_map_write(slot, cell, depth + 1, &cm, why)) { q_duckdb_codec_map_free(&cm); return false; }
        /* a record UNDER a list declares the fields ALL its rows agree on, so the rows are gathered into the one
         * column qd_rec_map reads them off — depth 0's own law, one flattening away */
        if (q_duckdb_codec_is_rec(cm.leaf)) {
            int    rd   = cm.depth;
            size_t mark = q_duckdb_err_mark();
            q_duckdb_codec_map_free(&cm);
            ray_t* rows = qd_rec_spine(slot, v, rd - depth);
            bool   ok   = rows && qd_rec_map(slot, rows, rd, out);
            if (rows && RAY_IS_ERR(rows)) q_duckdb_err_rewind(slot, mark);
            q_duckdb_drop(rows);
            return ok;
        }
        if (!have) { *out = cm; have = true; }
        else if (cm.leaf != out->leaf || cm.depth != out->depth || cm.rec || out->rec) {
            q_duckdb_codec_map_free(&cm);
            return false;
        }
    }
    return have;
}

static bool qd_map_read_logical(duck_logical_type lt, qd_colmap_t* out, duck_type* miss, const char** why);

/* A record logical type's fields: a STRUCT's children, a UNION's members, a MAP's key and value.  A MAP's
 * VARCHAR key reads as a q SYMBOL, because a dict key is a name.  A field MAY be named like a companion: the
 * mirror is parallel to the value (ADR 16), so no name inside a record is reserved. */
static bool qd_rec_read_type(duck_logical_type lt, duck_type id, qd_colmap_t* out, duck_type* miss,
                             const char** why) {
    int n = id == QDUCK_TYPE_MAP     ? 2
          : id == QDUCK_TYPE_UNION   ? (int)QAPI.union_type_member_count(lt)
                                     : (int)QAPI.struct_type_child_count(lt);
    if (n <= 0) return false;
    qd_rec_t* r = qd_rec_alloc(n);
    if (!r) return false;
    bool ok = true;
    for (int i = 0; ok && i < n; i++) {
        duck_logical_type ct = id == QDUCK_TYPE_MAP ? (i ? QAPI.map_type_value_type(lt) : QAPI.map_type_key_type(lt))
                             : id == QDUCK_TYPE_UNION ? QAPI.union_type_member_type(lt, (duck_idx_t)i)
                                                      : QAPI.struct_type_child_type(lt, (duck_idx_t)i);
        char* nm = id == QDUCK_TYPE_MAP ? NULL
                 : id == QDUCK_TYPE_UNION ? QAPI.union_type_member_name(lt, (duck_idx_t)i)
                                          : QAPI.struct_type_child_name(lt, (duck_idx_t)i);
        const char* fn = nm ? nm : i ? "value" : "key";
        r->f[i].name = q_duckdb_text(fn, strlen(fn));
        if (!r->f[i].name)
            { ok = false; *why = "no memory for the record's field names"; }
        else ok = ct && qd_map_read_logical(ct, &r->f[i].map, miss, why);
        if (ok && id == QDUCK_TYPE_MAP && i == 0 && !r->f[0].map.depth &&
            r->f[0].map.leaf->dk_type == QDUCK_TYPE_VARCHAR)
            r->f[0].map.leaf = qd_map_logical("symbol", 6);
        if (nm) QAPI.duck_free(nm);
        if (ct) QAPI.destroy_logical_type(&ct);
    }
    out->rec = r;
    if (ok) return true;
    q_duckdb_codec_map_free(out);
    return false;
}

/* false = no mapping, with *miss the DuckDB type id that had none and *why the reason where it is not the
 * bare "no mapping" */
static bool qd_map_read_logical(duck_logical_type lt, qd_colmap_t* out, duck_type* miss, const char** why) {
    duck_logical_type cur = lt, owned = NULL;
    int depth = 0;
    duck_type id;
    while (cur && ((id = QAPI.get_type_id(cur)) == QDUCK_TYPE_LIST || id == QDUCK_TYPE_ARRAY) && depth < QD_MAX_DEPTH) {
        duck_logical_type child = id == QDUCK_TYPE_LIST ? QAPI.list_type_child_type(cur) : QAPI.array_type_child_type(cur);
        if (owned) QAPI.destroy_logical_type(&owned);
        cur = owned = child;
        depth++;
    }
    *miss = cur ? QAPI.get_type_id(cur) : 0;
    const qd_tmap_t* leaf = cur ? qd_map_read(*miss) : NULL;
    out->leaf  = leaf;
    out->rec   = NULL;
    out->depth = depth;
    bool ok = leaf && !q_duckdb_codec_is_rec(leaf);
    if (leaf && !ok) ok = qd_rec_read_type(cur, *miss, out, miss, why);
    if (owned) QAPI.destroy_logical_type(&owned);
    return ok;
}

/* the DuckDB type a staged column is written into: a record builds its fields, everything else its leaf row */
static duck_logical_type qd_make_logical(const qd_colmap_t* cm) {
    duck_logical_type t;
    if (!cm->rec) t = QAPI.create_logical_type(cm->leaf->dk_type);
    else {
        int n = cm->rec->n, i = 0;
        duck_logical_type* ct = q_duckdb_cols(n, sizeof *ct + sizeof(char*));   /* the field types, then their names */
        const char**       nm = ct ? (const char**)(ct + n) : NULL;
        for (; ct && i < n; i++) {
            nm[i] = cm->rec->f[i].name;
            if (!(ct[i] = qd_make_logical(&cm->rec->f[i].map))) break;
        }
        t = i < n                                      ? NULL
          : cm->leaf->dk_type == QDUCK_TYPE_MAP        ? QAPI.create_map_type(ct[0], ct[1])
          : cm->leaf->dk_type == QDUCK_TYPE_UNION      ? QAPI.create_union_type(ct, nm, (duck_idx_t)i)
                                                       : QAPI.create_struct_type(ct, nm, (duck_idx_t)i);
        for (int k = 0; k < i; k++) QAPI.destroy_logical_type(&ct[k]);
        free(ct);
    }
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

/* ---- BIT <-> boolean vector (ADR 20).  The cell is a string_t: byte 0 is the PADDING count 0..7, then ceil(n/8)
 * data bytes read MSB-first, the padding in the HIGH bits of the first of them (DuckDB fills those with 1s, and a
 * write matches it so the stored bytes — which is what DuckDB compares and orders — are the ones it would write
 * itself).  The bit count is the value's identity, so it is the q cell's `count`. ---- */

/* There is no zero-bit BIT, so a cell with no data byte is malformed, not empty.  The padding bits themselves are
 * DuckDB's own filler and carry no value, so they are read as don't-care and written back at DuckDB's fill. */
static bool qd_bits_wellformed(const char* p, int64_t len) {
    return len > 1 && (uint8_t)p[0] <= 7 && (len - 1) * 8 > (uint8_t)p[0];
}

static int64_t qd_bits_count(const char* p, int64_t len) { return len ? (len - 1) * 8 - (uint8_t)p[0] : 0; }

static ray_t* qd_bits_read(const char* p, int64_t len) {
    int64_t pad = len ? (uint8_t)p[0] : 0, n = qd_bits_count(p, len);
    ray_t* cell = ray_vec_new(RAY_BOOL, n ? n : 1);
    for (int64_t i = 0, j = pad; cell && !RAY_IS_ERR(cell) && i < n; i++, j++) {
        uint8_t b = ((uint8_t)p[1 + j / 8] >> (7 - j % 8)) & 1;
        cell = ray_vec_append(cell, &b);
    }
    return cell;
}

static ray_t* qd_bits_write(duck_vector dv, duck_idx_t r, ray_t* cell) {
    int64_t n = cell->len, nb = (n + 7) / 8, pad = (8 - n % 8) % 8;
    uint8_t* buf = malloc((size_t)nb + 1);
    if (!buf) return q_err(QE_WSFULL);
    const uint8_t* src = (const uint8_t*)ray_vec_get(cell, 0);
    buf[0] = (uint8_t)pad;
    memset(buf + 1, 0xff, (size_t)nb);
    for (int64_t i = 0, j = pad; i < n; i++, j++) {
        uint8_t m = (uint8_t)(1u << (7 - j % 8));
        if (src[i]) buf[1 + j / 8] |= m;
        else        buf[1 + j / 8] &= (uint8_t)~m;
    }
    QAPI.vector_assign_string_element_len(dv, r, (const char*)buf, (duck_idx_t)(nb + 1));
    free(buf);
    return NULL;
}

/* The envelope's `bitstring` row over the map a nested boolean column derives: the ONE remap a declaration makes,
 * because DuckDB casts BOOLEAN[] neither to BIT nor back, so stage-then-cast has no leg.  `depth` is the LIST
 * depth the declaration puts over the bitstring, so a BIT[] eats one more level of booleans than a BIT does.  A
 * column with no rows classifies as bytes (nothing votes), so the empty BLOB map is the other shape it may
 * arrive as. */
bool q_duckdb_codec_map_bits(ray_t* col, int depth, qd_colmap_t* cm) {
    bool bools = cm->depth == depth + 1 && cm->leaf->ray_type == RAY_BOOL;
    bool empty = cm->depth == 0 && cm->leaf->dk_type == QDUCK_TYPE_BLOB && ray_len(col) == 0;
    if (cm->rec || !(bools || empty)) return false;
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (QD_TYPES[i].dk_type == QDUCK_TYPE_BIT) { cm->leaf = &QD_TYPES[i]; cm->depth = depth; return true; }
    return false;
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
 * (the int minimum, NaN, the all-zero UUID) through <c>_q_notnull.  A carrier whose RANGE the DuckDB type
 * exceeds carries the value itself in a long instead (ADR 11): a temporal outside q's window leaves 0N in the
 * slot and its raw count in <c>_q_raw; a 128-bit word keeps its low half in the slot and, once any row needs
 * it, every row's high half in <c>_q_hi.  Those two families fold their null-patterned value into the same
 * column, so every type has exactly ONE companion kind, and a column grows it only when a row needs it. */
static const char* const QD_CO_NAMES[] = { "none", "isnull", "notnull", "raw", "hi", "tzoff" };

const char* q_duckdb_codec_co_name(int kind) { return QD_CO_NAMES[kind]; }

/* the companion's column name, <parent>_q_<kind> — THE spelling the reader emits, the strip recognises, the stage
 * declares and the cast leg consumes */
void q_duckdb_codec_companion_col(qd_buf* b, const char* parent, size_t n, int kind) {
    q_duckdb_putn(b, parent, n);
    q_duckdb_puts(b, "_q_");
    q_duckdb_puts(b, QD_CO_NAMES[kind]);
}

/* the kind a column name's suffix spells (QD_CO_NONE = not a companion); *sl the suffix length */
static int qd_companion_kind(const char* s, size_t n, size_t* sl) {
    for (int k = QD_CO_ISNULL; k <= QD_CO_TZOFF; k++) {
        qd_buf sfx = {0};
        q_duckdb_codec_companion_col(&sfx, "", 0, k);
        bool hit = !sfx.oom && n >= sfx.len && memcmp(s + n - sfx.len, sfx.p, sfx.len) == 0;
        *sl = sfx.len;
        q_duckdb_buf_free(&sfx);
        if (hit) return k;
    }
    return QD_CO_NONE;
}

bool q_duckdb_codec_companion_name(const char* s, size_t n) {
    size_t sl;
    return qd_companion_kind(s, n, &sl) != QD_CO_NONE;
}

/* The bridge's reserved namespace is the _q_ infix: a BARE table carries none, because a companion travels only
 * inside the pair (data;schema) and a q-born column of that spelling is a user's, colliding (ADR 11). */
static bool qd_reserved_col(const char* s, size_t n) {
    for (size_t i = 0; i + 3 <= n; i++) if (memcmp(s + i, "_q_", 3) == 0) return true;
    return false;
}

static bool qd_nullless(const qd_tmap_t* tm) {
    return tm->ray_type == RAY_BOOL || tm->ray_type == RAY_BYTE_ONLY || tm->ray_type == RAY_SYM ||
           tm->ray_type == RAY_STR  || tm->ray_type == RAY_LIST      || tm->ray_type == RAY_CHARV ||
           tm->ray_type == RAY_ENUM;
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

/* ---- The SHAPE MIRROR (ADR 16), a law stated per COMPANION.  EVERY companion MIRRORS THE SHAPE of the value it
 * answers for: an atom takes one leaf, a list of n takes n leaves, a dict takes a like-keyed dict, and a nest
 * recurses.  An ATOM therefore means "this whole thing is NULL" and any container means "this exists, look
 * inside" — the one discrimination that gives a flag with no row of its own (an element inside a LIST, a field
 * inside a record) a slot at every depth.  The KIND decides only what a leaf is: a boolean for isnull and
 * notnull, a long for hi and a counting raw, DuckDB's own text for a text raw, an int for tzoff.  A nested
 * column's containers are null-less and its elements are not, so the two companions ride as two columns.
 * UNDER A RECORD each FIELD grows its own, and the record ships one column per KIND any field grew — the write
 * back reaches the ones the APPENDER consumes, hi/raw/tzoff awaiting a cast leg that descends. ---- */

/* the accumulator a companion of `kind` takes at this level: a list above the leaf, the kind's own vector at it */
static ray_t* qd_co_new(const qd_colmap_t* cm, int kind, int64_t cap) {
    if (cap < 1) cap = 1;
    if (cm->rec && !cm->depth) return qd_rec_shell(cm, kind);
    if (cm->depth) return ray_list_new(cap);
    if (kind == QD_CO_TZOFF) return ray_vec_new(RAY_I32, cap);
    if (kind == QD_CO_HI)    return ray_vec_new(RAY_I64, cap);
    if (kind == QD_CO_RAW)
        return q_duckdb_codec_raw_is_text(cm->leaf->ray_type) ? ray_list_new(cap) : ray_vec_new(RAY_I64, cap);
    return ray_vec_new(RAY_BOOL, cap);
}

/* the ATOM that answers for a whole NULL container: the isnull mirror flags it, every other companion has
 * nothing inside to say anything about and takes its own absent marker */
static ray_t* qd_co_absent(int kind, const qd_tmap_t* tm) {
    if (kind == QD_CO_ISNULL) return ray_bool(true);
    if (kind == QD_CO_TZOFF)  return ray_i32(NULL_I32);
    if (kind == QD_CO_HI)     return ray_i64(NULL_I64);
    if (kind == QD_CO_RAW)    return q_duckdb_codec_raw_is_text(tm->ray_type) ? ray_charv("", 0) : ray_i64(NULL_I64);
    return ray_bool(false);
}

/* The EMPTY cell of a record column under a LIST, in kind `kind` (QD_CO_NONE the value itself): the fields' own
 * empty columns, as the TABLE a run of like dicts is.  A cell with no row still carries the record's SHAPE, which
 * is what leaves an empty cell and a NULL one telling apart by the mirror alone.  A MAP's keys and a UNION's live
 * member are data that varies per row, so neither spells a shell: the empty list is all they are. */
static ray_t* qd_rec_shell(const qd_colmap_t* cm, int kind) {
    const qd_rec_t* r = cm->rec;
    if (!r || cm->leaf->dk_type != QDUCK_TYPE_STRUCT) return ray_list_new(1);
    ray_t* t = ray_table_new(r->n);
    for (int i = 0; i < r->n && t && !RAY_IS_ERR(t); i++) {
        const qd_colmap_t* fm = &r->f[i].map;
        ray_t* c = kind == QD_CO_NONE ? qd_new_col(fm, 0) : qd_co_new(fm, kind, 0);
        if (!c || RAY_IS_ERR(c)) { ray_release(t); return c ? c : q_err(QE_WSFULL); }
        t = ray_table_add_col(t, ray_sym_intern_runtime(r->f[i].name, (int64_t)strlen(r->f[i].name)), c);
        ray_release(c);
    }
    return t ? t : q_err(QE_WSFULL);
}

/* one leaf flag onto the accumulator — a boolean vector at a leaf level, a list of atoms above it */
static ray_t* qd_mirror_bit_append(ray_t* m, bool flag) {
    if (!m || RAY_IS_ERR(m)) return m;
    if (m->type == RAY_BOOL) { uint8_t f = flag; return ray_vec_append(m, &f); }
    ray_t* a = ray_bool(flag);
    if (!a || RAY_IS_ERR(a)) { ray_release(m); return a ? a : q_err(QE_WSFULL); }
    ray_t* out = ray_list_append(m, a);   /* retains */
    ray_release(a);
    if (out && RAY_IS_ERR(out)) ray_release(m);
    return out;
}

/* one whole row-mirror onto the accumulator; the caller's ref is consumed either way */
static ray_t* qd_mirror_append(ray_t* m, ray_t* cell) {
    if (!cell || RAY_IS_ERR(cell)) { if (m && !RAY_IS_ERR(m)) ray_release(m); return cell ? cell : q_err(QE_WSFULL); }
    if (!m || RAY_IS_ERR(m)) { ray_release(cell); return m; }
    ray_t* out = ray_list_append(m, cell);   /* retains */
    ray_release(cell);
    if (out && RAY_IS_ERR(out)) ray_release(m);
    return out;
}

/* the i-th sub-mirror, borrowed; a boolean vector holds leaves, not sub-mirrors */
static ray_t* qd_mirror_item(ray_t* m, int64_t i) {
    return m && m->type == RAY_LIST && i < m->len ? ray_list_get(m, i) : NULL;
}

/* The BOOLEAN at i, whatever shape carries it — at a leaf level "this element is NULL", one level up "this whole
 * cell is NULL".  A run of atoms collapses to a boolean vector, which is exactly the flat companion a flat column
 * always had, so both spellings read the same here. */
static bool qd_mirror_bit(ray_t* m, int64_t i) {
    if (!m) return false;
    if (m->type == -RAY_BOOL) return m->b8 != 0;
    if (m->type == RAY_BOOL)  return i < m->len && *(uint8_t*)ray_vec_get(m, i) != 0;
    ray_t* it = qd_mirror_item(m, i);
    return it && it->type == -RAY_BOOL && it->b8;
}

/* A mirror is an ordinary q value, so it settles into the spelling q construction gives it: a run of atoms is a
 * boolean vector (the flat companion, unchanged), a run of like-keyed dicts the table they are.  Consumes m. */
static ray_t* qd_mirror_pack(ray_t* m) {
    if (!m || RAY_IS_ERR(m) || m->type != RAY_LIST) return m;
    ray_t* c = q_list_collapse(m);   /* owned: retains-or-builds, so the incoming ref is ours to drop either way */
    if (!c) return m;
    ray_release(m);
    return c;
}

/* ADR 17: a FULL raw's own cells no longer say whether it was REQUIRED, so the pair does.  A slot that is NULL
 * beside a raw that is not is exactly a value the carrier could not hold, and nothing else is: an in-range value
 * and an infinity both fill the slot, and a SQL NULL nulls both halves. */
static bool qd_raw_needed(ray_t* col, ray_t* raw) {
    if (!col || !raw || RAY_IS_ERR(col) || RAY_IS_ERR(raw)) return false;
    for (int64_t i = 0; i < col->len && i < ray_len(raw); i++) {
        if (!ray_vec_is_null(col, i)) continue;
        if (raw->type == RAY_LIST) { ray_t* c = ray_list_get(raw, i); if (c && ray_len(c)) return true; }
        else if (!ray_vec_is_null(raw, i)) return true;
    }
    return false;
}

/* emit-when-needed, decided per COLUMN over the whole mirror: no flag anywhere, no companion */
static bool qd_mirror_any(ray_t* m) {
    if (!m || RAY_IS_ERR(m)) return false;
    if (m->type == -RAY_BOOL) return m->b8 != 0;
    if (m->type == RAY_BOOL) {
        for (int64_t i = 0; i < m->len; i++) if (*(uint8_t*)ray_vec_get(m, i)) return true;
        return false;
    }
    if (m->type == RAY_DICT) return qd_mirror_any(ray_dict_vals(m));
    if (m->type == RAY_TABLE) {   /* like-keyed rows, already packed into the table they are */
        for (int64_t c = 0; c < ray_table_ncols(m); c++) if (qd_mirror_any(ray_table_get_col_idx(m, c))) return true;
        return false;
    }
    if (m->type == RAY_LIST) {
        for (int64_t i = 0; i < m->len; i++) if (qd_mirror_any(ray_list_get(m, i))) return true;
        return false;
    }
    return false;
}

/* ADR 21: a zoned temporal's offset rides c_q_tzoff, an INT companion emitted for EVERY zoned column — and, being
 * a companion, mirroring the shape above.  A NULL value's offset is 0Ni: no instant, so no zone. */

#define QD_TZ_MAX 57599   /* ±15:59:59, the offset range TIMETZ constructs, and the bias its storage subtracts from */

static ray_t* qd_tz_append(ray_t* m, int32_t off, bool isnull) {
    if (!m || RAY_IS_ERR(m)) return m;
    if (m->type == RAY_I32) return isnull ? qd_append_null(m) : ray_vec_append(m, &off);
    ray_t* a = ray_i32(isnull ? NULL_I32 : off);
    if (!a || RAY_IS_ERR(a)) { ray_release(m); return a ? a : q_err(QE_WSFULL); }
    ray_t* out = ray_list_append(m, a);   /* retains */
    ray_release(a);
    if (out && RAY_IS_ERR(out)) ray_release(m);
    return out;
}

/* The companions one read builds beside a value — the shape mirror, the leaf carrier's own, and a zoned leaf's
 * offsets.  ADR 16's law is per companion, so ONE recursion serves them all: a level allocates a child set, the
 * level below fills it, and the parent packs each and appends it.  The set is indexed BY KIND, which is also the
 * name each ships under, so a value whose parts need DIFFERENT kinds — a record, whose fields each grow their
 * own — has a slot for every one of them.  `want` is a field of its own because an accumulator is also NULL
 * before its first flagged row — the birth-on-demand a flat column's value companion still uses.  `need` latches
 * the row that asked for that kind, which is how emit-when-needed survives a nest, where every level is
 * allocated up front. */
#define QD_NCOS (QD_CO_TZOFF + 1)
typedef struct {
    ray_t* acc;
    bool   want;
    bool   need;
} qd_co_acc_t;
typedef struct {
    qd_co_acc_t co[QD_NCOS];   /* co[QD_CO_NONE] is never wanted: the slot a carrier with no companion asks for */
} qd_cos_t;

static void qd_cos_drop(qd_cos_t* c) {
    for (int i = 0; i < QD_NCOS; i++) { q_duckdb_drop(c->co[i].acc); c->co[i].acc = NULL; }
}

static ray_t* qd_store_companions(int slot, duck_result* res, int64_t ncols, const qd_colmap_t* cms, ray_t** cols,
                                  qd_cos_t* cos, bool* taken);

/* A long companion born on the first row that needs it, over a column already holding that row.  The rows before
 * it: a raw's absent marker is 0N; a hi's is the high word a fitting row has — its low word's sign extension, or 0N
 * where the row is NULL (a VALUE whose low word is the null pattern needs hi, so before the birth every such slot
 * is a NULL). */
static ray_t* qd_long_row(ray_t* mask, ray_t* col, int64_t v, bool hi) {
    int64_t row = col->len - 1;
    if (!mask) mask = ray_vec_new(RAY_I64, row + 1);
    while (mask && !RAY_IS_ERR(mask) && mask->len < row) {
        int64_t lo = hi ? ((int64_t*)ray_data(col))[mask->len] : NULL_I64;
        int64_t b  = lo == NULL_I64 ? NULL_I64 : lo < 0 ? -1 : 0;
        mask = b == NULL_I64 ? qd_append_null(mask) : ray_vec_append(mask, &b);
    }
    if (!mask || RAY_IS_ERR(mask)) return mask;
    return v == NULL_I64 ? qd_append_null(mask) : ray_vec_append(mask, &v);
}

/* The TEXT twin of qd_long_row (ADR 11/19): a raw companion of DuckDB's own spelling, "" the absent marker the
 * write-back reads as "the slot holds it".  Born on the first row that needs it, over a column already holding it. */
static ray_t* qd_text_cell(ray_t* mask, const char* s) {
    ray_t* cell = ray_charv(s, (int64_t)strlen(s));
    if (!cell || RAY_IS_ERR(cell)) { ray_release(mask); return cell ? cell : q_err(QE_WSFULL); }
    ray_t* out = ray_list_append(mask, cell);   /* retains; leaves the accumulator standing when it fails */
    ray_release(cell);
    if (RAY_IS_ERR(out)) ray_release(mask);
    return out;
}

static ray_t* qd_text_row(ray_t* mask, ray_t* col, const char* s) {
    int64_t row = col->len - 1;
    if (!mask) mask = ray_list_new(row + 1);
    while (mask && !RAY_IS_ERR(mask) && mask->len < row) mask = qd_text_cell(mask, "");
    return !mask || RAY_IS_ERR(mask) ? mask : qd_text_cell(mask, s);
}

/* A months-bearing INTERVAL as text DuckDB parses back to the same three fields; its own rendering would need its
 * printer reimplemented, and every field-exact spelling is one CAST away from the value. */
static void qd_interval_text(const void* data, duck_idx_t r, char* buf, size_t cap) {
    const duck_interval* iv = &((const duck_interval*)data)[r];
    snprintf(buf, cap, "%d months %d days %lld microseconds", iv->months, iv->days, (long long)iv->micros);
}

/* ADR 21: a BIGNUM's DIGITS, since ::BIGINT and ::HUGEINT both refuse it and ::VARCHAR is the only lossless
 * extraction.  Its storage is a 3-byte header — the sign in the top bit, then the magnitude's byte count — over
 * big-endian magnitude bytes, every byte one's-complemented when the value is negative.  The caller owns the
 * returned text; NULL = malformed, too wide, or out of memory.
 * The conversion is repeated division by ten, O(bytes^2), and the header's 23-bit length admits 8 MiB of magnitude —
 * twenty million digits, which would wedge the reader for hours.  So a magnitude past QD_VARINT_MAX is REFUSED,
 * loudly, at a width (9,800-odd digits) no arithmetic a store holds comes near.  §21's "unbounded" is the ruling
 * that no FIXED-WIDTH CARRIER suffices, not a promise to decode an 8 MiB integer inside a read. */
#define QD_VARINT_MAX 4096
static char* qd_varint_text(const char* p, int64_t len) {
    if (len < 3) return NULL;
    uint8_t inv = ((uint8_t)p[0] & 0x80) ? 0 : 0xFF;
    uint32_t n  = (uint32_t)((((uint8_t)p[0] ^ inv) & 0x7F) << 16) | (uint32_t)((uint8_t)p[1] ^ inv) << 8 |
                  (uint32_t)((uint8_t)p[2] ^ inv);
    if ((int64_t)n + 3 != len || n > QD_VARINT_MAX) return NULL;
    uint8_t* m   = malloc(n ? n : 1);
    char*    out = malloc((size_t)n * 3 + 4);   /* 256^n < 10^(2.41n), so 3 digits a byte is room to spare */
    if (!m || !out) { free(m); free(out); return NULL; }
    for (uint32_t i = 0; i < n; i++) m[i] = (uint8_t)p[3 + i] ^ inv;
    size_t   d  = 0;
    uint32_t lo = 0;
    while (lo < n) {
        uint32_t rem = 0;
        for (uint32_t i = lo; i < n; i++) {
            uint32_t cur = rem << 8 | m[i];
            m[i] = (uint8_t)(cur / 10);
            rem  = cur % 10;
        }
        out[d++] = (char)('0' + rem);
        while (lo < n && !m[lo]) lo++;
    }
    if (!d) out[d++] = '0';
    if (inv && !(d == 1 && out[0] == '0')) out[d++] = '-';
    for (size_t i = 0; i < d / 2; i++) { char t = out[i]; out[i] = out[d - 1 - i]; out[d - 1 - i] = t; }
    out[d] = '\0';
    free(m);
    return out;
}

static ray_t* qd_read_fail(int slot, ray_t* col, const qd_tmap_t* tm, const char* why) {
    ray_release(col);
    return q_duckdb_fail(slot, tm->logical, why);
}

/* A µs count as the micros-only INTERVAL it is: months and days stay 0, so nothing we write can leak into a unit
 * whose length is not fixed (`datepart('day', to_microseconds(86400000000))` is 0, measured on 1.5.5). */
static void qd_put_interval(void* data, duck_idx_t r, int64_t us) {
    ((duck_interval*)data)[r] = (duck_interval){ 0, 0, us };
}

/* An INTERVAL cell as one µs count: days fold at DuckDB's own 24h, months never (a month is no fixed duration).
 * A day count is int32, so the fold itself can exceed int64 — the bound is checked before the multiply. */
#define QD_DAY_US 86400000000LL
static bool qd_interval_us(const void* data, duck_idx_t r, int64_t* out) {
    const duck_interval* iv = &((const duck_interval*)data)[r];
    if (iv->months || iv->days > INT64_MAX / QD_DAY_US || iv->days < INT64_MIN / QD_DAY_US) return false;
    int64_t d = (int64_t)iv->days * QD_DAY_US;
    if ((d > 0 && iv->micros > INT64_MAX - d) || (d < 0 && iv->micros < INT64_MIN - d)) return false;
    *out = d + iv->micros;
    return true;
}

/* DuckDB counts its infinities from 1970 and q counts 0W/-0W from 2000, but both are the carrier's extremes,
 * so the SAME bits spell both and either direction steps around the epoch shift, which maps neither onto the
 * other (ADR 11 reverses 6's overflow error for exactly these values). */
static bool qd_date_inf(int32_t v) { return v == INT32_MAX || v == -INT32_MAX; }
static bool qd_ts_inf(int64_t v)   { return v == INT64_MAX || v == -INT64_MAX; }

/* ns per unit of a DuckDB timestamp carrier: the q timestamp is ns, so every coarser grain scales up exactly */
static int64_t qd_ts_scale(duck_type t) {
    return t == QDUCK_TYPE_TIMESTAMP_S ? 1000000000LL : t == QDUCK_TYPE_TIMESTAMP_MS ? 1000000LL
         : t == QDUCK_TYPE_TIMESTAMP || t == QDUCK_TYPE_TIMESTAMP_TZ ? 1000LL : 1LL;
}

/* the two-word family: whose value exceeds `j` (UBIGINT above 2^63, HUGEINT, UHUGEINT) and DECIMAL, whose
 * unscaled integer does above 18 digits */
static bool qd_wide_int(duck_type t) {
    return t == QDUCK_TYPE_UBIGINT || t == QDUCK_TYPE_HUGEINT || t == QDUCK_TYPE_UHUGEINT || t == QDUCK_TYPE_DECIMAL;
}

/* the two words of a two-word cell (UBIGINT's high word is 0), as the bits they are; `phys` is the storage the
 * value sits in — DECIMAL's is its width's, every other type's is its own */
static int64_t qd_wide_words(duck_type phys, const void* data, duck_idx_t r, int64_t* lo) {
    switch (phys) {
        case QDUCK_TYPE_SMALLINT: *lo = ((const int16_t*)data)[r]; break;
        case QDUCK_TYPE_INTEGER:  *lo = ((const int32_t*)data)[r]; break;
        case QDUCK_TYPE_BIGINT:   *lo = ((const int64_t*)data)[r]; break;
        case QDUCK_TYPE_UBIGINT:  *lo = (int64_t)((const uint64_t*)data)[r]; return 0;
        case QDUCK_TYPE_HUGEINT:  { const duck_hugeint* h = &((const duck_hugeint*)data)[r]; *lo = (int64_t)h->lower; return h->upper; }
        case QDUCK_TYPE_UHUGEINT: { const duck_uhugeint* h = &((const duck_uhugeint*)data)[r]; *lo = (int64_t)h->lower; return (int64_t)h->upper; }
        /* a storage this build does not know: the (0N;0N) pattern, which the caller refuses, never a mis-typed read */
        default: *lo = NULL_I64; return NULL_I64;
    }
    return *lo < 0 ? -1 : 0;
}

/* Which companion a value of this type can grow (ADR 2/4/11), THE decision every consumer reads — the contract
 * table, the reader's column name, the writer's admissibility.  A temporal carrier has a window DuckDB's range
 * exceeds (`raw`); the wide integer family exceeds `j` (`hi`); the rest ask the read cell codec's sentinel question —
 * can a legal value land on the q null pattern (a `hit`)?  Only where the carrier is read at full width: the
 * widening arms (TINYINT, USMALLINT, UINTEGER) stop short of it, and so does TIME_NS, whose day-long domain sits
 * six orders inside the timespan it lands in. */
static int qd_leaf_co_kind(const qd_tmap_t* tm) {
    if (qd_nullless(tm)) return QD_CO_ISNULL;
    if (qd_wide_int(tm->dk_type)) return QD_CO_HI;
    if (tm->dk_type == QDUCK_TYPE_INTERVAL && q_duckdb_codec_raw_is_text(tm->ray_type)) return QD_CO_RAW;
    if (tm->dk_type == QDUCK_TYPE_VARINT) return QD_CO_RAW;   /* ADR 21: every row, since the slot holds none of it */
    if (tm->ray_type == RAY_DATE || tm->ray_type == RAY_TIME || tm->ray_type == RAY_TIMESTAMP) return QD_CO_RAW;
    bool off_pattern = tm->dk_type == QDUCK_TYPE_TINYINT || tm->dk_type == QDUCK_TYPE_USMALLINT ||
                       tm->dk_type == QDUCK_TYPE_UINTEGER || tm->dk_type == QDUCK_TYPE_TIME_NS;
    return off_pattern ? QD_CO_NONE : QD_CO_NOTNULL;
}

/* the SHAPE companion a whole column grows: a nest, a record and a null-less carrier all flag through the mirror */
static int qd_co_kind(const qd_colmap_t* cm) {
    return qd_cm_nullless(cm) ? QD_CO_ISNULL : qd_leaf_co_kind(cm->leaf);
}

/* And the ELEMENT companion beside it (ADR 16 per companion): a nested column's leaves grow whatever their own
 * carrier needs, which the shape mirror cannot say — its leaves already mean "this null-less element is NULL". */
static int qd_val_co_kind(const qd_colmap_t* cm) {
    if (cm->rec) return QD_CO_NONE;
    int k = qd_leaf_co_kind(cm->leaf);
    return k == QD_CO_ISNULL ? QD_CO_NONE : k;
}

/* ADR 21: a zoned temporal is a PAIR — the slot holds one integer and c_q_tzoff the other, ALWAYS, because an
 * offset of zero is a fact about the value and not an absence. */
bool q_duckdb_codec_zoned(const qd_colmap_t* cm) {
    return !cm->rec && (cm->leaf->dk_type == QDUCK_TYPE_TIME_TZ || cm->leaf->dk_type == QDUCK_TYPE_TIMESTAMP_TZ);
}

/* THE complete set a value of this shape can grow, the shape companion first: a nested column's element companion
 * beside it (ADR 16), then a zoned type's offsets.  The count is what `.duckdb.types[]` reports, which asks about
 * flat manifest rows, where the first two are the same one answer. */
int q_duckdb_codec_companions_of(const qd_colmap_t* cm, const char* out[QD_CO_MAX]) {
    int n = 0;
    int k = qd_co_kind(cm), v = qd_val_co_kind(cm);
    if (k != QD_CO_NONE || !q_duckdb_codec_zoned(cm)) out[n++] = QD_CO_NAMES[k];
    if (v != QD_CO_NONE && v != k) out[n++] = QD_CO_NAMES[v];
    if (q_duckdb_codec_zoned(cm)) out[n++] = QD_CO_NAMES[QD_CO_TZOFF];
    return n;
}

/* the kind of a companion in hand: a null-less parent's is its shape mirror, a boolean is its group, a text column
 * is the raw spelling of what no count holds, and a long is what the parent's carrier lacks */
int q_duckdb_codec_mask_kind(const qd_colmap_t* cm, ray_t* mask) {
    if (!mask) return QD_CO_NONE;
    if (qd_cm_nullless(cm)) return QD_CO_ISNULL;
    if (mask->type == RAY_BOOL) return QD_CO_NOTNULL;
    if (mask->type == RAY_LIST) return QD_CO_RAW;
    return cm->leaf->ray_type == RAY_I64 ? QD_CO_HI : QD_CO_RAW;
}

/* an ENUM cell is an index into the type's dictionary, as wide as the dictionary needs */
static duck_idx_t qd_enum_index(const void* data, duck_type width, duck_idx_t r) {
    return width == QDUCK_TYPE_UTINYINT  ? ((const uint8_t*)data)[r]
         : width == QDUCK_TYPE_USMALLINT ? ((const uint16_t*)data)[r] : ((const uint32_t*)data)[r];
}

/* n elements of one DuckDB vector from row `from` -> the accumulating q column
 * (moved), or error.  THE read cell codec — every LIST child level lands here
 * too, so the epoch shifts and the null law have one home: a sentinel carrier
 * nulls in-band and a data value on the null pattern is a `hit`; a null-less one
 * (bool/byte/symbol/string/blob) takes the fill on NULL.  Either group's rare
 * state flags the row in cos->val; a raw or hi carrier lands its long there instead
 * (`cval`, the column forced by `need`) — no QD_W_VAL = no row to carry it, 'duckdb.  cos->mir takes the
 * isnull leaf at EVERY level (ADR 16), so a null-less element inside a LIST has its slot, and cos->tz the zoned
 * leaf's UTC offset at every level the same way (ADR 21).  `scratch` holds a raw text too long for the row
 * buffer — a BIGNUM's digits — and the wrapper below frees it however the walk ends. */
static ray_t* qd_read_cells_1(int slot, ray_t* col, const qd_tmap_t* tm, duck_vector dv, duck_idx_t from,
                              duck_idx_t n, qd_cos_t* cos, duck_logical_type enum_lt,
                              duck_type phys, char** scratch) {
    void*     data     = QAPI.vector_get_data(dv);
    uint64_t* validity = QAPI.vector_get_validity(dv);
    const bool nullless = qd_nullless(tm);
    const int  kind     = qd_co_kind(&(const qd_colmap_t){ tm, NULL, 0, false });
    const duck_type ewidth = enum_lt ? QAPI.enum_internal_type(enum_lt) : 0;
    for (duck_idx_t r = from; r < from + n; r++) {
        bool ok = q_duckdb_validity_ok(validity, r), hit = false, need = false;
        int64_t cval = NULL_I64;
        int32_t tzoff = 0;
        char    cotext[96] = "";
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
            case RAY_I32: {  /* USMALLINT widens losslessly */
                int32_t v = tm->dk_type == QDUCK_TYPE_USMALLINT ? (int32_t)((uint16_t*)data)[r] : ((int32_t*)data)[r];
                if (!ok || (hit = v == NULL_I32)) col = qd_append_null(col);
                else                              col = ray_vec_append(col, &v);
                break;
            }
            case RAY_MONTH: {    /* the DATE's month, the infinities passing through as the same bits */
                int32_t v = ((int32_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                int64_t mp = v;
                if (!qd_date_inf(v) && !q_calendar_month_from_days((int64_t)v - QD_EPOCH_DAYS, &mp))
                    return qd_read_fail(slot, col, tm, "a DATE past the first of its month is no whole month");
                int32_t m = (int32_t)mp;
                col = ray_vec_append(col, &m);
                break;
            }
            case RAY_MINUTE:
            case RAY_SECOND: {   /* the INTERVAL's µs at the grain, exact or nothing */
                int64_t unit = tm->ray_type == RAY_MINUTE ? 60000000LL : 1000000LL;
                int64_t v;
                if (!ok) { col = qd_append_null(col); break; }
                if (!qd_interval_us(data, r, &v))
                    return qd_read_fail(slot, col, tm, "an INTERVAL bearing months is no fixed duration");
                if (v % unit != 0)
                    return qd_read_fail(slot, col, tm, "an INTERVAL finer than the grain is no whole count");
                int64_t c = v / unit;
                if (c > INT32_MAX || c < INT32_MIN)
                    return qd_read_fail(slot, col, tm, "an INTERVAL outside the count's range");
                int32_t q = (int32_t)c;
                hit = q == NULL_I32;
                col = ray_vec_append(col, &q);
                break;
            }
            case RAY_I64:
            case RAY_TIMESPAN: { /* timespan: raw ns rides BIGINT; UINTEGER widens losslessly; the wide integer
                                  * family lands its LOW word here as the bits they are, `co` its high word (ADR 4) */
                int64_t v, hi = 0;
                /* ADR 19: INTERVAL's canonical read, µs -> ns; what no timespan holds takes 0Nn and rides raw */
                if (tm->dk_type == QDUCK_TYPE_INTERVAL) {
                    if (!ok) { col = qd_append_null(col); break; }
                    int64_t us;
                    qd_interval_text(data, r, cotext, sizeof cotext);   /* ADR 17 */
                    need = !qd_interval_us(data, r, &us) || us > INT64_MAX / 1000 || us < INT64_MIN / 1000;
                    if (need) { col = qd_append_null(col); break; }
                    int64_t ns = us * 1000;
                    col = ray_vec_append(col, &ns);
                    break;
                }
                /* ADR 21: a BIGNUM has no q carrier at any width, so the slot is 0N and the DIGITS ride raw */
                if (tm->dk_type == QDUCK_TYPE_VARINT) {
                    free(*scratch);           /* "" is raw's absent marker, so a NULL row must not inherit digits */
                    *scratch = NULL;
                    col = qd_append_null(col);
                    if (!ok) break;
                    const duck_string_t* sv = &((const duck_string_t*)data)[r];
                    *scratch = qd_varint_text(q_duckdb_string_data(sv), (int64_t)q_duckdb_string_len(sv));
                    if (!*scratch)
                        return qd_read_fail(slot, col, tm, "a VARINT cell no header describes, or wider than 4096 bytes");
                    need = true;
                    break;
                }
                if (qd_wide_int(tm->dk_type)) hi = qd_wide_words(phys, data, r, &v);
                else v = tm->dk_type == QDUCK_TYPE_UINTEGER ? (int64_t)((uint32_t*)data)[r] : ((int64_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                if (kind != QD_CO_HI) { col = (hit = v == NULL_I64) ? qd_append_null(col) : ray_vec_append(col, &v); break; }
                if (hi == NULL_I64 && v == NULL_I64)
                    return qd_read_fail(slot, col, tm, "the one 128-bit pattern that spells NULL");
                bool sgn = tm->dk_type == QDUCK_TYPE_HUGEINT || tm->dk_type == QDUCK_TYPE_DECIMAL;
                need = hi != (sgn && v < 0 ? -1 : 0) || (sgn ? v == NULL_I64 : v < 0);
                cval = hi;
                col  = ray_vec_append(col, &v);
                break;
            }
            case RAY_F32: {      /* live-infinity: ONLY NaN is null */
                float v = ((float*)data)[r];
                if (!ok || (hit = v != v)) col = qd_append_null(col);
                else                       col = ray_vec_append(col, &v);
                break;
            }
            case RAY_F64: {
                double v = ((double*)data)[r];
                if (!ok || (hit = v != v)) col = qd_append_null(col);
                else                       col = ray_vec_append(col, &v);
                break;
            }
            case RAY_DATETIME: { /* the TIMESTAMP's µs as float days at q's millisecond grain; ±infinity is ±0w */
                int64_t v = ((int64_t*)data)[r];
                if (!ok || v == NULL_I64) { col = qd_append_null(col); break; }
                double d;
                if (qd_ts_inf(v)) d = v > 0 ? (double)INFINITY : -(double)INFINITY;
                else {
                    if (v % 1000 != 0)
                        return qd_read_fail(slot, col, tm, "a TIMESTAMP finer than the millisecond is no datetime");
                    d = (double)(v / 1000 - QD_EPOCH_MS) / 86400000.0;
                }
                col = ray_vec_append(col, &d);
                break;
            }
            case RAY_SYM: {      /* descriptor-refined VARCHAR, or an ENUM's dictionary entry: intern; the fill is ` */
                int64_t id;
                if (!ok) id = ray_sym_intern_runtime("", 0);
                else if (enum_lt) {
                    char* v = QAPI.enum_dictionary_value(enum_lt, qd_enum_index(data, ewidth, r));
                    if (!v) return qd_read_fail(slot, col, tm, "index outside the enum dictionary");
                    id = ray_sym_intern_runtime(v, strlen(v));
                    QAPI.duck_free(v);
                } else {
                    const duck_string_t* s = &((const duck_string_t*)data)[r];
                    id = ray_sym_intern_runtime(q_duckdb_string_data(s), q_duckdb_string_len(s));
                }
                col = ray_vec_append(col, &id);
                break;
            }
            case RAY_CHARV: {    /* the `char` logical row's column: one VARCHAR byte a row, the fill " " */
                const duck_string_t* s = ok ? &((const duck_string_t*)data)[r] : NULL;
                if (s && q_duckdb_string_len(s) != 1)
                    return qd_read_fail(slot, col, tm, "not a single character");
                uint8_t v = s ? (uint8_t)*q_duckdb_string_data(s) : (uint8_t)' ';
                col = ray_vec_append(col, &v);
                break;
            }
            case RAY_STR:        /* VARCHAR -> charv cell, BLOB -> byte-vector cell, BIT -> boolean-vector cell;
                                  * the fill is the empty one, and BIT has no empty value of its own (ADR 20) */
            case RAY_LIST: {
                const duck_string_t* s = ok ? &((const duck_string_t*)data)[r] : NULL;
                const char* p   = s ? q_duckdb_string_data(s) : "";
                int64_t     len = s ? (int64_t)q_duckdb_string_len(s) : 0;
                bool bits = tm->dk_type == QDUCK_TYPE_BIT;
                if (bits && ok && !qd_bits_wellformed(p, len))
                    return qd_read_fail(slot, col, tm, "a BIT cell whose leading byte is no padding count");
                ray_t* cell = bits ? qd_bits_read(p, len)
                            : tm->ray_type == RAY_STR ? ray_charv(p, len)
                            : len ? ray_vec_from_raw(RAY_BYTE_ONLY, p, len) : ray_vec_new(RAY_BYTE_ONLY, 1);
                if (!cell || RAY_IS_ERR(cell)) { ray_release(col);
                    return cell ? cell : q_err(QE_WSFULL); }
                col = ray_list_append(col, cell);   /* retains */
                ray_release(cell);
                break;
            }
            case RAY_DATE: {     /* q's finite window is (-0W, 0W): a day below it rides raw (ADR 11 reverses 6) */
                int32_t v = ((int32_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                cval = v;        /* ADR 17: the count of EVERY row, the infinities' extremes included */
                if (qd_date_inf(v)) { col = ray_vec_append(col, &v); break; }
                int64_t q = (int64_t)v - QD_EPOCH_DAYS;
                if ((need = q < INT32_MIN + 2)) { col = qd_append_null(col); break; }
                int32_t d = (int32_t)q;
                col = ray_vec_append(col, &d);
                break;
            }
            case RAY_TIME: {     /* µs -> ms; a count no q time holds is never truncated — it rides raw */
                int64_t v;
                if (!ok) { col = qd_append_null(col); break; }
                /* ADR 21: a TIMETZ is the WALL time and its offset, packed into one word — µs above the low 24
                 * bits, and below them QD_TZ_MAX less the offset's seconds, so the encoding sorts by instant */
                if (tm->dk_type == QDUCK_TYPE_TIME_TZ) {
                    uint64_t bits = ((const uint64_t*)data)[r];
                    v     = (int64_t)(bits >> 24);
                    tzoff = QD_TZ_MAX - (int32_t)(bits & 0xFFFFFF);
                }
                else if (tm->dk_type == QDUCK_TYPE_TIME) v = ((int64_t*)data)[r];
                else if (!qd_interval_us(data, r, &v))
                    return qd_read_fail(slot, col, tm, "an INTERVAL bearing months is no time");
                int64_t ms = v / 1000;
                cval = v;        /* ADR 17 */
                if ((need = v % 1000 != 0 || ms <= INT32_MIN || ms > INT32_MAX)) {
                    col = qd_append_null(col); break;
                }
                int32_t q = (int32_t)ms;
                col = ray_vec_append(col, &q);
                break;
            }
            case RAY_TIMESTAMP: {  /* int64 at the carrier's grain, epoch 1970 -> ns, epoch 2000; an instant the
                                    * window (-0W, 0W) cannot hold rides raw at the carrier's own grain */
                int64_t v = ((int64_t*)data)[r], scale = qd_ts_scale(tm->dk_type);
                if (!ok || v == NULL_I64) { col = qd_append_null(col); break; }
                cval = v;        /* ADR 17: the count at the carrier's own grain, for every row */
                if (qd_ts_inf(v)) { col = ray_vec_append(col, &v); break; }
                need = v > INT64_MAX / scale || v < INT64_MIN / scale || v * scale < INT64_MIN + 2 + QD_EPOCH_NS;
                if (need) { col = qd_append_null(col); break; }
                int64_t q = v * scale - QD_EPOCH_NS;
                col = ray_vec_append(col, &q);
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
        bool flag = kind >= QD_CO_RAW ? need : nullless ? !ok : hit;
        qd_co_acc_t* mir = &cos->co[QD_CO_ISNULL];
        qd_co_acc_t* val = &cos->co[kind];   /* the leaf's OWN kind names its slot; QD_CO_NONE's is never wanted */
        qd_co_acc_t* tz  = &cos->co[QD_CO_TZOFF];
        if (tz->want) {
            tz->acc = qd_tz_append(tz->acc, tzoff, !ok);
            if (!tz->acc || RAY_IS_ERR(tz->acc)) {
                ray_release(col); ray_t* e = tz->acc; tz->acc = NULL; return e ? e : q_err(QE_WSFULL); }
        }
        if (mir->want) {
            mir->acc = qd_mirror_bit_append(mir->acc, nullless && !ok);
            if (!mir->acc || RAY_IS_ERR(mir->acc)) {
                ray_release(col); ray_t* e = mir->acc; mir->acc = NULL; return e ? e : q_err(QE_WSFULL); }
        }
        if (kind == QD_CO_ISNULL) {
            if (!mir->want && flag) return qd_read_fail(slot, col, tm, "NULL element has no row to flag");
            continue;
        }
        if (!val->want) {
            if (!flag) continue;
            return qd_read_fail(slot, col, tm, kind >= QD_CO_RAW ? "element outside the carrier has no row to carry it"
                                                                 : "element on the null pattern has no row to flag");
        }
        val->need = val->need || flag;
        /* ADR 17: a raw is a FULL column whenever it is present, so it takes EVERY row and the caller drops it
         * where no row ever needed it — the birth-on-first-flag the other companions use would back-fill 0N over
         * rows whose count is real.  Inside a nest every level is allocated up front, so `need` is what the drop
         * reads instead. */
        if (val->acc || flag || kind == QD_CO_RAW)
            val->acc = kind == QD_CO_RAW && q_duckdb_codec_raw_is_text(tm->ray_type)
                           ? qd_text_row(val->acc, col, *scratch ? *scratch : cotext)
                           : kind >= QD_CO_RAW ? qd_long_row(val->acc, col, cval, kind == QD_CO_HI)
                                               : qd_mask_row(val->acc, col->len - 1, flag);
        if (val->acc && RAY_IS_ERR(val->acc)) { ray_release(col); ray_t* e = val->acc; val->acc = NULL; return e; }
    }
    return col;
}

/* the scratch's one owner: however the walk above ends, the text it borrowed is freed here */
static ray_t* qd_read_cells(int slot, ray_t* col, const qd_tmap_t* tm, duck_vector dv, duck_idx_t from,
                            duck_idx_t n, qd_cos_t* cos, duck_logical_type enum_lt, duck_type phys) {
    char*  scratch = NULL;
    ray_t* r = qd_read_cells_1(slot, col, tm, dv, from, n, cos, enum_lt, phys, &scratch);
    free(scratch);
    return r;
}

/* ENUM and DECIMAL are the two rows whose cells cannot be read off the manifest alone: the dictionary and the
 * unscaled value's storage width both live on the vector's own logical type. */
static ray_t* qd_read_leaf(int slot, ray_t* col, const qd_tmap_t* tm,
                           duck_vector dv, duck_idx_t from, duck_idx_t n, qd_cos_t* cos) {
    bool enumd = tm->dk_type == QDUCK_TYPE_ENUM, dec = tm->dk_type == QDUCK_TYPE_DECIMAL;
    duck_logical_type lt = enumd || dec ? QAPI.vector_get_column_type(dv) : NULL;
    duck_type phys = dec && lt ? QAPI.decimal_internal_type(lt) : tm->dk_type;
    ray_t* r = qd_read_cells(slot, col, tm, dv, from, n, cos, enumd ? lt : NULL, phys);
    if (lt) QAPI.destroy_logical_type(&lt);
    return r;
}

/* A NULL LIST cell takes the empty fill and mirrors as the ATOM every companion spells for "nothing inside", so it
 * and an EMPTY list — which mirrors as an empty leaf vector — survive the crossing as the distinct values they are.
 * A fixed ARRAY level is the same walk over a child holding every row's elements back to back, array_size each.
 * The allocate-recurse-pack-append below is ADR 16's recursion, run once per companion the caller asked for. */
static ray_t* qd_read_rec_cell(int slot, ray_t* col, const qd_colmap_t* cm, duck_vector dv,
                         duck_idx_t from, duck_idx_t n, qd_cos_t* cos);

static ray_t* qd_read_col(int slot, ray_t* col, const qd_colmap_t* cm,
                          duck_vector dv, duck_idx_t from, duck_idx_t n, qd_cos_t* cos) {
    if (cm->depth == 0)
        return cm->rec ? qd_read_rec_cell(slot, col, cm, dv, from, n, cos)
                       : qd_read_leaf(slot, col, cm->leaf, dv, from, n, cos);
    const qd_colmap_t child = { cm->leaf, cm->rec, cm->depth - 1, false };
    duck_logical_type lt = QAPI.vector_get_column_type(dv);
    duck_idx_t asz = lt && QAPI.get_type_id(lt) == QDUCK_TYPE_ARRAY ? QAPI.array_type_array_size(lt) : 0;
    if (lt) QAPI.destroy_logical_type(&lt);
    const duck_list_entry* ent = asz ? NULL : (const duck_list_entry*)QAPI.vector_get_data(dv);
    uint64_t*   validity = QAPI.vector_get_validity(dv);
    duck_vector cv       = asz ? QAPI.array_vector_get_child(dv) : QAPI.list_vector_get_child(dv);
    if ((!asz && !ent) || !cv) { ray_release(col); return q_err(QE_DUCKDB); }
    for (duck_idx_t r = from; r < from + n; r++) {
        bool       ok  = q_duckdb_validity_ok(validity, r);
        if (!ok && !cos->co[QD_CO_ISNULL].want)
            return qd_read_fail(slot, col, cm->leaf, "NULL list element has no row to flag");
        duck_idx_t off  = asz ? r * asz : (duck_idx_t)ent[r].offset;
        duck_idx_t len  = !ok ? 0 : asz ? asz : (duck_idx_t)ent[r].length;
        ray_t*     cell = qd_new_col(&child, (int64_t)len);
        qd_cos_t   cc   = *cos;   /* the same wants, its own empty accumulators */
        ray_t* e = NULL;
        if (!cell) e = q_err(QE_WSFULL);
        else if (RAY_IS_ERR(cell)) { e = cell; cell = NULL; }
        for (int i = 0; i < QD_NCOS; i++) {
            cc.co[i].acc  = NULL;
            cc.co[i].need = false;
            cc.co[i].want = ok && cc.co[i].want;   /* a NULL cell has nothing inside for any of them */
            if (e || !cc.co[i].want) continue;
            ray_t* a = qd_co_new(&child, i, (int64_t)len);
            if (!a || RAY_IS_ERR(a)) e = a ? a : q_err(QE_WSFULL);
            else cc.co[i].acc = a;
        }
        if (!e && ok) cell = qd_read_col(slot, cell, &child, cv, off, len, &cc);
        if (!e && (!cell || RAY_IS_ERR(cell))) { e = cell ? cell : q_err(QE_WSFULL); cell = NULL; }
        for (int i = 0; i < QD_NCOS; i++) cos->co[i].need = cos->co[i].need || cc.co[i].need;
        if (!e) {
            col = ray_list_append(col, cell);   /* retains */
            ray_release(cell);
            cell = NULL;
            if (!col || RAY_IS_ERR(col)) { qd_cos_drop(&cc); return col ? col : q_err(QE_WSFULL); }
        }
        for (int i = 0; !e && i < QD_NCOS; i++) {
            if (!cos->co[i].want) continue;
            ray_t** p = &cos->co[i].acc;
            ray_t*  v = ok ? qd_mirror_pack(cc.co[i].acc) : qd_co_absent(i, cm->leaf);
            cc.co[i].acc = NULL;
            *p = qd_mirror_append(*p, v);       /* consumes v either way */
            if (!*p || RAY_IS_ERR(*p)) { e = *p ? *p : q_err(QE_WSFULL); *p = NULL; }
        }
        if (e) { q_duckdb_drop(cell); qd_cos_drop(&cc); ray_release(col); return e; }
    }
    return col;
}
/* ---- the record READ (ADR 12).  A record node accumulates its fields across EVERY chunk and materialises
 * once, so a field's companion is decided over the whole column and never per chunk.  A NULL cell reads no
 * fields at all — its children never vote — and materialises as the empty dict, which carries no type the
 * way () carries none for a list. ---- */

typedef struct qd_racc_s {
    ray_t* sel;                /* per row: the field read (a UNION's live tag), 0 elsewhere, -1 = a NULL cell */
    ray_t* ent;                /* MAP only: how many entries the row holds */
    int    n;
    struct qd_racc_s** sub;
    qd_cos_t* fcos;            /* n companion sets, one a field: ADR 12's own companion, at ADR 16's shape */
    ray_t* col[];              /* n field columns */
} qd_racc_t;

static void qd_racc_free(qd_racc_t* a) {
    if (!a) return;
    for (int i = 0; i < a->n; i++) if (a->col[i]) ray_release(a->col[i]);
    for (int i = 0; a->fcos && i < a->n; i++) qd_cos_drop(&a->fcos[i]);
    free(a->fcos);
    for (int i = 0; a->sub && i < a->n; i++) qd_racc_free(a->sub[i]);
    free(a->sub);
    if (a->sel) ray_release(a->sel);
    if (a->ent) ray_release(a->ent);
    free(a);
}

static ray_t* qd_born(ray_t** slot, ray_t* v) {
    if (v && !RAY_IS_ERR(v)) { *slot = v; return NULL; }
    if (v) ray_release(v);
    return q_err(QE_WSFULL);
}

/* Which companions a FIELD grows, and which of them are allocated up front.  A field's shape mirror is
 * unconditional (ADR 16: the mirror is uniform), its carrier's own companion and a zoned leaf's offsets follow
 * the same rules a top-level column's do — so a NaN double, a 128-bit integer and a TIMETZ all keep, inside a
 * record, the companion they keep outside one. */
static void qd_field_cos(const qd_colmap_t* fm, qd_cos_t* cos) {
    memset(cos, 0, sizeof *cos);
    cos->co[QD_CO_ISNULL].want = true;
    /* a RECORD field grows what its own fields grow — never a MAP's keys, which are the mirror's own keys and can
     * carry nothing beside them.  Read off the MAP alone, because a record under a list settles its companions per
     * CELL and every cell of the column has to agree on which columns stand beside it. */
    if (fm->rec) {
        for (int i = fm->leaf->dk_type == QDUCK_TYPE_MAP ? 1 : 0; i < fm->rec->n; i++) {
            qd_cos_t f;
            qd_field_cos(&fm->rec->f[i].map, &f);
            for (int k = 0; k < QD_NCOS; k++) cos->co[k].want = cos->co[k].want || f.co[k].want;
        }
        return;
    }
    cos->co[QD_CO_TZOFF].want = q_duckdb_codec_zoned(fm);
    int v = qd_val_co_kind(fm);
    if (v != QD_CO_NONE) cos->co[v].want = true;
}

static ray_t* qd_cos_alloc(const qd_colmap_t* cm, qd_cos_t* cos) {
    for (int i = 0; i < QD_NCOS; i++) {
        /* a NESTED level is allocated up front, because a cell's companion has to be as long as the cell; a FLAT
         * value companion still waits for its first flagged row, which is what its own drop rule reads */
        if (!cos->co[i].want || (i != QD_CO_ISNULL && i != QD_CO_TZOFF && !cm->depth)) continue;
        ray_t* a = qd_co_new(cm, i, 8);
        if (!a || RAY_IS_ERR(a)) return a ? a : q_err(QE_WSFULL);
        cos->co[i].acc = a;
    }
    return NULL;
}

static qd_racc_t* qd_racc_new(const qd_colmap_t* cm) {
    const qd_rec_t* r = cm->rec;
    qd_racc_t* a = calloc(1, sizeof *a + (size_t)r->n * sizeof(ray_t*));
    if (!a) return NULL;
    a->n    = r->n;
    a->sub  = calloc((size_t)r->n, sizeof *a->sub);
    a->fcos = calloc((size_t)r->n, sizeof *a->fcos);
    bool ok = a->sub && a->fcos && !qd_born(&a->sel, ray_vec_new(RAY_I16, 8)) &&
              (cm->leaf->dk_type != QDUCK_TYPE_MAP || !qd_born(&a->ent, ray_vec_new(RAY_I64, 8)));
    for (int i = 0; ok && i < r->n; i++) {
        if (r->f[i].map.rec && !r->f[i].map.depth) { ok = (a->sub[i] = qd_racc_new(&r->f[i].map)) != NULL; continue; }
        qd_field_cos(&r->f[i].map, &a->fcos[i]);
        ray_t* e = qd_cos_alloc(&r->f[i].map, &a->fcos[i]);
        if (e) { q_duckdb_drop(e); ok = false; break; }
        ok = !qd_born(&a->col[i], qd_new_col(&r->f[i].map, 8));
    }
    if (ok) return a;
    qd_racc_free(a);
    return NULL;
}

/* n rows of one record vector into its accumulator: a UNION's tag is physical child 0 and its members follow;
 * a MAP is a LIST whose child is STRUCT(key, value), so its two halves are entry-aligned, not row-aligned. */
static ray_t* qd_read_rec(int slot, qd_racc_t* acc, const qd_colmap_t* cm, duck_vector dv,
                          duck_idx_t from, duck_idx_t n) {
    const qd_rec_t* r = cm->rec;
    bool uni = cm->leaf->dk_type == QDUCK_TYPE_UNION, map = cm->leaf->dk_type == QDUCK_TYPE_MAP;
    uint64_t* validity = QAPI.vector_get_validity(dv);
    const duck_list_entry* ent = NULL;
    duck_vector* child = q_duckdb_cols(r->n, sizeof *child);
    duck_vector  tagv  = NULL;
    if (!child) return q_err(QE_WSFULL);
    if (map) {
        duck_vector kv = QAPI.list_vector_get_child(dv);
        ent = (const duck_list_entry*)QAPI.vector_get_data(dv);
        child[0] = kv ? QAPI.struct_vector_get_child(kv, 0) : NULL;
        child[1] = kv ? QAPI.struct_vector_get_child(kv, 1) : NULL;
    } else {
        tagv = uni ? QAPI.struct_vector_get_child(dv, 0) : NULL;
        for (int i = 0; i < r->n; i++) child[i] = QAPI.struct_vector_get_child(dv, (duck_idx_t)(uni + i));
    }
    ray_t* e = NULL;
    for (int i = 0; !e && i < r->n; i++) if (!child[i]) e = q_err(QE_DUCKDB);
    if (!e && ((map && !ent) || (uni && !tagv))) e = q_err(QE_DUCKDB);
    const uint8_t* tag = uni && tagv ? (const uint8_t*)QAPI.vector_get_data(tagv) : NULL;
    for (duck_idx_t rw = from; !e && rw < from + n; rw++) {
        bool ok = q_duckdb_validity_ok(validity, rw);
        int16_t sel = -1;
        if (ok) {
            sel = uni ? (int16_t)tag[rw] : 0;
            if (sel >= r->n) { e = q_duckdb_fail(slot, cm->leaf->logical, "tag outside the union's members"); break; }
            for (int i = uni ? sel : 0, hi = uni ? sel + 1 : r->n; !e && i < hi; i++) {
                duck_idx_t off = map ? (duck_idx_t)ent[rw].offset : rw;
                duck_idx_t len = map ? (duck_idx_t)ent[rw].length : 1;
                /* a sub-record records its own NULL rows in its `sel`, so its companions are born at the build */
                if (r->f[i].map.rec && !r->f[i].map.depth)
                    { e = qd_read_rec(slot, acc->sub[i], &r->f[i].map, child[i], off, len); continue; }
                /* ADR 12/16: a field grows its OWN companions, at the shape of the field, and `fcos` is where they
                 * accumulate across every chunk — so the emit decision is the column's, never one chunk's */
                acc->col[i] = qd_read_col(slot, acc->col[i], &r->f[i].map, child[i], off, len, &acc->fcos[i]);
                if (!acc->col[i] || RAY_IS_ERR(acc->col[i])) {
                    e = acc->col[i] ? acc->col[i] : q_err(QE_WSFULL);
                    acc->col[i] = NULL;
                }
            }
            if (e) break;
        }
        if (map) {
            int64_t l = ok ? (int64_t)ent[rw].length : 0;
            acc->ent = ray_vec_append(acc->ent, &l);
            if (!acc->ent || RAY_IS_ERR(acc->ent)) { e = acc->ent ? acc->ent : q_err(QE_WSFULL); acc->ent = NULL; break; }
        }
        acc->sel = ray_vec_append(acc->sel, &sel);
        if (!acc->sel || RAY_IS_ERR(acc->sel)) { e = acc->sel ? acc->sel : q_err(QE_WSFULL); acc->sel = NULL; break; }
    }
    free(child);
    return e;
}

/* one column value, boxed: a list hands its cell back, every typed vector its atom */
static ray_t* qd_cell_at(ray_t* col, int64_t i) {
    if (col->type == RAY_LIST) { ray_t* c = ray_list_get(col, i); ray_retain(c); return c; }
    return q_join_item(col, i);
}

/* The dict of fields [from, from+cnt) at row i, in kind `co` — QD_CO_NONE the data itself, any other kind that
 * companion of each field.  A field with nothing to say in that kind takes its ABSENT MARKER, because the mirror
 * is keyed exactly like the value (ADR 16) and a key the value carries may not go missing from beside it.
 * A run of like dicts is a dict of dicts, never a table, so only atoms collapse. */
static ray_t* qd_rec_cell(qd_racc_t* acc, const qd_rec_t* r, int from, int cnt, int64_t i, int co) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, cnt);
    ray_t* vals = ray_list_new(cnt);
    bool   box  = false;
    for (int k = from; k < from + cnt; k++) {
        ray_t*  src = co == QD_CO_NONE ? acc->col[k] : acc->fcos[k].co[co].acc;
        int64_t id  = ray_sym_intern_runtime(r->f[k].name, (int64_t)strlen(r->f[k].name));
        ray_t*  v   = id < 0                 ? NULL
                    : src                    ? qd_cell_at(src, i)
                    : co == QD_CO_NONE       ? NULL
                                             : qd_co_absent(co, r->f[k].map.leaf);
        if (!v || RAY_IS_ERR(v) || !keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
            if (v && !RAY_IS_ERR(v)) ray_release(v);
            if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
            if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
            return q_err(QE_WSFULL);
        }
        box  = box || v->type == RAY_DICT || v->type == RAY_TABLE;
        keys = ray_vec_append(keys, &id);
        vals = ray_list_append(vals, v);
        ray_release(v);
    }
    ray_t* cv = box ? vals : q_list_collapse(vals);
    if (cv != vals) ray_release(vals);
    if (!keys || RAY_IS_ERR(keys) || !cv || RAY_IS_ERR(cv)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (cv && !RAY_IS_ERR(cv)) ray_release(cv);
        return q_err(QE_WSFULL);
    }
    return ray_dict_new(keys, cv);   /* consumes both */
}

static ray_t* qd_empty_dict(void) {
    ray_t* k = ray_list_new(1);
    ray_t* v = ray_list_new(1);
    if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        return q_err(QE_WSFULL);
    }
    return ray_dict_new(k, v);
}

/* ONE pass over the accumulated rows, in kind `co` — the data column itself (QD_CO_NONE) or the companion of
 * that kind standing beside it.  The two walks are the same walk because they index the same rows the same way:
 * a STRUCT row is the fields' dict, a UNION row the ONE field its tag names, a MAP row the window its entry
 * count cuts out of the two flat halves, and a NULL cell read no fields at all — so the field columns count LIVE
 * rows, never the row index.  `tm` is the leaf a RAW companion's absent marker takes its carrier from. */
static ray_t* qd_rec_pass(qd_racc_t* acc, const qd_rec_t* r, bool uni, bool map, int co, const qd_tmap_t* tm) {
    int64_t  nrows = acc->sel->len, off = 0;
    int64_t* live  = q_duckdb_cols(r->n, sizeof *live);
    ray_t*   out   = live ? ray_list_new(nrows ? nrows : 1) : NULL;
    if (!out || RAY_IS_ERR(out)) { free(live); return out ? out : q_err(QE_WSFULL); }
    for (int64_t rw = 0; rw < nrows; rw++) {
        int16_t sel = *(int16_t*)ray_vec_get(acc->sel, rw);
        int64_t len = map && sel >= 0 ? *(int64_t*)ray_vec_get(acc->ent, rw) : 0;
        int64_t at  = sel < 0 ? 0 : live[uni ? sel : 0];
        ray_t*  cell;
        if (sel < 0) cell = co == QD_CO_NONE ? qd_empty_dict() : qd_co_absent(co, tm);
        else if (map) {   /* a MAP's companion is keyed by its own DATA keys — only the values can be NULL */
            ray_t* k = ray_vec_slice(acc->col[0], off, len);
            ray_t* v = ray_vec_slice(co == QD_CO_NONE ? acc->col[1] : acc->fcos[1].co[co].acc, off, len);
            if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v))
                { q_duckdb_drop(k); q_duckdb_drop(v); cell = q_err(QE_WSFULL); }
            else cell = ray_dict_new(k, v);   /* consumes both */
        }
        else cell = qd_rec_cell(acc, r, uni ? sel : 0, uni ? 1 : r->n, at, co);
        if (sel >= 0) live[uni ? sel : 0]++;
        off += len;
        out = qd_mirror_append(out, cell);   /* consumes cell either way */
        if (!out || RAY_IS_ERR(out)) break;
    }
    free(live);
    return out ? out : q_err(QE_WSFULL);
}

/* The accumulated fields as the q cells they are, and beside them `out`: ONE companion per KIND any field grows
 * (ADR 12/16), each a like-keyed dict per live row and the kind's own atom where the cell is NULL. */
static ray_t* qd_rec_build(int slot, qd_racc_t* acc, const qd_colmap_t* cm, bool flat, qd_cos_t* out) {
    const qd_rec_t* r = cm->rec;
    bool uni = cm->leaf->dk_type == QDUCK_TYPE_UNION, map = cm->leaf->dk_type == QDUCK_TYPE_MAP;
    if (out) memset(out, 0, sizeof *out);
    for (int i = 0; i < r->n; i++) {
        if (!acc->sub[i]) continue;
        /* a MAP's halves are entry-flat and get SLICED per row, so they must stay a list; every other
         * caller reads them per row and copes with either shape */
        ray_t* c = qd_rec_build(slot, acc->sub[i], &r->f[i].map, map, &acc->fcos[i]);
        if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_WSFULL);
        acc->col[i] = c;
    }
    /* THE emit decision, per kind and over the whole column: the shape mirror always builds (the caller drops it
     * where nothing flagged), a zoned field's offsets always ship, and every other kind ships where some field,
     * at some depth, asked for it.  A MAP carries its companions on its VALUES — its keys are the mirror's own
     * keys — so a key half that grows one has nowhere to put it. */
    const qd_tmap_t* tmof[QD_NCOS] = { NULL };
    for (int k = 0; out && k < QD_NCOS; k++) {
        bool always = k == QD_CO_ISNULL || k == QD_CO_TZOFF;
        if (map && !always && acc->fcos[0].co[k].want && acc->fcos[0].co[k].need)
            return q_duckdb_fail(slot, cm->leaf->logical, "a MAP's keys grow a companion its mirror cannot carry");
        for (int i = map ? 1 : 0; i < r->n; i++) {
            if (!acc->fcos[i].co[k].want || !(always || acc->fcos[i].co[k].need)) continue;
            if (!out->co[k].want) tmof[k] = r->f[i].map.leaf;
            out->co[k].want = true;
            out->co[k].need = out->co[k].need || acc->fcos[i].co[k].need;
        }
    }
    ray_t* col = qd_rec_pass(acc, r, uni, map, QD_CO_NONE, NULL);
    for (int k = 0; out && col && !RAY_IS_ERR(col) && k < QD_NCOS; k++) {
        if (!out->co[k].want) continue;
        ray_t* c = qd_rec_pass(acc, r, uni, map, k, tmof[k]);
        if (!c || RAY_IS_ERR(c)) { ray_release(col); col = c ? c : q_err(QE_WSFULL); break; }
        out->co[k].acc = flat ? c : qd_mirror_pack(c);
    }
    if (flat || !col || RAY_IS_ERR(col)) return col ? col : q_err(QE_WSFULL);
    /* a run of like-keyed dicts IS a table, so a bare list is a spelling nothing that rebuilds the value keeps */
    ray_t* packed = q_list_collapse(col);
    ray_release(col);
    return packed ? packed : q_err(QE_WSFULL);
}

/* ONE LIST cell of records: its own accumulator, read and built at once.  On the read a record's fields come from
 * the TYPE, never from the rows, so a per-cell build says exactly what a per-column one would — and a cell is what
 * the value materialises into anyway.  `col` and `cos` arrive fresh from qd_read_col's per-cell allocation (the
 * only caller that reaches a record leaf), so the built value and its companions REPLACE them. */
static ray_t* qd_read_rec_cell(int slot, ray_t* col, const qd_colmap_t* cm, duck_vector dv,
                         duck_idx_t from, duck_idx_t n, qd_cos_t* cos) {
    if (!n) return col;   /* the empty cell: the shell qd_new_col built already carries the record's shape */
    qd_racc_t* acc = qd_racc_new(cm);
    qd_cos_t   out;
    memset(&out, 0, sizeof out);   /* a build that never ran, or stopped part way, still leaves them to drop */
    ray_t* e = acc ? qd_read_rec(slot, acc, cm, dv, from, n) : q_err(QE_WSFULL);
    ray_t* v = e ? NULL : qd_rec_build(slot, acc, cm, false, &out);
    qd_racc_free(acc);
    if (!e && (!v || RAY_IS_ERR(v))) { e = v ? v : q_err(QE_WSFULL); v = NULL; }
    for (int k = 0; k < QD_NCOS; k++) {
        /* the COLUMN carries what any cell can grow; a cell that grew nothing in this kind takes the ATOM that
         * answers for the whole of it (ADR 16), so every cell's mirror stands the same number of columns deep */
        if (e || !cos->co[k].want) { q_duckdb_drop(out.co[k].acc); continue; }
        ray_release(cos->co[k].acc);
        cos->co[k].acc  = out.co[k].acc ? out.co[k].acc : qd_co_absent(k, cm->leaf);
        cos->co[k].need = cos->co[k].need || out.co[k].need;
    }
    ray_release(col);
    return e ? e : v;
}

/* Descriptor refinement: a row refines the physical map IFF its logical exists
 * AND agrees on both carrier and LIST depth; anything else DEGRADES. */
void q_duckdb_codec_refine(const char* cname, const qd_desc_t* desc, int64_t ndesc,
                           qd_colmap_t* cm) {
    const qd_desc_t* d = q_duckdb_schema_desc_find(cname, strlen(cname), desc, ndesc);
    qd_colmap_t ref;
    /* a record's fields are its refinement and only the dtype carries them, so no logical row may flatten it */
    if (cm->rec || !d || !q_duckdb_codec_parse_logical(d->logical, &ref) || ref.depth != cm->depth ||
        ref.leaf->dk_type != cm->leaf->dk_type)
        return;
    *cm = ref;
}

/* duck_result -> q table (does NOT destroy the result); desc = _q_schema rows.  THE reader: with `schema` the
 * envelope's schema table is built beside the data, here where the logical types are in hand; NULL drops it. */
ray_t* q_duckdb_codec_result_to_table(int slot, duck_result* res, const qd_desc_t* desc,
                                      int64_t ndesc, ray_t** schema) {
    int64_t ncols = (int64_t)QAPI.column_count(res);
    bool query = QAPI.result_return_type(*res) == QDuckReturnQuery;
    if (schema) *schema = NULL;
    if (ncols == 0) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }

    /* one block a column — its map, the column being read, its companion set, a record's accumulator — and, asked,
     * the schema rows beside it; ONE exit below releases whatever any path left in them */
    qd_colmap_t* cms   = q_duckdb_cols(ncols, sizeof *cms + 2 * sizeof(void*) + sizeof(qd_cos_t) + sizeof(bool));
    qd_desc_t*   rows  = cms && schema ? q_duckdb_cols(ncols, sizeof *rows + sizeof(bool)) : NULL;
    if (!cms || (schema && !rows)) { free(cms); return q_err(QE_WSFULL); }
    ray_t**      cols  = (ray_t**)(cms + ncols);
    qd_racc_t**  accs  = (qd_racc_t**)(cols + ncols);
    qd_cos_t*    cos   = (qd_cos_t*)(accs + ncols);
    bool*        taken = (bool*)(cos + ncols);   /* a store column that IS another column's companion */
    bool*        want  = rows ? (bool*)(rows + ncols) : NULL;   /* one flag a column, decided twice (see below) */
    ray_t*       err   = NULL;
    for (int64_t c = 0; !err && c < ncols; c++) {
        const char* nm = QAPI.column_name(res, (duck_idx_t)c);
        for (int64_t d = 0; !err && d < c; d++)   /* a q table keys its columns by name, so a second one is unreachable */
            if (strcmp(QAPI.column_name(res, (duck_idx_t)d), nm) == 0)
                { q_duckdb_err_stash(slot, "two result columns named %s", nm); err = q_err(QE_DUCKDB); }
        if (err) break;
        duck_logical_type lt = QAPI.column_logical_type(res, (duck_idx_t)c);
        duck_type miss = 0;
        const char* why = NULL;
        bool ok = lt && qd_map_read_logical(lt, &cms[c], &miss, &why);
        if (!ok && why) q_duckdb_err_stash(slot, "column %s: %s", nm, why);
        else if (!ok) q_duckdb_err_stash(slot, "column %s: %s has no mapping", nm, q_duckdb_type_name(miss));
        else {
            q_duckdb_codec_refine(nm, desc, ndesc, &cms[c]);
            /* the type alone answers here; whether the column also turns out to be EMPTY is not known until the
             * chunks are in, and the logical type is gone by then — so the row is spelled now and re-judged later */
            if (rows) err = q_duckdb_schema_desc_of(nm, lt, &cms[c], desc, ndesc, false, &rows[c], &want[c]);
        }
        if (lt) QAPI.destroy_logical_type(&lt);
        if (!ok && !err) err = q_err(QE_DUCKDB);
    }

    for (int64_t c = 0; c < ncols && !err; c++) {
        memset(&cos[c], 0, sizeof cos[c]);
        /* a record's companions are its FIELDS' (ADR 12), decided at the build; nothing accumulates here.  UNDER a
         * LIST the cells are where that build happens, so the column reads as the nest it is (ADR 16) */
        if (cms[c].rec && !cms[c].depth)
            { if (!(accs[c] = qd_racc_new(&cms[c]))) err = q_err(QE_WSFULL); continue; }
        qd_field_cos(&cms[c], &cos[c]);
        cos[c].co[QD_CO_ISNULL].want = qd_cm_nullless(&cms[c]);   /* a FLAT sentinel carrier's null is in band */
        cols[c] = qd_new_col(&cms[c], 8);
        if (!cols[c] || RAY_IS_ERR(cols[c])) { err = cols[c] ? cols[c] : q_err(QE_WSFULL); cols[c] = NULL; continue; }
        ray_t* e = qd_cos_alloc(&cms[c], &cos[c]);
        if (e) err = e;
    }

    duck_data_chunk chunk;
    while (!err && (chunk = QAPI.fetch_chunk(*res)) != NULL) {
        duck_idx_t n = QAPI.data_chunk_get_size(chunk);
        for (int64_t c = 0; c < ncols && !err; c++) {
            duck_vector dv = QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c);
            if (accs[c]) { err = qd_read_rec(slot, accs[c], &cms[c], dv, 0, n); continue; }
            cols[c] = qd_read_col(slot, cols[c], &cms[c], dv, 0, n, &cos[c]);
            if (!cols[c] || RAY_IS_ERR(cols[c])) { err = cols[c] ? cols[c] : q_err(QE_WSFULL); cols[c] = NULL; }
        }
        QAPI.destroy_data_chunk(&chunk);
    }
    for (int64_t c = 0; c < ncols; c++) {
        if (!accs[c]) continue;
        if (!err) {
            cols[c] = qd_rec_build(slot, accs[c], &cms[c], false, &cos[c]);
            if (!cols[c] || RAY_IS_ERR(cols[c])) { err = cols[c] ? cols[c] : q_err(QE_WSFULL); cols[c] = NULL; }
        }
        qd_racc_free(accs[c]);
    }
    /* emit-when-needed (ADR 16/17): the shape mirror ships only where some leaf, at some depth, is a null-less
     * NULL; a value companion only where some row asked for it — a nest says so through `need`, a flat raw
     * through the slot its count stands beside; and a zoned column's offsets always (ADR 21).  A record column
     * settled its own at the build, and reaches here already decided. */
    for (int64_t c = 0; c < ncols; c++) {
        bool    flatrec = cms[c].rec && !cms[c].depth;   /* settled at its own build; every other column decides here */
        ray_t** mir     = &cos[c].co[QD_CO_ISNULL].acc;
        if (*mir && !RAY_IS_ERR(*mir)) {
            if (qd_mirror_any(*mir)) *mir = qd_mirror_pack(*mir);
            else { ray_release(*mir); *mir = NULL; }
        }
        for (int i = QD_CO_NOTNULL; !flatrec && i < QD_NCOS; i++) {
            ray_t** val = &cos[c].co[i].acc;
            if (i == QD_CO_TZOFF || !*val || RAY_IS_ERR(*val)) continue;
            bool keep = cms[c].depth ? cos[c].co[i].need : i == QD_CO_RAW ? qd_raw_needed(cols[c], *val) : true;
            if (!keep) { ray_release(*val); *val = NULL; }
            else if (cms[c].depth) *val = qd_mirror_pack(*val);
        }
        ray_t** tz = &cos[c].co[QD_CO_TZOFF].acc;
        if (!flatrec && *tz && !RAY_IS_ERR(*tz)) *tz = qd_mirror_pack(*tz);
        for (int i = 0; i < QD_NCOS; i++) {
            ray_t** p = &cos[c].co[i].acc;
            if (!*p || !RAY_IS_ERR(*p)) continue;
            if (!err) err = *p; else ray_error_free(*p);
            *p = NULL;
        }
    }

    if (!err) err = qd_store_companions(slot, res, ncols, cms, cols, cos, taken);
    ray_t* tbl = err ? NULL : ray_table_new(ncols);
    for (int64_t c = 0; c < ncols; c++) {
        const char* nm = QAPI.column_name(res, (duck_idx_t)c);
        if (taken[c]) continue;
        if (tbl && !RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(nm, strlen(nm)), cols[c]);
        for (int i = 0; i < QD_NCOS && tbl && !RAY_IS_ERR(tbl); i++) {
            ray_t* co = cos[c].co[i].acc;
            if (!co) continue;
            qd_buf cn = {0};
            q_duckdb_codec_companion_col(&cn, nm, strlen(nm), i);
            if (cn.oom) { ray_release(tbl); tbl = NULL; err = q_err(QE_WSFULL); }
            else tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(cn.p, cn.len), co);
            q_duckdb_buf_free(&cn);
        }
    }
    if (!err && !tbl) err = q_err(QE_WSFULL);
    /* DDL spells itself as a row-less `Count`, so only the return type tells it from a query that found nothing */
    bool ddl = !err && !query && ray_table_nrows(tbl) == 0;
    if (ddl) {
        ray_release(tbl);
        tbl = RAY_NULL_OBJ;
        ray_retain(tbl);
    }
    /* the same question a SECOND time, now that the rows are in (ADR 15): a column that turned out to be empty and
     * surfaces as 0h keeps its type only through a row, so it earns one here even where its type alone needed none */
    if (!err && rows && !ddl) {
        bool empty = ray_table_nrows(tbl) == 0;
        qd_desc_t* keep = q_duckdb_cols(ncols, sizeof *keep);   /* SHALLOW copies: `rows` still owns the strings */
        int64_t nkeep = 0;
        if (!keep) err = q_err(QE_WSFULL);
        for (int64_t c = 0; keep && c < ncols; c++) {
            if (empty && !want[c]) want[c] = q_duckdb_schema_needs(&cms[c], rows[c].dtype, true) != 0;
            if (want[c] && !taken[c]) keep[nkeep++] = rows[c];
        }
        if (keep) {
            *schema = q_duckdb_schema_desc_table(keep, nkeep);
            free(keep);
            if (RAY_IS_ERR(*schema)) { err = *schema; *schema = NULL; }
        }
    }
    if (err && schema && *schema) { ray_release(*schema); *schema = NULL; }
    for (int64_t c = 0; c < ncols; c++) {   /* add_col retained its own refs; a read that stopped short left NULLs */
        q_duckdb_codec_map_free(&cms[c]);
        if (cols[c]) ray_release(cols[c]);
        qd_cos_drop(&cos[c]);
    }
    q_duckdb_schema_desc_free(rows, rows ? ncols : 0);
    free(cms);
    return err ? err : tbl;
}
static void qd_set_invalid(duck_vector dv, duck_idx_t r) {
    QAPI.vector_ensure_validity_writable(dv);
    QAPI.validity_set_row_invalid(QAPI.vector_get_validity(dv), r);
}

/* the boundary DuckDB itself draws (probed 2026-09-14): overlongs, surrogates and above U+10FFFF are invalid */
static bool qd_utf8_valid(const unsigned char* s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t   k;
        unsigned lo = 0x80, hi = 0xBF;
        if      (c >= 0xC2 && c <= 0xDF) k = 1;
        else if (c == 0xE0)              { k = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) k = 2;
        else if (c == 0xED)              { k = 2; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) k = 2;
        else if (c == 0xF0)              { k = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) k = 3;
        else if (c == 0xF4)              { k = 3; hi = 0x8F; }
        else return false;
        if (i + k >= n || s[i + 1] < lo || s[i + 1] > hi) return false;
        for (size_t j = 2; j <= k; j++) if ((s[i + j] & 0xC0) != 0x80) return false;
        i += k + 1;
    }
    return true;
}

/* the text of cell i of a text column, as the put reads it: a symbol, a char, a string-list cell (whose 0n is the
 * loaders' NULL) or a physical STR; QD_TX_NONE = a list cell that is no text */
enum { QD_TX_TEXT, QD_TX_NULL, QD_TX_NONE };
static int qd_text_at(ray_t* col, int64_t i, const char** p, int64_t* n) {
    *p = "";
    *n = 0;
    if (col->type == RAY_SYM) {
        ray_t* s = ray_sym_vec_cell(col, i);   /* borrowed; ` (sym 0 / empty) writes '' — a value */
        if (s) { *p = ray_str_ptr(s); *n = (int64_t)ray_str_len(s); }
        return QD_TX_TEXT;
    }
    if (col->type == RAY_CHARV) { *p = (const char*)ray_vec_get(col, i); *n = 1; return QD_TX_TEXT; }
    if (col->type == RAY_LIST) {
        ray_t* cell = ray_list_get(col, i);
        if (qd_cell_is_0n(cell)) return QD_TX_NULL;
        return cell && q_str_text_bytes(cell, p, n) ? QD_TX_TEXT : QD_TX_NONE;
    }
    size_t len = 0;
    *p = ray_str_vec_get(col, i, &len);
    *n = (int64_t)len;
    if (!*p) *p = "";
    return QD_TX_TEXT;
}

/* THE VARCHAR put: q text is bytes and DuckDB VARCHAR is UTF-8, so what it cannot hold is refused, never NULL — in
 * ONE line, the first offender and how many cells of the column share its fault */
static ray_t* qd_put_text(int slot, duck_vector dv, duck_idx_t r, ray_t* col, int64_t i, const qd_tmap_t* tm) {
    const char* p;
    int64_t     n;
    int         k = qd_text_at(col, i, &p, &n);
    if (k == QD_TX_NULL) { qd_set_invalid(dv, r); return NULL; }
    if (k == QD_TX_NONE) return q_duckdb_fail(slot, tm->logical, "cell is not text");
    if (qd_utf8_valid((const unsigned char*)p, (size_t)n)) {
        QAPI.vector_assign_string_element_len(dv, r, p, (duck_idx_t)n);
        return NULL;
    }
    int64_t bad = 1;
    for (int64_t j = i + 1; j < col->len; j++)
        if (qd_text_at(col, j, &p, &n) == QD_TX_TEXT && !qd_utf8_valid((const unsigned char*)p, (size_t)n)) bad++;
    char why[96];
    snprintf(why, sizeof why, "text is not valid UTF-8 in %lld cell%s, the first at %lld", (long long)bad,
             bad == 1 ? "" : "s", (long long)i);
    return q_duckdb_fail(slot, tm->logical, why);
}

/* THE write cell codec: n elements of col from `base` into dv rows from `dst`.  A sentinel
 * carrier's null-patterned cell writes NULL unless `keep` flags the row — its _q_notnull companion
 * per row, or a hi companion for EVERY row (a 128-bit word's halves are bits, never nulls) — when
 * the raw bits go through as the value they were. */
static ray_t* qd_write_leaf(int slot, duck_vector dv, ray_t* col, const qd_tmap_t* tm,
                            int64_t base, duck_idx_t dst, int64_t n, ray_t* keep, ray_t* mir) {
    void* data = QAPI.vector_get_data(dv);
    for (int64_t i = 0; i < n; i++) {
        int64_t src = base + i;
        duck_idx_t r = dst + (duck_idx_t)i;
        /* a hi companion is a long a row, so every row keeps; a boolean one answers per element, and an ATOM
         * answers for the whole of what it stands beside — the discrimination ADR 16's mirror already reads */
        bool boolco = keep && (keep->type == RAY_BOOL || keep->type == -RAY_BOOL);
        bool kp     = keep && (!boolco || qd_mirror_bit(keep, src));
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
            case RAY_I32: {
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32 && !kp) qd_set_invalid(dv, r);
                else                      ((int32_t*)data)[r] = v;
                break;
            }
            case RAY_MONTH: {    /* the DATE of the first day; 0Wm/-0Wm are the same bits as DATE's infinities */
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32 && !kp) { qd_set_invalid(dv, r); break; }
                if (qd_date_inf(v)) { ((int32_t*)data)[r] = v; break; }
                int64_t d = month_payload_as_days(v) + QD_EPOCH_DAYS;
                if (d > INT32_MAX || d < -(int64_t)INT32_MAX)
                    return q_duckdb_fail(slot, tm->logical, "outside the DATE range");
                ((int32_t*)data)[r] = (int32_t)d;
                break;
            }
            case RAY_MINUTE:
            case RAY_SECOND: {   /* ADR 19: micros-only, no domain — INTERVAL holds every count with 72x headroom */
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32 && !kp) { qd_set_invalid(dv, r); break; }
                qd_put_interval(data, r, (int64_t)v * (tm->ray_type == RAY_MINUTE ? 60000000LL : 1000000LL));
                break;
            }
            case RAY_I64:
            case RAY_TIMESPAN: {
                int64_t v = *(int64_t*)ray_vec_get(col, src);
                if (v == NULL_I64 && !kp) qd_set_invalid(dv, r);
                else                      ((int64_t*)data)[r] = v;
                break;
            }
            case RAY_F32: {      /* a kept NaN writes as DuckDB's own (positive quiet): q's NaN sign is not a SQL fact */
                float v = *(float*)ray_vec_get(col, src);
                if (v != v && !kp) qd_set_invalid(dv, r);       /* 0Ne -> NULL */
                else               ((float*)data)[r] = v != v ? __builtin_nanf("") : v;
                break;
            }
            case RAY_F64: {
                double v = *(double*)ray_vec_get(col, src);
                if (v != v && !kp) qd_set_invalid(dv, r);       /* 0n -> NULL */
                else               ((double*)data)[r] = v != v ? __builtin_nan("") : v;
                break;
            }
            case RAY_DATETIME: { /* float days -> the TIMESTAMP's µs at q's millisecond grain; ±0w is ±infinity */
                double v = *(double*)ray_vec_get(col, src);
                if (v != v) {                                   /* 0Nz -> NULL; no TIMESTAMP spells it as a value */
                    if (kp) return q_duckdb_fail(slot, tm->logical, "no TIMESTAMP holds the datetime null");
                    qd_set_invalid(dv, r);
                    break;
                }
                if (v > 0 && isinf(v))  { ((int64_t*)data)[r] = INT64_MAX;  break; }
                if (v < 0 && isinf(v))  { ((int64_t*)data)[r] = -INT64_MAX; break; }
                double ms = v * 86400000.0;
                if (!(ms >= -9.0e15 && ms <= 9.0e15))
                    return q_duckdb_fail(slot, tm->logical, "outside the TIMESTAMP range");
                int64_t n = llround(ms);
                if ((double)n / 86400000.0 != v)   /* the count the reader rebuilds from must give the value back */
                    return q_duckdb_fail(slot, tm->logical, "below the millisecond");
                ((int64_t*)data)[r] = (n + QD_EPOCH_MS) * 1000;
                break;
            }
            case RAY_SYM:
            case RAY_CHARV:      /* " " (the char null) and ` are ordinary values */
            case RAY_STR: {      /* string column: text cells, or physical STR */
                ray_t* e = qd_put_text(slot, dv, r, col, src, tm);
                if (e) return e;
                break;
            }
            case RAY_LIST: {     /* byte-vector cell -> BLOB, boolean-vector cell -> BIT */
                ray_t* cell = ray_list_get(col, src);
                if (qd_cell_is_0n(cell)) { qd_set_invalid(dv, r); break; }
                if (tm->dk_type == QDUCK_TYPE_BIT) {
                    if (!cell || (cell->type != RAY_BOOL && cell->len))
                        return q_duckdb_fail(slot, tm->logical, "cell is not a boolean vector");
                    /* DuckDB has no zero-bit BIT, so an empty cell has exactly one meaning: the NULL a read fills
                     * with one.  Flag it here — the isnull companion never reaches the leaf. */
                    if (!cell->len) { qd_set_invalid(dv, r); break; }
                    ray_t* e = qd_bits_write(dv, r, cell);
                    if (e) return e;
                    break;
                }
                if (!cell || cell->type != RAY_BYTE_ONLY)
                    return q_duckdb_fail(slot, tm->logical, "cell is not a byte vector");
                const char* bp = cell->len ? (const char*)ray_vec_get(cell, 0) : "";
                QAPI.vector_assign_string_element_len(dv, r, bp,
                                                      (duck_idx_t)cell->len);
                break;
            }
            case RAY_DATE: {     /* a temporal's companion is raw, consumed by the cast leg: 0N here is NULL */
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32) { qd_set_invalid(dv, r); break; }
                if (qd_date_inf(v)) { ((int32_t*)data)[r] = v; break; }
                if (v >= INT32_MAX - QD_EPOCH_DAYS)   /* the count ON the sentinel would read back as infinity */
                    return q_duckdb_fail(slot, tm->logical, "above the q epoch shift");
                ((int32_t*)data)[r] = v + QD_EPOCH_DAYS;
                break;
            }
            case RAY_TIME: {     /* ms -> µs; a declared TIME is the cast leg's business, never the appender's */
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32) { qd_set_invalid(dv, r); break; }
                qd_put_interval(data, r, (int64_t)v * 1000);
                break;
            }
            case RAY_TIMESTAMP: {
                int64_t v = *(int64_t*)ray_vec_get(col, src);
                if (v == NULL_I64) { qd_set_invalid(dv, r); break; }
                if (qd_ts_inf(v)) { ((int64_t*)data)[r] = v; break; }
                if (v >= INT64_MAX - QD_EPOCH_NS)
                    return q_duckdb_fail(slot, tm->logical, "above the q epoch shift");
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
        /* the fill goes down first, then the mirror's leaf says the slot was NULL all along */
        if (mir && qd_mirror_bit(mir, src)) qd_set_invalid(dv, r);
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
        int64_t len = q_duckdb_codec_rec_rows(cell);   /* a record cell is the TABLE its rows are */
        count[depth - 1] += len;
        if (depth > 1) {
            ray_t* e = qd_count_levels(cell, depth - 1, 0, len, count);
            if (e) return e;
        }
    }
    return NULL;
}

static ray_t* qd_write_data(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm,
                            int64_t base, duck_idx_t dst, int64_t n, ray_t* keep, ray_t* mir);

static ray_t* qd_write_nested(int slot, qd_level_t* lv, const qd_colmap_t* cm, int level,
                              ray_t* col, int64_t base, duck_idx_t dst, int64_t n, ray_t* mir, ray_t* keep) {
    duck_list_entry* ent  = (duck_list_entry*)QAPI.vector_get_data(lv[level].vec);
    const qd_colmap_t bot = { cm->leaf, cm->rec, 0, false };   /* what a level-1 cell holds: a leaf, or a record */
    if (!ent) return q_err(QE_DUCKDB);
    for (int64_t i = 0; i < n; i++) {
        ray_t* cell = ray_list_get(col, base + i);
        ray_t* mc   = qd_mirror_item(mir, base + i);
        /* ADR 16: the element companion mirrors the same shape, and an atom answers for the whole of it */
        ray_t* kc   = keep && keep->type == -RAY_BOOL ? keep : qd_mirror_item(keep, base + i);
        if (!cell) return q_err(QE_DUCKDB);
        duck_idx_t off = lv[level - 1].used;
        if (qd_cell_is_0n(cell) || qd_mirror_bit(mir, base + i)) {
            ent[dst + (duck_idx_t)i].offset = off;
            ent[dst + (duck_idx_t)i].length = 0;
            qd_set_invalid(lv[level].vec, dst + (duck_idx_t)i);
            continue;
        }
        int64_t    len = q_duckdb_codec_rec_rows(cell);
        ent[dst + (duck_idx_t)i].offset = off;
        ent[dst + (duck_idx_t)i].length = (uint64_t)len;
        lv[level - 1].used += (duck_idx_t)len;
        ray_t* e = level > 1
                       ? qd_write_nested(slot, lv, cm, level - 1, cell, 0, off, len, mc, kc)
                       : qd_write_data(slot, lv[0].vec, cell, &bot, 0, off, len, kc, mc);
        if (e) return e;
    }
    return NULL;
}

static ray_t* qd_write_rec(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm, int64_t base,
                           duck_idx_t dst, int64_t n, ray_t* mir, ray_t* keep);

/* Every LIST level is sized BEFORE any data pointer is taken: reserve reallocates the child, so a half-written
 * spine would dangle.  A level GROWS from what it already holds — a record under a list writes its fields cell
 * by cell into one shared spine, so the size a cell adds is never the size the vector has. */
static ray_t* qd_write_data(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm,
                            int64_t base, duck_idx_t dst, int64_t n, ray_t* keep, ray_t* mir) {
    if (cm->rec && !cm->depth) return qd_write_rec(slot, dv, col, cm, base, dst, n, mir, keep);
    if (cm->depth == 0) return qd_write_leaf(slot, dv, col, cm->leaf, base, dst, n, keep, mir);
    int64_t count[QD_MAX_DEPTH] = { 0 };
    ray_t*  e = qd_count_levels(col, cm->depth, base, n, count);
    if (e) return e;
    qd_level_t lv[QD_MAX_DEPTH + 1];
    lv[cm->depth].vec  = dv;
    lv[cm->depth].used = 0;
    for (int l = cm->depth - 1; l >= 0; l--) {
        duck_idx_t have = QAPI.list_vector_get_size(lv[l + 1].vec);
        if (QAPI.list_vector_reserve(lv[l + 1].vec, have + (duck_idx_t)count[l]) != QDuckSuccess ||
            QAPI.list_vector_set_size(lv[l + 1].vec, have + (duck_idx_t)count[l]) != QDuckSuccess)
            return q_err(QE_DUCKDB);
        lv[l].vec  = QAPI.list_vector_get_child(lv[l + 1].vec);
        lv[l].used = have;
        if (!lv[l].vec) return q_err(QE_DUCKDB);
    }
    return qd_write_nested(slot, lv, cm, cm->depth, col, base, dst, n, mir, keep);
}

/* The companion's meaning follows the column's group (the strip has matched them): a null-less column's
 * companion is the SHAPE MIRROR the writer walks alongside the value, a sentinel carrier's rides into the leaf
 * as `keep` (a hi companion keeps every row); a raw parent reaches here maskless, its count a stage column.
 * A NESTED column brings both: the mirror for its NULL cells, `keep` for its elements' own null pattern. */
static ray_t* qd_write_col(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm,
                           int64_t base, duck_idx_t dst, int64_t n, ray_t* mask, ray_t* keep) {
    bool mirror = qd_cm_nullless(cm);   /* the COLUMN decides, not whether a mirror happens to be present */
    return qd_write_data(slot, dv, col, cm, base, dst, n, mirror ? keep : mask, mirror ? mask : NULL);
}

/* Append a q table through the appender in vector-size chunks (cms[] pre-validated; masks[] and keeps[] = the
 * consumed companions, the shape mirror and a nested column's element companion, NULL where a column has neither;
 * catalog NULL = the default, "temp" = staging). */
static ray_t* append_table_rows(int slot, const qd_name_t* nm, bool temp, ray_t* tbl,
                                const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* keeps) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);
    /* the appender takes the parts the name grammar found, never the text: catalog, the schema the name
     * resolved in, then the table */
    const char* catalog = temp ? "temp" : nm->n == 3 ? nm->part[0] : NULL;
    const char* schema  = temp || !*nm->schema ? "main" : nm->schema;

    duck_appender app = NULL;
    if (QAPI.appender_create_ext(q_duckdb_con(slot), catalog, schema, nm->part[nm->n - 1], &app)
            != QDuckSuccess) {
        q_duckdb_err_stash(slot, "%s", QD_TEXT(QAPI.appender_error(app)));
        QAPI.appender_destroy(&app);
        return q_err(QE_DUCKDB);
    }

    duck_logical_type* ltypes = q_duckdb_cols(ncols, sizeof *ltypes);
    if (!ltypes) { QAPI.appender_destroy(&app); return q_err(QE_WSFULL); }
    ray_t* err = NULL;
    for (int64_t c = 0; c < ncols; c++)
        if (!(ltypes[c] = qd_make_logical(&cms[c]))) err = q_err(QE_WSFULL);
    duck_data_chunk chunk = err ? NULL : QAPI.create_data_chunk(ltypes, (duck_idx_t)ncols);
    if (!err && !chunk) err = q_err(QE_DUCKDB);

    int64_t vecsz = (int64_t)QAPI.vector_size();
    if (vecsz <= 0) vecsz = 2048;
    for (int64_t base = 0; base < nrows && !err; base += vecsz) {
        int64_t n = nrows - base < vecsz ? nrows - base : vecsz;
        QAPI.data_chunk_reset(chunk);
        for (int64_t c = 0; c < ncols && !err; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
            err = qd_write_col(slot, QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c),
                               col, &cms[c], base, 0, n, masks[c], keeps[c]);
        }
        if (!err) {
            QAPI.data_chunk_set_size(chunk, (duck_idx_t)n);
            if (QAPI.append_data_chunk(app, chunk) != QDuckSuccess) {
                q_duckdb_err_stash(slot, "%s", QD_TEXT(QAPI.appender_error(app)));
                err = q_err(QE_DUCKDB);
            }
        }
    }

    if (chunk) QAPI.destroy_data_chunk(&chunk);
    for (int64_t c = 0; c < ncols; c++) if (ltypes[c]) QAPI.destroy_logical_type(&ltypes[c]);
    free(ltypes);

    /* explicit flush first: destroy frees the error text (codex round 3) */
    if (!err && QAPI.appender_flush(app) != QDuckSuccess) {
        q_duckdb_err_stash(slot, "%s", QD_TEXT(QAPI.appender_error(app)));
        err = q_err(QE_DUCKDB);
    }
    if (QAPI.appender_destroy(&app) != QDuckSuccess && !err)
        err = q_err(QE_DUCKDB);
    return err;
}

/* The appender moves the data without issuing SQL, so it writes its own log line — a `/` comment, which is not a
 * statement in any dialect, so nothing can mistake it for one and the timeline has no hole where the write was. */
ray_t* q_duckdb_codec_append_table(int slot, const qd_name_t* nm, bool temp, ray_t* tbl,
                                   const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* keeps) {
    int64_t t0  = q_dotz_now_ns(0);
    ray_t*  err = append_table_rows(slot, nm, temp, tbl, cms, masks, keeps);
    char    what[1024];
    const char* catalog = temp ? "temp" : nm->n == 3 ? nm->part[0] : "";
    const char* schema  = temp || !*nm->schema ? "main" : nm->schema;
    snprintf(what, sizeof what, "/ appender %s%s%s.%s (%lld rows)", catalog, *catalog ? "." : "", schema,
             nm->part[nm->n - 1], (long long)ray_table_nrows(tbl));
    q_duckdb_stmt_note(slot, what, t0, !err, ray_table_nrows(tbl), err ? q_duckdb_err_text(slot) : NULL);
    return err;
}

/* Validate every column against the write manifest; fills cms (sized by the caller to the table). */
ray_t* q_duckdb_codec_check_table(int slot, ray_t* tbl, qd_colmap_t* cms) {
    int64_t ncols = ray_table_ncols(tbl);
    /* EVERY slot is emptied before the first can fail, so a caller's free walks the whole array either way */
    for (int64_t c = 0; c < ncols; c++) cms[c] = (qd_colmap_t){ NULL, NULL, 0, false };
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
        ray_t* nm  = ray_sym_str(ray_table_col_name(tbl, c));
        const char* why = "the q column has no SQL meaning";
        if (nm && col && qd_map_write(slot, col, 0, &cms[c], &why)) {
            /* a flat enum column was decayed at the door, so one still here is under a LIST */
            if (cms[c].leaf->ray_type != RAY_ENUM) continue;
            return q_duckdb_fail(slot, "enum", "a nested enum column has no mapping");
        }
        char what[300];
        snprintf(what, sizeof what, "column %.*s", (int)(nm ? ray_str_len(nm) : 1), nm ? ray_str_ptr(nm) : "?");
        return q_duckdb_fail(slot, what, why);
    }
    return NULL;
}

/* the declaration one level down (owned): kind < 0 = a LIST/ARRAY's element, else record `kind`'s child i */
static duck_logical_type qd_lt_under(duck_logical_type lt, int kind, int i) {
    if (!lt) return NULL;
    duck_type id = QAPI.get_type_id(lt);
    if (kind < 0) return id == QDUCK_TYPE_LIST ? QAPI.list_type_child_type(lt)
                       : id == QDUCK_TYPE_ARRAY ? QAPI.array_type_child_type(lt) : NULL;
    if (id != (duck_type)kind) return NULL;
    return id == QDUCK_TYPE_MAP    ? (i ? QAPI.map_type_value_type(lt) : QAPI.map_type_key_type(lt))
         : id == QDUCK_TYPE_UNION  ? QAPI.union_type_member_type(lt, (duck_idx_t)i)
                                   : QAPI.struct_type_child_type(lt, (duck_idx_t)i);
}

/* the first field whose q type only a sidecar row could bring back (no bare read answers it) and whose declared
 * type does not read as it by itself (ENUM reads as symbol; VARCHAR does not) — a MAP's VARCHAR keys are exempt,
 * symbols being how the reader spells them */
static bool qd_field_refined(const qd_colmap_t* cm, duck_logical_type lt, char* path, size_t cap, const char** type) {
    duck_logical_type at = NULL;
    for (int d = 0; d < cm->depth && (d == 0 ? lt : at); d++) {
        duck_logical_type next = qd_lt_under(d ? at : lt, -1, 0);
        if (at) QAPI.destroy_logical_type(&at);
        at = next;
    }
    duck_logical_type here = cm->depth ? at : lt;
    bool refined = false;
    if (!cm->rec) {
        const qd_tmap_t* own = here ? qd_map_read(QAPI.get_type_id(here)) : NULL;
        refined = !cm->leaf->read_canon && !(own && own->ray_type == cm->leaf->ray_type);
        if (refined) *type = cm->leaf->logical;
    } else {
        size_t n   = strlen(path);
        bool   map = cm->leaf->dk_type == QDUCK_TYPE_MAP;
        for (int i = 0; !refined && i < cm->rec->n; i++) {
            const qd_colmap_t* f = &cm->rec->f[i].map;
            if (map && i == 0 && !f->rec && !f->depth && f->leaf->ray_type == RAY_SYM) continue;
            snprintf(path + n, cap - n, "%s%s", n ? "." : "", cm->rec->f[i].name);
            duck_logical_type ft = qd_lt_under(here, (int)cm->leaf->dk_type, i);
            refined = qd_field_refined(f, ft, path, cap, type);
            if (ft) QAPI.destroy_logical_type(&ft);
        }
        if (!refined) path[n] = '\0';
    }
    if (at) QAPI.destroy_logical_type(&at);
    return refined;
}

/* the record law (ruled 2026-09-14): the sidecar's `logical` names no field, so a refined one is refused, never
 * read back as its carrier */
ray_t* q_duckdb_codec_check_fields(int slot, ray_t* tbl, int64_t c, duck_logical_type lt, const qd_colmap_t* cm) {
    char        path[512] = "";
    const char* type;
    if (!cm->rec || !qd_field_refined(cm, lt, path, sizeof path, &type)) return NULL;
    ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
    char what[300], why[640];
    snprintf(what, sizeof what, "column %.*s", (int)(nm ? ray_str_len(nm) : 1), nm ? ray_str_ptr(nm) : "?");
    snprintf(why, sizeof why, "field %s is %s %s - no q refinement inside a record", path,
             strchr("aeiou", *type) ? "an" : "a", type);
    return q_duckdb_fail(slot, what, why);
}

/* THE enum site, at the write door's entry (2026-09-14): decayed before the schema check, the sidecar, the appender
 * or rekey can see a 20h column, an enum is STORED as its symbols (ADR 8) and cannot go stale when its domain changes */
ray_t* q_duckdb_codec_decay_enums(int slot, ray_t* tbl, qd_enum_t* en) {
    int64_t ncols = ray_table_ncols(tbl);
    bool any = false;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
        bool   e   = col && col->type == RAY_ENUM;
        en[c].dom  = e ? q_enum_domain(col) : 0;
        en[c].attr = e ? q_attr_letter(col) : 0;
        any |= e;
    }
    if (!any) { ray_retain(tbl); return tbl; }
    ray_t* out = ray_table_new(ncols);
    for (int64_t c = 0; c < ncols && out && !RAY_IS_ERR(out); c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
        if (!en[c].dom) { out = ray_table_add_col(out, ray_table_col_name(tbl, c), col); continue; }   /* retains */
        ray_t* val = q_enum_resolve(col);
        if (!val || RAY_IS_ERR(val) || val->type != RAY_SYM) {
            if (val) ray_release(val);
            ray_release(out);
            return q_duckdb_fail(slot, "enum", "the domain resolves to no symbol list");
        }
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), val);       /* retains */
        ray_release(val);
    }
    return out ? out : q_err(QE_WSFULL);
}

static ray_t* qd_reject(int slot, ray_t* nm, const char* why) {
    char what[300];
    snprintf(what, sizeof what, "companion %.*s", (int)ray_str_len(nm), ray_str_ptr(nm));
    return q_duckdb_fail(slot, what, why);
}

/* A companion is admissible on a parent iff some manifest row of the parent's carrier can emit it: a j column may
 * carry notnull (BIGINT) or hi (the 128-bit family), a temporal raw, a null-less carrier isnull and nothing else. */
static bool qd_co_admissible(const qd_colmap_t* pcm, int kind) {
    if (pcm->undet) return true;   /* ADR 15: no carrier to judge against — the DECLARATION answers for this one */
    if (q_duckdb_codec_is_rec(pcm->leaf)) {   /* ADR 12: a record grows what its FIELDS grow, plus every record's mirror */
        if (kind == QD_CO_ISNULL || !pcm->rec) return true;   /* fields deferred: the DECLARATION answers, as for undet */
        for (int i = 0; i < pcm->rec->n; i++)
            if (qd_co_admissible(&pcm->rec->f[i].map, kind)) return true;
        return false;
    }
    for (size_t i = 0; kind == QD_CO_TZOFF && i < QD_NTYPES; i++)   /* ADR 21: it rides beside, so nesting is no bar */
        if (QD_TYPES[i].ray_type == pcm->leaf->ray_type &&
            q_duckdb_codec_zoned(&(const qd_colmap_t){ &QD_TYPES[i], NULL, 0, false })) return true;
    if (kind == QD_CO_TZOFF) return false;
    if (kind == QD_CO_ISNULL) return qd_cm_nullless(pcm);
    /* Depth is no bar to any of them: an element companion is the appender's to consume and the appender reaches
     * every depth, while hi and raw are rebuilt by a CAST LEG, which reaches every depth too now that the legs
     * lift elementwise (ADR 21).  What the companion has to be is its parent's SHAPE, and qd_mirror_fits_col is
     * what judges that. */
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (QD_TYPES[i].ray_type == pcm->leaf->ray_type && qd_leaf_co_kind(&QD_TYPES[i]) == kind) return true;
    return false;
}

static int64_t qd_sym_id(ray_t* v, int64_t i);

/* one row of any column shape, OWNED: a table hands back its row dict, a list its cell, a vector its atom */
static ray_t* qd_row_at(ray_t* v, int64_t i) {
    if (v->type == RAY_TABLE) return q_table_row_at(v, i);
    if (v->type == RAY_LIST)  { ray_t* c = ray_list_get(v, i); ray_retain(c); return c; }
    return q_join_item(v, i);
}

/* Does this mirror have the value's SHAPE (ADR 16)?  A boolean ATOM answers for the whole of it at any level;
 * anything else must match what it stands beside, element for element and key for key — a MAP's entry ORDER is
 * data, so a mirror that reorders it would flag the wrong entry.  A mirror that does not fit is REFUSED at the
 * door, never read as 0b, because a dropped flag writes the fill as if it were the value. */
static bool qd_mirror_fits(const qd_colmap_t* cm, ray_t* v, ray_t* m) {
    if (!v || !m || RAY_IS_ERR(v) || RAY_IS_ERR(m)) return false;
    if (m->type == -RAY_BOOL) return true;
    if (cm->depth > 0) {
        /* a record cell is the TABLE its rows are, and so is the mirror standing beside it — the LEAF says so,
         * since a record whose fields are deferred (a UNION's, a MAP's) is one all the same */
        bool    rec    = q_duckdb_codec_is_rec(cm->leaf);
        bool    rows   = rec && cm->depth == 1;
        int64_t n      = q_duckdb_codec_rec_rows(v);
        bool    leaves = cm->depth == 1 && !rec;
        if ((m->type != RAY_LIST && m->type != RAY_BOOL && !(rows && m->type == RAY_TABLE)) ||
            q_duckdb_codec_rec_rows(m) != n) return false;
        if (m->type == RAY_BOOL) return true;   /* the run of atoms, at whatever level it stands */
        const qd_colmap_t ch = { cm->leaf, cm->rec, cm->depth - 1, false };
        for (int64_t i = 0; i < n; i++) {
            ray_t* ve = qd_row_at(v, i);
            ray_t* me = qd_row_at(m, i);
            bool   ok = leaves ? me && !RAY_IS_ERR(me) && me->type == -RAY_BOOL : qd_mirror_fits(&ch, ve, me);
            q_duckdb_drop(ve);
            q_duckdb_drop(me);
            if (!ok) return false;
        }
        return true;
    }
    /* a flat leaf's row mirror is the atom handled above; only a record row goes deeper */
    if (!q_duckdb_codec_is_rec(cm->leaf) || v->type != RAY_DICT || m->type != RAY_DICT) return false;
    ray_t* vk = ray_dict_keys(v);
    if (!q_match_rec(vk, ray_dict_keys(m))) return false;
    if (!cm->rec) return true;   /* the fields are not derivable (a MAP's keys vary): the keys ARE the check */
    for (int64_t j = 0; j < ray_len(vk); j++) {
        int64_t id = qd_sym_id(vk, j);
        const qd_colmap_t* fm = NULL;
        for (int f = 0; !fm && f < cm->rec->n; f++)
            if (ray_sym_intern_runtime(cm->rec->f[f].name, (int64_t)strlen(cm->rec->f[f].name)) == id)
                fm = &cm->rec->f[f].map;
        if (!fm) continue;
        ray_t* vv = qd_row_at(ray_dict_vals(v), j);
        ray_t* mv = qd_row_at(ray_dict_vals(m), j);
        bool   ok = qd_mirror_fits(fm, vv, mv);
        q_duckdb_drop(vv);
        q_duckdb_drop(mv);
        if (!ok) return false;
    }
    return true;
}

/* One row-mirror per row, whatever spells them.  A boolean VECTOR is the flat spelling of a run of atoms, so it
 * is admissible over a NESTED column too and says exactly what an atom says: this row is (not) NULL, and nothing
 * inside it is - which is what the reader itself emits when every flagged row is a whole-row NULL. */
static bool qd_mirror_fits_col(const qd_colmap_t* cm, ray_t* col, ray_t* m) {
    int64_t n = q_duckdb_codec_rec_rows(col);
    if (q_duckdb_codec_rec_rows(m) != n) return false;
    if (m->type == RAY_BOOL) return true;               /* every row-mirror is an atom */
    if (m->type != RAY_LIST && m->type != RAY_TABLE) return false;
    for (int64_t i = 0; i < n; i++) {
        ray_t* v = qd_row_at(col, i);
        ray_t* r = qd_row_at(m, i);
        bool   ok = qd_mirror_fits(cm, v, r);
        q_duckdb_drop(v);
        q_duckdb_drop(r);
        if (!ok) return false;
    }
    return true;
}

/* ADR 16/21: a VALUE companion mirrors the value's shape too, but its leaves are its own carrier, not a boolean —
 * an int per element for the zone offset, a text per element for a raw spelling, a long per element for every
 * other count.  One flat run of them over a nested level is what an ATOM is to the isnull mirror: every element
 * answered at once.  Only the SHAPE is the bridge's business here; what a leaf HOLDS is the cast leg's, and
 * DuckDB's — a zone outside ±15:59:59 is DuckDB's own refusal on the way back. */
static bool qd_co_run(ray_t* m, int lf)  { return lf == RAY_STR ? m->type == RAY_LIST : m->type == lf; }
static bool qd_co_atom(ray_t* m, int lf) { return lf == RAY_STR ? m->type == RAY_CHARV : m->type == -lf; }

static bool qd_leaf_fits(const qd_colmap_t* cm, ray_t* col, ray_t* m, int lf) {
    if (!col || !m || RAY_IS_ERR(m)) return false;
    int64_t n = q_duckdb_codec_rec_rows(col);
    if (q_duckdb_codec_rec_rows(m) != n) return false;
    /* nothing to stand beside — but a carrier still has to be one the leaves could wear, and the bare () an
     * empty column reads as is the one spelling that has no carrier to be judged by */
    if (!n) return qd_co_run(m, lf) || (m->type == RAY_LIST && !m->len);
    if (!cm->depth) return qd_co_run(m, lf);
    if (qd_co_run(m, lf) && m->type != RAY_LIST) return true;
    if (m->type != RAY_LIST) return false;
    const qd_colmap_t child = { cm->leaf, cm->rec, cm->depth - 1, false };
    for (int64_t i = 0; i < m->len; i++) {
        ray_t* mi = ray_list_get(m, i);         /* borrowed */
        if (mi && qd_co_atom(mi, lf)) continue;
        ray_t* vi = qd_row_at(col, i);
        bool   ok = qd_leaf_fits(&child, vi, mi, lf);
        q_duckdb_drop(vi);
        if (!ok) return false;
    }
    return true;
}

/* the OR of two shape mirrors (ADR 16): an atom 1b answers for the whole of what it stands beside, a leaf vector
 * ORs elementwise, a nest recurses; either side NULL is the other.  Owned. */
static ray_t* qd_mask_or(ray_t* a, ray_t* b) {
    if (!a || !b) { ray_t* v = a ? a : b; ray_retain(v); return v; }
    if (a->type == -RAY_BOOL || b->type == -RAY_BOOL) {
        ray_t* atom = a->type == -RAY_BOOL ? a : b;
        ray_t* rest = atom == a ? b : a;
        if (atom->b8 || rest->type == -RAY_BOOL) return ray_bool(atom->b8 || (rest->type == -RAY_BOOL && rest->b8));
        ray_retain(rest);
        return rest;
    }
    if (a->type == RAY_BOOL && b->type == RAY_BOOL && a->len == b->len) {
        ray_t* out = ray_vec_new(RAY_BOOL, a->len ? a->len : 1);
        for (int64_t i = 0; out && !RAY_IS_ERR(out) && i < a->len; i++) {
            uint8_t f = *(uint8_t*)ray_vec_get(a, i) | *(uint8_t*)ray_vec_get(b, i);
            out = ray_vec_append(out, &f);
        }
        return out ? out : q_err(QE_WSFULL);
    }
    if (a->type != RAY_LIST || b->type != RAY_LIST || a->len != b->len) return q_err(QE_DUCKDB);
    ray_t* out = ray_list_new(a->len ? a->len : 1);
    for (int64_t i = 0; out && !RAY_IS_ERR(out) && i < a->len; i++) {
        ray_t* e = qd_mask_or(ray_list_get(a, i), ray_list_get(b, i));
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e ? e : q_err(QE_WSFULL); }
        out = ray_list_append(out, e);   /* retains */
        ray_release(e);
    }
    return out ? qd_mirror_pack(out) : q_err(QE_WSFULL);
}

/* a store column spelled <c>_q_<kind> IS <c>'s companion (ruled 2026-09-14: a read assumes a q-sourced table), judged
 * as the writer judges one and folded with the reader's own — OR for a mask, the store's for a count */
static ray_t* qd_store_companions(int slot, duck_result* res, int64_t ncols, const qd_colmap_t* cms, ray_t** cols,
                                  qd_cos_t* cos, bool* taken) {
    for (int64_t d = 0; d < ncols; d++) {
        const char* nm = QAPI.column_name(res, (duck_idx_t)d);
        size_t n = strlen(nm), sl = 0;
        int kind = qd_companion_kind(nm, n, &sl);
        if (kind == QD_CO_NONE) continue;
        int64_t c = -1;
        for (int64_t p = 0; p < ncols && c < 0; p++) {
            const char* pn = QAPI.column_name(res, (duck_idx_t)p);
            if (p != d && strlen(pn) == n - sl && memcmp(pn, nm, n - sl) == 0) c = p;
        }
        if (c < 0) continue;   /* an orphan is a column of its own; the write door refuses it by name */
        char what[300];
        snprintf(what, sizeof what, "companion %s", nm);
        const qd_colmap_t* pcm = &cms[c];
        bool text = kind == QD_CO_RAW && q_duckdb_codec_raw_is_text(pcm->leaf->ray_type);
        int  lf   = kind < QD_CO_RAW ? RAY_BOOL : kind == QD_CO_TZOFF ? RAY_I32 : text ? RAY_STR : RAY_I64;
        if (pcm->rec || cms[d].rec) return q_duckdb_fail(slot, what, "a record's fields rebuild through no cast leg yet");
        if (!qd_co_admissible(pcm, kind)) return q_duckdb_fail(slot, what, "no column of the parent's type grows it");
        if (cms[d].leaf->ray_type != lf || cms[d].depth != pcm->depth)
            return q_duckdb_fail(slot, what, kind < QD_CO_RAW ? "not a boolean column" : kind == QD_CO_TZOFF
                                 ? "is no int column mirroring its parent" : text ? "not a text column" : "not a long column");
        for (int i = 0; i < QD_NCOS; i++)
            if (cos[d].co[i].acc) return q_duckdb_fail(slot, what, "a store companion holds NULL");
        bool fits = kind < QD_CO_RAW ? qd_mirror_fits_col(pcm, cols[c], cols[d]) : qd_leaf_fits(pcm, cols[c], cols[d], lf);
        if (!fits) return q_duckdb_fail(slot, what, kind == QD_CO_TZOFF ? "is no int column mirroring its parent"
                                                                        : "does not mirror the shape of its parent");
        ray_t** acc = &cos[c].co[kind].acc;
        ray_t*  v   = kind < QD_CO_RAW ? qd_mask_or(*acc, cols[d]) : (ray_retain(cols[d]), cols[d]);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_WSFULL);
        if (*acc) ray_release(*acc);
        *acc     = v;
        taken[d] = true;
    }
    return NULL;
}

/* ADR 5/21: a TIMESTAMPTZ is a UTC instant and stores no offset of its own, so the only offset it writes back is
 * zero — anything else is refused rather than silently dropped on the way in. */
bool q_duckdb_codec_tz_all_zero(ray_t* off) {
    if (!off || RAY_IS_ERR(off)) return true;
    if (off->type == -RAY_I32) return off->i32 == 0 || off->i32 == NULL_I32;
    if (off->type == RAY_I32) {
        for (int64_t i = 0; i < off->len; i++) {
            int32_t v = *(int32_t*)ray_vec_get(off, i);
            if (v != 0 && v != NULL_I32) return false;
        }
        return true;
    }
    if (off->type == RAY_LIST) {
        for (int64_t i = 0; i < off->len; i++) if (!q_duckdb_codec_tz_all_zero(ray_list_get(off, i))) return false;
        return true;
    }
    return true;
}

/* The strip over its three scratch arrays: each column's parent (-1 = a store column), its place in the store, and
 * whether a companion is a NESTED parent's element companion, which rides its own slot beside the shape mirror. */
static ray_t* qd_strip(int slot, ray_t* tbl, ray_t** masks, ray_t** offs, ray_t** keeps, bool* iskey, qd_enum_t* en,
                       bool bare, int64_t* parent, int64_t* at, int64_t* nest) {
    int64_t ncols = ray_table_ncols(tbl), nkeep = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        size_t n  = nm ? ray_str_len(nm) : 0, sl = 0;
        if (bare && nm && qd_reserved_col(ray_str_ptr(nm), n)) {
            char what[300];
            snprintf(what, sizeof what, "column %.*s", (int)n, ray_str_ptr(nm));
            return q_duckdb_fail(slot, what, "_q_ is reserved; a companion rides the pair (data;schema)");
        }
        int kind = nm ? qd_companion_kind(ray_str_ptr(nm), n, &sl) : QD_CO_NONE;
        parent[c] = -1;
        nest[c]   = 0;
        if (kind == QD_CO_NONE) { nkeep++; continue; }
        int64_t pid = ray_sym_intern_runtime(ray_str_ptr(nm), n - sl);
        for (int64_t p = 0; p < ncols; p++) if (ray_table_col_name(tbl, p) == pid) parent[c] = p;
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (parent[c] < 0) return qd_reject(slot, nm, "no parent column");
        /* the parent's map first: which CARRIER a raw companion wears is the parent's business (ADR 19 — a raw over
         * a timespan is DuckDB's own text, every other raw a long), so its own type check has to wait for it */
        ray_t* pcol = ray_table_get_col_idx(tbl, parent[c]);
        qd_colmap_t pcm = { NULL, NULL, 0, false };
        bool mapped = pcol && qd_map_write(slot, pcol, 0, &pcm, NULL), fits = mapped && qd_co_admissible(&pcm, kind);
        bool text = mapped && kind == QD_CO_RAW && q_duckdb_codec_raw_is_text(pcm.leaf->ray_type);
        /* ADR 12/16: a record's fields grow their own companions and the READ ships every one of them, but only
         * what the APPENDER consumes travels back — hi, raw and a zone offset are rebuilt by a CAST LEG, and no
         * leg reaches inside a record yet */
        bool prec    = mapped && q_duckdb_codec_is_rec(pcm.leaf);   /* a UNION's and a MAP's fields are deferred */
        bool legless = prec && kind >= QD_CO_RAW;
        /* a companion IS the parent's shape mirror (ADR 16), so its shape is checked against the parent itself —
         * a flat boolean over a flat column, one row-mirror per row over a nested or record one.  The isnull
         * mirror always mirrors; a value companion only where the parent NESTS or is a RECORD, a flat one being
         * the plain boolean or long column its carrier names. */
        nest[c] = mapped && (pcm.depth > 0 || prec) && kind != QD_CO_TZOFF;
        bool rows   = pcol && col && q_duckdb_codec_rec_rows(pcol) == q_duckdb_codec_rec_rows(col);
        bool mirror = kind == QD_CO_ISNULL || nest[c];
        bool shapes = !mirror || (mapped && rows && qd_mirror_fits_col(&pcm, pcol, col));
        bool shaped = mirror && col && (col->type == RAY_LIST || col->type == RAY_TABLE);
        /* the flat run-of-atoms spelling answers for a whole CONTAINER, and only the isnull mirror has anything
         * to say about one — so a nested parent's VALUE companion must carry the shape itself, never that escape */
        if (nest[c] && kind != QD_CO_ISNULL && col && col->type != RAY_LIST &&
            !(prec && col->type == RAY_TABLE)) shapes = false;
        /* a hi or raw companion carries its own carrier at the leaves, so the LEAF law judges it, not the mirror's;
         * over a NESTED parent it must carry the shape itself, since the cast leg indexes it element for element */
        bool carry = col && col->type == RAY_LIST;
        if (mapped && pcm.depth && kind >= QD_CO_RAW)
            shapes = rows && carry && qd_leaf_fits(&pcm, pcol, col, text ? RAY_STR : RAY_I64);
        if (kind == QD_CO_TZOFF) {
            shaped = true;
            shapes = mapped && rows && (!pcm.depth || carry) && qd_leaf_fits(&pcm, pcol, col, RAY_I32);
        }
        nest[c] = nest[c] && kind != QD_CO_ISNULL;   /* the mirror keeps its own slot; only the VALUE moves over */
        q_duckdb_codec_map_free(&pcm);
        if (legless) return qd_reject(slot, nm, "a record's fields rebuild through no cast leg yet");
        if (!shaped && (!col || col->type != (kind < QD_CO_RAW ? RAY_BOOL : text ? RAY_LIST : RAY_I64)))
            return qd_reject(slot, nm, kind < QD_CO_RAW ? "not a boolean column"
                                     : text ? "not a text column" : "not a long column");
        if (!rows)   return qd_reject(slot, nm, "row count differs from its parent");
        if (!mapped) return qd_reject(slot, nm, "parent has no mapping");
        if (!fits)   return qd_reject(slot, nm, "no column of the parent's type grows it");
        if (!shapes) return qd_reject(slot, nm, kind == QD_CO_TZOFF ? "is no int column mirroring its parent"
                                                                    : "does not mirror the shape of its parent");
    }
    ray_t* out = ray_table_new(nkeep);
    for (int64_t c = 0, k = 0; c < ncols && out && !RAY_IS_ERR(out); c++) {
        if (parent[c] >= 0) continue;
        at[c] = k;
        masks[k] = NULL;
        offs[k]  = NULL;
        keeps[k] = NULL;
        if (iskey) iskey[k] = iskey[c];
        if (en) en[k] = en[c];
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), ray_table_get_col_idx(tbl, c));  /* retains */
        k++;
    }
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_WSFULL);
    for (int64_t c = 0; c < ncols; c++) {
        if (parent[c] < 0) continue;
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* non-NULL: only a NAMED companion got a parent above */
        size_t  sl   = 0;
        int     kind = qd_companion_kind(ray_str_ptr(nm), ray_str_len(nm), &sl);
        ray_t** slot_of = kind == QD_CO_TZOFF ? offs : nest[c] ? keeps : masks;
        if (parent[parent[c]] >= 0)   { ray_release(out); return qd_reject(slot, nm, "parent is itself a companion"); }
        if (slot_of[at[parent[c]]])   { ray_release(out); return qd_reject(slot, nm, "parent already has a companion"); }
        slot_of[at[parent[c]]] = ray_table_get_col_idx(tbl, c);
    }
    return out;
}

/* Peel the companions off a q table (ADR 1–2/4): the store table (owned) with iskey and THREE companion arrays
 * compacted alongside, all borrowed from tbl and NULL where a column has none — masks[] the shape mirror or a flat
 * column's own companion, offs[] a zoned column's offsets (ADR 21), keeps[] a NESTED column's element companion
 * (ADR 16), which rides beside its shape mirror.  A companion is a column whose suffix its parent's carrier can
 * grow, the parent not itself one — anything else is 'duckdb, reason stashed. */
ray_t* q_duckdb_codec_strip_companions(int slot, ray_t* tbl, ray_t** masks, ray_t** offs, ray_t** keeps,
                                       bool* iskey, qd_enum_t* en, bool bare) {
    int64_t  ncols  = ray_table_ncols(tbl);
    int64_t* parent = q_duckdb_cols(3 * ncols, sizeof *parent);
    if (!parent) return q_err(QE_WSFULL);
    ray_t* out = qd_strip(slot, tbl, masks, offs, keeps, iskey, en, bare, parent, parent + ncols, parent + 2 * ncols);
    free(parent);
    return out;
}

/* WHICH companion a NESTED column's element one is, read the way a flat column's is — off its own LEAVES, the
 * name being gone by the time the cast leg asks.  The descent steps past a cell that ANSWERS FOR THE WHOLE of
 * what it stands beside (ADR 16), since such a cell says nothing about the carrier underneath. */
int q_duckdb_codec_keep_kind(const qd_colmap_t* cm, ray_t* keep) {
    ray_t* e = keep;
    for (int d = cm->depth; d > 0 && e && e->type == RAY_LIST; d--) {
        ray_t* in = NULL;
        for (int64_t i = 0; i < e->len && !in; i++) {
            ray_t* c = ray_list_get(e, i);   /* borrowed */
            if (c && (c->type == RAY_LIST || (c->type > 0 && c->type != RAY_CHARV))) in = c;
        }
        if (!in) return QD_CO_NONE;
        e = in;
    }
    const qd_colmap_t leaf = { cm->leaf, NULL, 0, false };
    return e ? q_duckdb_codec_mask_kind(&leaf, e) : QD_CO_NONE;
}

/* n copies of one atom as the vector they are */
static ray_t* qd_spread_atom(ray_t* a, int64_t n) {
    ray_t* box = ray_list_new(n ? n : 1);
    for (int64_t i = 0; i < n && box && !RAY_IS_ERR(box); i++) box = ray_list_append(box, a);
    if (!box || RAY_IS_ERR(box)) return box ? box : q_err(QE_WSFULL);
    ray_t* out = q_list_collapse(box);
    ray_release(box);
    return out;
}

/* ADR 16: an ATOM answers for the whole of what it stands beside — but the appender writes a LIST cell for a LIST
 * cell, so a companion STAGED at its parent's shape takes that atom spread over the cell it answered for.  A
 * companion already carrying the shape is handed straight back. */
static ray_t* qd_co_spread(ray_t* co, ray_t* val, int depth) {
    if (!co || !val || co->type != RAY_LIST || depth < 1) { ray_retain(co); return co; }
    ray_t* out = ray_list_new(co->len ? co->len : 1);
    for (int64_t i = 0; i < co->len && out && !RAY_IS_ERR(out); i++) {
        ray_t* c = ray_list_get(co, i);   /* borrowed */
        ray_t* v = qd_row_at(val, i);     /* owned */
        ray_t* e = c && c->type < 0 ? qd_spread_atom(c, v ? ray_len(v) : 0) : qd_co_spread(c, v, depth - 1);
        q_duckdb_drop(v);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e ? e : q_err(QE_WSFULL); }
        out = ray_list_append(out, e);
        ray_release(e);
    }
    return out ? out : q_err(QE_WSFULL);
}

/* The staging image of a write (ADR 9a): the store columns as they are, then each raw/hi long as a BIGINT column
 * under its own name for the cast leg to consume — at most 256 in all, since the strip admitted those companions
 * among the table's own 256.  The leaf's verbatim rule follows the kind — a hi parent AND its hi column keep every
 * bit (the pair (0N;0N) is NULL, judged by the cast leg), a raw parent writes its null slot as NULL.  scms/smasks
 * describe the image; the table itself, retained, when no long companion is present. */
ray_t* q_duckdb_codec_stage_image(ray_t* tbl, const qd_colmap_t* cms, ray_t* const* masks, ray_t* const* offs,
                                  ray_t* const* keeps, qd_colmap_t* scms, ray_t** smasks, ray_t** skeeps) {
    int64_t ncols = ray_table_ncols(tbl), n = ncols;
    bool any = false;
    for (int64_t c = 0; c < ncols; c++) {
        int kind = q_duckdb_codec_mask_kind(&cms[c], masks[c]);
        int vk   = q_duckdb_codec_keep_kind(&cms[c], keeps[c]);
        scms[c]   = cms[c];
        smasks[c] = kind == QD_CO_RAW ? NULL : masks[c];
        /* the image copies a nested column whole, so its element companion rides along — a RAW one excepted, as
         * a raw parent stages maskless either way: the slot it left behind is the cast leg's NULL, not a value */
        skeeps[c] = vk == QD_CO_RAW ? NULL : keeps[c];
        any = any || kind >= QD_CO_RAW || vk >= QD_CO_RAW || offs[c] != NULL;
    }
    if (!any) { ray_retain(tbl); return tbl; }
    ray_t* out = ray_table_new(4 * ncols);
    for (int64_t c = 0; c < ncols && out && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), ray_table_get_col_idx(tbl, c));   /* retains */
    for (int64_t c = 0; c < ncols && out && !RAY_IS_ERR(out); c++) {
        /* the three a column can bring: its own hi/raw count, a NESTED column's element one — the same count one
         * level in, and the cast leg's all the same — and the zone offset beside either (ADR 21) */
        for (int k = 0; k < 3 && out && !RAY_IS_ERR(out); k++) {
            int    vk   = q_duckdb_codec_keep_kind(&cms[c], keeps[c]);
            int    kind = k == 2 ? QD_CO_TZOFF : k ? vk : q_duckdb_codec_mask_kind(&cms[c], masks[c]);
            ray_t* co   = k == 2 ? offs[c] : k ? keeps[c] : masks[c];
            if (!co || (k < 2 && kind < QD_CO_RAW)) continue;
            ray_t* pn = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
            qd_buf cn = {0};
            q_duckdb_codec_companion_col(&cn, pn ? ray_str_ptr(pn) : "?", pn ? ray_str_len(pn) : 1, kind);
            if (cn.oom) { q_duckdb_buf_free(&cn); ray_release(out); return q_err(QE_WSFULL); }
            scms[n].leaf  = q_duckdb_codec_canon_leaf(k == 2 ? RAY_I32
                                : kind == QD_CO_RAW && q_duckdb_codec_raw_is_text(cms[c].leaf->ray_type) ? RAY_STR
                                                                                                        : RAY_I64);
            scms[n].rec   = NULL;
            scms[n].depth = k ? cms[c].depth : 0;   /* an element companion is staged at its parent's own shape */
            /* a hi column keeps every bit, its own column standing in for the flag that pattern would otherwise be */
            smasks[n]     = !k && kind == QD_CO_HI ? masks[c] : NULL;
            skeeps[n]     = k == 1 && kind == QD_CO_HI ? keeps[c] : NULL;
            ray_t* sc = k && cms[c].depth ? qd_co_spread(co, ray_table_get_col_idx(tbl, c), cms[c].depth) : co;
            if (!sc || RAY_IS_ERR(sc)) { q_duckdb_buf_free(&cn); ray_release(out); return sc ? sc : q_err(QE_WSFULL); }
            out = ray_table_add_col(out, ray_sym_intern_runtime(cn.p, cn.len), sc);   /* retains */
            if (sc != co) ray_release(sc);
            q_duckdb_buf_free(&cn);
            n++;
        }
    }
    return out ? out : q_err(QE_WSFULL);
}

/* ---- the record WRITE (ADR 12).  A dict-celled column IS the table its fields are, so every rule the bridge
 * already has — the companion by name, the orphan, the wrong group, the carrier map — is the table's own, one
 * level down.  Only the DECLARED type says which of the three a dict was, so the explode reads a STRUCT (or,
 * asked, a UNION) and q_duckdb_codec_map_rekind restages what the declaration names instead. ---- */

static int64_t qd_sym_id(ray_t* v, int64_t i) {
    ray_t* s = ray_sym_vec_cell(v, i);   /* borrowed */
    return s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : -1;
}

/* a record's fields are NAMES, so a cell's keys are symbols or it is no record */
static ray_t* qd_dict_syms(ray_t* cell) {
    ray_t* k = cell && cell->type == RAY_DICT ? ray_dict_keys(cell) : NULL;   /* borrowed */
    return k && k->type == RAY_SYM ? k : NULL;
}

static ray_t* qd_rec_fail(int slot, const char* why) { return q_duckdb_fail(slot, "record", why); }

/* a record column is a list of dict cells or — q having collapsed a run of like dicts — the table they are */
int64_t q_duckdb_codec_rec_rows(ray_t* col) {
    return col->type == RAY_TABLE ? ray_table_nrows(col) : ray_len(col);
}

static ray_t* qd_dict_row(ray_t* col, int64_t i) {
    if (col->type == RAY_TABLE) return q_table_row_at(col, i);
    ray_t* c = ray_list_get(col, i);
    ray_retain(c);
    return c;
}

/* Every record row under `lev` LIST levels of v, gathered into one column — the shape qd_rec_map reads fields
 * off.  An empty cell contributes no row and so casts no vote, exactly as an empty cell does at depth 0. */
static ray_t* qd_rec_flatten(ray_t* v, int lev, ray_t* acc) {
    for (int64_t i = 0; acc && !RAY_IS_ERR(acc) && i < v->len; i++) {
        ray_t* cell = ray_list_get(v, i);   /* borrowed */
        if (!cell || qd_cell_is_0n(cell)) continue;
        if (lev > 1) { acc = qd_rec_flatten(cell, lev - 1, acc); continue; }
        for (int64_t r = 0, n = q_duckdb_codec_rec_rows(cell); r < n; r++)
            acc = qd_mirror_append(acc, qd_dict_row(cell, r));   /* consumes the row either way */
    }
    return acc;
}

/* The first record cell under `lev` levels that is a TABLE, and whether every other such cell names the same
 * fields: an empty table cell still declares its fields and their types, which is all the spine below needs. */
static ray_t* qd_rec_first_table(ray_t* v, int lev, ray_t* first, bool* agree) {
    for (int64_t i = 0; *agree && i < v->len; i++) {
        ray_t* cell = ray_list_get(v, i);   /* borrowed */
        if (!cell || qd_cell_is_0n(cell)) continue;
        if (lev > 1) { first = qd_rec_first_table(cell, lev - 1, first, agree); continue; }
        if (cell->type != RAY_TABLE) continue;
        if (!first) { first = cell; continue; }
        int64_t nc = ray_table_ncols(cell);
        *agree = nc == ray_table_ncols(first);
        for (int64_t c = 0; *agree && c < nc; c++) *agree = ray_table_get_col(first, ray_table_col_name(cell, c)) != NULL;
    }
    return first;
}

/* The column of rows a record UNDER a LIST reads its fields off (owned).  When no cell carries a row the rows
 * cannot say, and a typed EMPTY table cell can — so the first one stands in, provided the cells agree. */
static ray_t* qd_rec_spine(int slot, ray_t* v, int lev) {
    ray_t* rows = qd_rec_flatten(v, lev, ray_list_new(1));
    if (!rows || RAY_IS_ERR(rows) || rows->len) return rows;
    bool   agree = true;
    ray_t* first = qd_rec_first_table(v, lev, NULL, &agree);
    if (!agree) { ray_release(rows); return qd_rec_fail(slot, "the cells do not agree on their fields"); }
    if (!first) return rows;
    ray_release(rows);
    ray_retain(first);
    return first;
}

/* the boxed values of one slot as the column it is: a run of like dicts is a nested record, never a table */
static ray_t* qd_rec_collapse(ray_t* box) {
    ray_t** e = (ray_t**)ray_data(box);
    for (int64_t i = 0; i < box->len; i++)
        if (e[i] && (e[i]->type == RAY_DICT || e[i]->type == RAY_TABLE)) { ray_retain(box); return box; }
    return q_list_collapse(box);
}

/* A dict-celled column exploded into the columns its fields are: one column per key of the template, a row
 * without that key taking the field's FIRST PRESENT value as a fill DuckDB never reads (the slot is invalid
 * there).  An EMPTY dict carries no fields, the way () carries no type.  `tag` is the live field of each row,
 * -1 where the cell is empty — which IS a NULL record; a UNION cell carries one member and, at most, its
 * companion, and there the tag is also the member DuckDB stores beside the value.  `fielded` = a declaration
 * already names the fields, so a column showing none is an empty store rather than the refusal it would be. */
/* one field of the explode: its name, the column being built for it, and the fill an absent row takes */
typedef struct { int64_t want; ray_t* box; ray_t* fill; } qd_slot_t;

static ray_t* qd_rec_explode(int slot, ray_t* col, bool uni, bool fielded, ray_t** tag) {
    int64_t    n  = q_duckdb_codec_rec_rows(col);
    qd_slot_t* sl = NULL;
    int        nw = 0, cap = 0;
    ray_t*     tv = ray_vec_new(RAY_I16, n ? n : 1);
    ray_t*     tbl = NULL, *e = NULL;
    bool       fixed = false;                               /* a STRUCT's fields are settled by its first cell */
    if (col->type == RAY_TABLE) {                           /* already exploded: q's own collapse of like rows */
        int16_t live = -1;
        for (int64_t c = 0; c < ray_table_ncols(col); c++) {
            if (live >= 0 && uni) { e = qd_rec_fail(slot, "a union cell carries one member"); break; }
            if (live < 0) live = (int16_t)c;
        }
        for (int64_t rw = 0; !e && tv && !RAY_IS_ERR(tv) && rw < n; rw++) tv = ray_vec_append(tv, &live);
        if (!e && live < 0 && !fielded) e = qd_rec_fail(slot, "no fields");
        if (!e && (!tv || RAY_IS_ERR(tv))) e = q_err(QE_WSFULL);
        if (e) { q_duckdb_drop(tv); return e; }
        if (tag) *tag = tv; else ray_release(tv);
        ray_retain(col);
        return col;
    }
    for (int64_t rw = 0; !e && rw < n; rw++) {              /* the template: which fields exist, and one value each */
        ray_t* cell = ray_list_get(col, rw);
        if (!cell || cell->type != RAY_DICT) { e = qd_rec_fail(slot, "a cell is not a dict"); break; }
        ray_t*  keys = qd_dict_syms(cell);
        ray_t*  vals = ray_dict_vals(cell);                          /* borrowed */
        int64_t kn   = ray_len(ray_dict_keys(cell));
        if (kn == 0) continue;
        if (!keys) { e = qd_rec_fail(slot, "a record's fields are names"); break; }
        if (uni && kn > 1) { e = qd_rec_fail(slot, "a union cell carries one member"); break; }
        for (int64_t j = 0; !e && j < kn; j++) {
            int64_t id = qd_sym_id(keys, j);
            int k = 0;
            while (k < nw && sl[k].want != id) k++;
            if (k == nw) {
                if (!uni && fixed) { e = qd_rec_fail(slot, "the cells do not agree on their fields"); break; }
                if (nw == cap) {
                    int        nc = cap ? 2 * cap : 8;
                    qd_slot_t* ns = realloc(sl, (size_t)nc * sizeof *ns);
                    if (!ns) { e = q_err(QE_WSFULL); break; }
                    memset(ns + cap, 0, (size_t)(nc - cap) * sizeof *ns);
                    sl = ns; cap = nc;
                }
                sl[nw++].want = id;
            }
            if (sl[k].fill) continue;
            sl[k].fill = qd_cell_at(vals, j);
            if (!sl[k].fill || RAY_IS_ERR(sl[k].fill)) { e = sl[k].fill ? sl[k].fill : q_err(QE_WSFULL); sl[k].fill = NULL; }
        }
        if (!e && !uni && kn != nw) e = qd_rec_fail(slot, "the cells do not agree on their fields");
        fixed = true;
    }
    if (!e && !nw && !fielded) e = qd_rec_fail(slot, "an all-empty record column declares no fields");
    for (int k = 0; !e && k < nw; k++)
        if (!(sl[k].box = ray_list_new(n ? n : 1)) || RAY_IS_ERR(sl[k].box)) e = q_err(QE_WSFULL);

    for (int64_t rw = 0; !e && rw < n; rw++) {                       /* the columns, absent rows filled */
        ray_t*  cell = ray_list_get(col, rw);
        ray_t*  keys = qd_dict_syms(cell);
        ray_t*  vals = cell ? ray_dict_vals(cell) : NULL;            /* borrowed */
        int64_t kn   = keys ? ray_len(keys) : 0;
        int16_t live = -1;
        for (int k = 0; !e && k < nw; k++) {
            ray_t* v = NULL;
            for (int64_t j = 0; !v && j < kn; j++) {
                if (qd_sym_id(keys, j) != sl[k].want) continue;
                v = qd_cell_at(vals, j);                             /* owned */
                if (live < 0) live = (int16_t)k;
            }
            if (!v && sl[k].fill) { v = sl[k].fill; ray_retain(v); }
            if (!v || RAY_IS_ERR(v)) { e = v ? v : q_err(QE_WSFULL); break; }
            sl[k].box = ray_list_append(sl[k].box, v);
            ray_release(v);
            if (!sl[k].box || RAY_IS_ERR(sl[k].box)) e = sl[k].box ? sl[k].box : q_err(QE_WSFULL);
        }
        if (!e && tv) { tv = ray_vec_append(tv, &live); if (!tv || RAY_IS_ERR(tv)) e = tv ? tv : q_err(QE_WSFULL); }
    }
    for (int k = 0; k < nw; k++) if (sl[k].fill) ray_release(sl[k].fill);
    if (!e && !(tbl = ray_table_new(nw ? nw : 1))) e = q_err(QE_WSFULL);
    for (int k = 0; !e && k < nw; k++) {
        ray_t* cv = qd_rec_collapse(sl[k].box);
        if (!cv || RAY_IS_ERR(cv)) { e = cv ? cv : q_err(QE_WSFULL); break; }
        tbl = ray_table_add_col(tbl, sl[k].want, cv);   /* retains */
        ray_release(cv);
        if (!tbl || RAY_IS_ERR(tbl)) e = tbl ? tbl : q_err(QE_WSFULL);
    }
    for (int k = 0; k < nw; k++) q_duckdb_drop(sl[k].box);
    free(sl);
    if (!e) { if (tag) *tag = tv; else if (tv) ray_release(tv); return tbl; }
    q_duckdb_drop(tv);
    if (tbl != e) q_duckdb_drop(tbl);
    return e;
}

/* everything a dict-celled column IS: the store columns (owned), their maps (owned, in ONE block *cms sized to
 * the exploded table — qd_rec_close) and, asked, the live field of each row.  ONE explode serves the map and the
 * write.  A field wears NO companion of its own (ADR 16): the mirror is parallel, so every key here is data. */
static ray_t* qd_rec_open(int slot, ray_t* col, bool uni, bool fielded, qd_colmap_t** cms, ray_t** tag,
                          ray_t** store) {
    if (tag) *tag = NULL;
    *cms = NULL;
    ray_t* st = qd_rec_explode(slot, col, uni, fielded, tag);
    if (RAY_IS_ERR(st)) return st;
    int64_t      nf = ray_table_ncols(st);
    qd_colmap_t* cm = q_duckdb_cols(nf, sizeof *cm);
    ray_t*       e  = cm ? q_duckdb_codec_check_table(slot, st, cm) : q_err(QE_WSFULL);
    /* undeclared, a nested record's cells must agree here or nothing can spell it; `fielded` means a declaration
     * stands above them and the merge supplies the fields the rows deferred */
    for (int64_t c = 0; !e && !fielded && c < nf; c++)
        if (q_duckdb_codec_is_rec(cm[c].leaf) && !cm[c].rec)
            e = qd_rec_fail(slot, "a nested record's cells do not agree on their fields");
    if (!e) { *store = st; *cms = cm; return NULL; }
    for (int64_t c = 0; cm && c < nf; c++) q_duckdb_codec_map_free(&cm[c]);
    if (st != e) ray_release(st);
    free(cm);
    if (tag && *tag) { ray_release(*tag); *tag = NULL; }
    return e;
}

static void qd_rec_close(int nf, qd_colmap_t* cms, ray_t* store, ray_t* tag) {
    for (int c = 0; c < nf; c++) q_duckdb_codec_map_free(&cms[c]);
    free(cms);
    if (store) ray_release(store);
    if (tag) ray_release(tag);
}

/* the record fields of an exploded store table, their maps moved in; the caller owns the result (NULL on OOM,
 * the maps moved so far freed with it, the rest still the caller's) */
static qd_rec_t* qd_rec_of_store(ray_t* store, qd_colmap_t* cms) {
    int nf = (int)ray_table_ncols(store);
    qd_rec_t* r = qd_rec_alloc(nf);
    for (int i = 0; r && i < nf; i++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(store, i));   /* borrowed */
        r->f[i].name = q_duckdb_text(nm ? ray_str_ptr(nm) : "", nm ? ray_str_len(nm) : 0);
        r->f[i].map  = cms[i];
        cms[i] = (qd_colmap_t){ NULL, NULL, 0, false };
        if (!r->f[i].name) { qd_colmap_t t = { NULL, r, 0, false }; q_duckdb_codec_map_free(&t); return NULL; }
    }
    return r;
}

/* The record a dict-celled column derives from its own data — a STRUCT, the one kind a dict spells by itself.
 * Cells that do NOT agree on their fields leave the fields DEFERRED (rec NULL): a MAP's keys and a UNION's tag
 * both vary per row, and only the declaration says which, so q_duckdb_schema_declare derives those instead.
 * A spine that could not be read (an error for `v`) is deferred the same way; the declaration pass reports it. */
static bool qd_rec_map(int slot, ray_t* v, int depth, qd_colmap_t* out) {
    qd_colmap_t* cms;
    ray_t*       store = NULL;
    size_t       mark  = q_duckdb_err_mark();
    ray_t*       e = RAY_IS_ERR(v) ? v : qd_rec_open(slot, v, false, false, &cms, NULL, &store);
    out->leaf  = qd_map_read(QDUCK_TYPE_STRUCT);
    out->rec   = NULL;
    out->depth = depth;
    if (e) { if (e != v) { ray_error_free(e); q_duckdb_err_rewind(slot, mark); } return out->leaf != NULL; }
    out->rec = qd_rec_of_store(store, cms);
    qd_rec_close((int)ray_table_ncols(store), cms, store, NULL);
    return out->leaf && out->rec;
}

/* The two flat halves of a MAP column and how many entries each row holds.  A q dict's ENTRY ORDER IS the
 * map's key order — DuckDB compares MAP{'a':1,'b':2} and MAP{'b':2,'a':1} as different values — so nothing
 * here sorts; and because DuckDB refuses a duplicate or a NULL key, a q-born dict is refused in its words. */
static ray_t* qd_map_flatten(int slot, ray_t* col, int64_t base, int64_t n,
                             ray_t** keys, ray_t** vals, ray_t** lens) {
    ray_t* kb = ray_list_new(n ? n : 1);
    ray_t* vb = ray_list_new(n ? n : 1);
    ray_t* lv = ray_vec_new(RAY_I64, n ? n : 1);
    ray_t* e  = kb && vb && lv && !RAY_IS_ERR(kb) && !RAY_IS_ERR(vb) && !RAY_IS_ERR(lv) ? NULL : q_err(QE_WSFULL);
    for (int64_t rw = base; !e && rw < base + n; rw++) {
        ray_t*  cell = qd_dict_row(col, rw);                                          /* owned */
        ray_t*  kk   = cell && cell->type == RAY_DICT ? ray_dict_keys(cell) : NULL;   /* borrowed */
        ray_t*  vv   = cell && cell->type == RAY_DICT ? ray_dict_vals(cell) : NULL;
        int64_t kn   = kk ? ray_len(kk) : -1;
        if (kn < 0) { if (cell) ray_release(cell); e = qd_rec_fail(slot, "a cell is not a dict"); break; }
        for (int64_t j = 0; !e && j < kn; j++) {
            ray_t* k = qd_cell_at(kk, j);
            ray_t* v = vv ? qd_cell_at(vv, j) : NULL;
            if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) e = q_err(QE_WSFULL);
            else if (ray_is_atom(k) && RAY_ATOM_IS_NULL(k)) e = qd_rec_fail(slot, "Map keys can not be NULL.");
            for (int64_t p = 0; !e && p < j; p++) {
                ray_t* prev = qd_cell_at(kk, p);
                if (prev && !RAY_IS_ERR(prev) && q_match_rec(prev, k)) e = qd_rec_fail(slot, "Map keys must be unique.");
                if (prev && !RAY_IS_ERR(prev)) ray_release(prev);
            }
            if (!e) {
                kb = ray_list_append(kb, k);
                vb = ray_list_append(vb, v);
                if (!kb || RAY_IS_ERR(kb) || !vb || RAY_IS_ERR(vb)) e = q_err(QE_WSFULL);
            }
            if (k && !RAY_IS_ERR(k)) ray_release(k);
            if (v && !RAY_IS_ERR(v)) ray_release(v);
        }
        if (!e) { lv = ray_vec_append(lv, &kn); if (!lv || RAY_IS_ERR(lv)) e = q_err(QE_WSFULL); }
        ray_release(cell);
    }
    ray_t* kc = e ? NULL : qd_rec_collapse(kb);
    ray_t* vc = e ? NULL : qd_rec_collapse(vb);
    if (kb && !RAY_IS_ERR(kb)) ray_release(kb);
    if (vb && !RAY_IS_ERR(vb)) ray_release(vb);
    if (e || !kc || RAY_IS_ERR(kc) || !vc || RAY_IS_ERR(vc)) {
        if (kc && !RAY_IS_ERR(kc)) ray_release(kc);
        if (vc && !RAY_IS_ERR(vc)) ray_release(vc);
        if (lv && !RAY_IS_ERR(lv)) ray_release(lv);
        return e ? e : q_err(QE_WSFULL);
    }
    *keys = kc;
    *vals = vc;
    if (lens) *lens = lv; else ray_release(lv);
    return NULL;
}

/* an accumulator handed to the caller: packed into *out, or the error it turned out to be.  Consumes `acc`. */
static ray_t* qd_co_settle(ray_t* acc, ray_t** out) {
    if (!acc) return q_err(QE_WSFULL);
    if (RAY_IS_ERR(acc)) return acc;
    ray_t* p = qd_mirror_pack(acc);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_WSFULL);
    *out = p;
    return NULL;
}

/* The entry-flat companion of a MAP's VALUES, into *out: each row's companion dict contributes its values in the
 * data's own key order, so the two halves stay entry-aligned.  A wholly-NULL row has no entries to reach.
 * Returns the error instead where one arises — a companion that could not be built is not one that is absent. */
static ray_t* qd_map_co(ray_t* mir, ray_t* lens, int64_t base, int64_t n, ray_t** out) {
    *out = NULL;
    if (!mir) return NULL;
    ray_t* acc = ray_list_new(1);
    bool box = mir->type == RAY_LIST || mir->type == RAY_TABLE;
    for (int64_t i = 0; box && acc && !RAY_IS_ERR(acc) && i < n; i++) {
        ray_t* mc = qd_dict_row(mir, base + i);
        if (RAY_IS_ERR(mc)) { ray_release(acc); return mc; }
        ray_t*  mv = mc && mc->type == RAY_DICT ? ray_dict_vals(mc) : NULL;   /* borrowed */
        int64_t l  = *(int64_t*)ray_vec_get(lens, i);
        for (int64_t j = 0; acc && !RAY_IS_ERR(acc) && j < l; j++) {
            ray_t* v = mv && j < ray_len(mv) ? qd_cell_at(mv, j) : ray_bool(false);
            acc = qd_mirror_append(acc, v);
        }
        ray_release(mc);
    }
    /* PACKED for the same reason a field's is: a run of boolean atoms is the per-entry companion a leaf reads,
     * where the list it was built as reads as "every entry keeps" */
    return qd_co_settle(acc, out);
}

/* A MAP vector IS a LIST of STRUCT(key, value): one entry window per row over the two flat halves. */
static ray_t* qd_write_map(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm, int64_t base,
                           duck_idx_t dst, int64_t n, ray_t* mir, ray_t* keep) {
    ray_t *keys = NULL, *vals = NULL, *lens = NULL;
    ray_t* e = qd_map_flatten(slot, col, base, n, &keys, &vals, &lens);
    if (e) return e;
    int64_t    tot  = ray_len(keys);
    duck_idx_t have = QAPI.list_vector_get_size(dv);   /* under a LIST every cell writes into one entry spine */
    if (QAPI.list_vector_reserve(dv, have + (duck_idx_t)tot) != QDuckSuccess ||
        QAPI.list_vector_set_size(dv, have + (duck_idx_t)tot) != QDuckSuccess)
        e = q_err(QE_DUCKDB);
    duck_list_entry* ent = e ? NULL : (duck_list_entry*)QAPI.vector_get_data(dv);
    duck_vector      kv  = e ? NULL : QAPI.list_vector_get_child(dv);
    if (!e && (!ent || !kv)) e = q_err(QE_DUCKDB);
    for (int64_t i = 0, off = have; !e && i < n; i++) {
        int64_t    l = *(int64_t*)ray_vec_get(lens, i);
        duck_idx_t r = dst + (duck_idx_t)i;
        ent[r].offset = (uint64_t)off;
        ent[r].length = (uint64_t)l;
        off += l;
        if (qd_mirror_bit(mir, base + i)) { ent[r].length = 0; qd_set_invalid(dv, r); }
    }
    ray_t *vmir = NULL, *vkp = NULL;
    if (!e && qd_co_admissible(&cm->rec->f[1].map, QD_CO_ISNULL))  e = qd_map_co(mir,  lens, base, n, &vmir);
    if (!e && qd_co_admissible(&cm->rec->f[1].map, QD_CO_NOTNULL)) e = qd_map_co(keep, lens, base, n, &vkp);
    if (!e) e = qd_write_col(slot, QAPI.struct_vector_get_child(kv, 0), keys, &cm->rec->f[0].map, 0, have, tot,
                             NULL, NULL);
    if (!e) e = qd_write_col(slot, QAPI.struct_vector_get_child(kv, 1), vals, &cm->rec->f[1].map, 0, have, tot,
                             vmir ? vmir : vkp, vmir ? vkp : NULL);
    ray_release(keys);
    ray_release(vals);
    ray_release(lens);
    ray_release(vmir);
    ray_release(vkp);
    return e;
}

/* A record that is NULL is NULL all the way down — DuckDB compares a STRUCT field by field, so a fill left
 * under a NULL parent reads back as a different row.  A NULL MAP simply has no entries to reach. */
static void qd_rec_invalidate(duck_vector dv, const qd_colmap_t* cm, duck_idx_t r) {
    qd_set_invalid(dv, r);
    if (!cm->rec || cm->depth || cm->leaf->dk_type == QDUCK_TYPE_MAP) return;
    bool uni = cm->leaf->dk_type == QDUCK_TYPE_UNION;
    if (uni) qd_set_invalid(QAPI.struct_vector_get_child(dv, 0), r);
    for (int i = 0; i < cm->rec->n; i++)
        qd_rec_invalidate(QAPI.struct_vector_get_child(dv, (duck_idx_t)(uni + i)), &cm->rec->f[i].map, r);
}

/* One field's column of sub-companions, into *out, cut out of the record's own (ADR 12/16) in the kind the
 * caller holds.  A row whose isnull mirror is the atom 1b is NULL all the way down, so the field takes that atom
 * too; every other kind has nothing to say about such a row and takes its own absent marker.  A field whose
 * carrier does not grow the kind takes nothing — a sentinel carrier needs no isnull, its null being in band. */
static ray_t* qd_rec_field_co(ray_t* co, const qd_colmap_t* fcm, const char* name, int64_t rows, int kind,
                              ray_t** out) {
    *out = NULL;
    if (!co || !qd_co_admissible(fcm, kind)) return NULL;
    int64_t id  = ray_sym_intern_runtime(name, (int64_t)strlen(name));
    ray_t*  acc = ray_list_new(rows ? rows : 1);
    for (int64_t i = 0; acc && !RAY_IS_ERR(acc) && i < rows; i++) {
        /* like-keyed companion rows collapse to a TABLE the way the value does, so the row is fetched, not borrowed */
        bool   box = co->type == RAY_LIST || co->type == RAY_TABLE;
        ray_t* mc  = box && !(kind == QD_CO_ISNULL && qd_mirror_bit(co, i)) ? qd_dict_row(co, i) : NULL;
        if (RAY_IS_ERR(mc)) { ray_release(acc); return mc; }
        ray_t* v  = NULL;
        if (mc && mc->type == RAY_DICT) {
            ray_t* k = ray_dict_keys(mc);   /* borrowed */
            for (int64_t j = 0; !v && k && j < ray_len(k); j++)
                if (qd_sym_id(k, j) == id) v = qd_cell_at(ray_dict_vals(mc), j);
        }
        ray_release(mc);
        if (!v) v = kind == QD_CO_ISNULL ? ray_bool(qd_mirror_bit(co, i)) : qd_co_absent(kind, fcm->leaf);
        acc = qd_mirror_append(acc, v);
    }
    /* PACKED, because the spelling is the meaning: a run of boolean atoms is the flat companion a leaf reads per
     * row, where the list it was built as reads as "every row keeps" (qd_write_leaf's hi rule) */
    return qd_co_settle(acc, out);
}

/* The record's rows [base, base+n) into dv: a STRUCT writes every field, a UNION its tag into physical child 0
 * and, of the members that follow, only the row's own — every other slot invalid, which is what makes the live
 * arm the only one a reader sees.  An EMPTY cell is a NULL record: the parent's companion says so too, but a
 * q-born column carries none and the row must land NULL all the same.  The FIELDS are the map's, which a
 * declaration may order differently from the explode and may name one the data never shows — NULL in every row,
 * and for a UNION a member no row is tagged with — so the tag is read back through the store's own order. */
static ray_t* qd_write_rec(int slot, duck_vector dv, ray_t* col, const qd_colmap_t* cm, int64_t base,
                           duck_idx_t dst, int64_t n, ray_t* mir, ray_t* keep) {
    if (cm->leaf->dk_type == QDUCK_TYPE_MAP) return qd_write_map(slot, dv, col, cm, base, dst, n, mir, keep);
    bool         uni = cm->leaf->dk_type == QDUCK_TYPE_UNION;
    qd_colmap_t* cms;
    ray_t*       store = NULL, *tag = NULL;
    ray_t*       e = qd_rec_open(slot, col, uni, true, &cms, &tag, &store);
    if (e) return e;
    int  nf  = (int)ray_table_ncols(store);
    int  nd  = cm->rec->n;
    int* pos = q_duckdb_cols(nd + nf, sizeof *pos);   /* each field's store column, then each store column's field */
    int* fld = pos ? pos + nd : NULL;
    if (!pos) e = q_err(QE_WSFULL);
    for (int i = 0; !e && i < nd; i++) {
        int c = 0;
        while (c < nf && strcmp(ray_str_ptr(ray_sym_str(ray_table_col_name(store, c))), cm->rec->f[i].name)) c++;
        pos[i] = c < nf ? c : -1;
        if (c < nf) fld[c] = i;
    }
    duck_vector tv = uni ? QAPI.struct_vector_get_child(dv, 0) : NULL;
    uint8_t*    t  = tv ? (uint8_t*)QAPI.vector_get_data(tv) : NULL;
    if (!e && uni && !t) e = q_err(QE_DUCKDB);
    for (int64_t i = 0; !e && t && i < n; i++) {
        int16_t live = *(int16_t*)ray_vec_get(tag, base + i);
        t[dst + (duck_idx_t)i] = (uint8_t)(live < 0 ? 0 : fld[live]);
    }
    for (int i = 0; !e && i < nd; i++) {
        duck_vector cv = QAPI.struct_vector_get_child(dv, (duck_idx_t)(uni + i));
        if (!cv) { e = q_err(QE_DUCKDB); break; }
        if (pos[i] < 0) {   /* declared, never shown: NULL in every row, and for a UNION a member with no tag */
            for (int64_t k = 0; k < n; k++) qd_rec_invalidate(cv, &cm->rec->f[i].map, dst + (duck_idx_t)k);
            continue;
        }
        /* the shape mirror where the field's carrier takes one, its own notnull where it does not: the same pair
         * qd_write_col reads for a nested column, one level in (ADR 16) */
        const qd_field_t* f = &cm->rec->f[i];
        ray_t *fm = NULL, *fk = NULL;
        e = qd_rec_field_co(mir, &f->map, f->name, ray_len(tag), QD_CO_ISNULL, &fm);
        if (!e) e = qd_rec_field_co(keep, &f->map, f->name, ray_len(tag), QD_CO_NOTNULL, &fk);
        if (!e) e = qd_write_col(slot, cv, ray_table_get_col_idx(store, pos[i]), &f->map, base, dst, n,
                                 fm ? fm : fk, fm ? fk : NULL);
        ray_release(fm);
        ray_release(fk);
        for (int64_t k = 0; !e && uni && k < n; k++)
            if (*(int16_t*)ray_vec_get(tag, base + k) != pos[i])
                qd_rec_invalidate(cv, &cm->rec->f[i].map, dst + (duck_idx_t)k);
    }
    /* a NULL record is NULL all the way down, whether its cell says so by being empty or its mirror by being 1b */
    for (int64_t i = 0; !e && i < n; i++)
        if (*(int16_t*)ray_vec_get(tag, base + i) < 0 || qd_mirror_bit(mir, base + i))
            qd_rec_invalidate(dv, cm, dst + (duck_idx_t)i);
    free(pos);
    qd_rec_close(nf, cms, store, tag);
    return e;
}

/* The shape a dict-celled column's own ROWS derive, in a given kind — all there is where nothing is declared, and
 * where what is declared is no record of this column's shape. */
static ray_t* qd_map_from_data(int slot, ray_t* col, duck_type kind, qd_colmap_t* cm) {
    if (cm->rec && cm->leaf->dk_type == kind) return NULL;
    qd_colmap_t out = { qd_map_read(kind), NULL, cm->depth, false };
    if (!out.leaf) return qd_rec_fail(slot, "the declared record type has no mapping");
    if (kind == QDUCK_TYPE_STRUCT) {
        qd_colmap_t* cms;
        ray_t*       store = NULL;
        ray_t*       e = qd_rec_open(slot, col, false, false, &cms, NULL, &store);
        if (e) return e;
        qd_rec_t* r = qd_rec_of_store(store, cms);
        qd_rec_close((int)ray_table_ncols(store), cms, store, NULL);
        if (!r) return q_err(QE_WSFULL);
        out.rec = r;
    } else if (kind == QDUCK_TYPE_MAP) {
        ray_t *keys = NULL, *vals = NULL;
        ray_t* e = qd_map_flatten(slot, col, 0, q_duckdb_codec_rec_rows(col), &keys, &vals, NULL);
        if (e) return e;
        qd_rec_t* r = qd_rec_alloc(2);
        if (r) { r->f[0].name = q_duckdb_text("key", 3); r->f[1].name = q_duckdb_text("value", 5); }
        bool ok = r && r->f[0].name && r->f[1].name &&
                  qd_map_write(slot, keys, 0, &r->f[0].map, NULL) && qd_map_write(slot, vals, 0, &r->f[1].map, NULL);
        ray_release(keys);
        ray_release(vals);
        out.rec = r;
        if (!ok) {
            q_duckdb_codec_map_free(&out);
            return qd_rec_fail(slot, "a MAP's keys and values have no mapping");
        }
    } else if (kind == QDUCK_TYPE_UNION) {
        qd_colmap_t* cms;
        ray_t*       store = NULL;
        ray_t*       e = qd_rec_open(slot, col, true, false, &cms, NULL, &store);
        if (e) return e;
        qd_rec_t* r = qd_rec_of_store(store, cms);
        qd_rec_close((int)ray_table_ncols(store), cms, store, NULL);
        if (!r) return q_err(QE_WSFULL);
        out.rec = r;
    } else return qd_rec_fail(slot, "a record declares STRUCT, MAP or UNION");
    q_duckdb_codec_map_free(cm);
    *cm = out;
    return NULL;
}

/* Every leaf of a shape DuckDB read for us, respelled as the q carrier's CANONICAL write row: a declaration names
 * the types it READS as (USMALLINT reads as an int), while the stage holds what q has and the cast leg makes up
 * the difference.  A record row is the shape itself and keeps its spelling. */
static void qd_map_stage(qd_colmap_t* cm) {
    if (cm->rec) {
        qd_rec_t* r = (qd_rec_t*)cm->rec;
        for (int i = 0; i < r->n; i++) qd_map_stage(&r->f[i].map);
        return;
    }
    const qd_tmap_t* canon = q_duckdb_codec_canon_leaf(cm->leaf->ray_type);
    if (canon) cm->leaf = canon;
}

/* The stage a DECLARED record takes: the declaration's fields, in its order, each spelled as the DATA spells it —
 * the cast leg is what turns a stage into the declared type, and only the q carrier can say how a cell is read —
 * and, for a field the data never shows, as the DECLARATION spells it, since nothing else can.  A field the cells
 * DO carry and the declaration does not name would be dropped on the way in, so it is refused instead. */
static ray_t* qd_decl_merge(int slot, ray_t* col, qd_colmap_t* dec) {
    qd_rec_t* r = (qd_rec_t*)dec->rec;
    if (dec->leaf->dk_type == QDUCK_TYPE_MAP) {
        ray_t *keys = NULL, *vals = NULL;
        ray_t* e = qd_map_flatten(slot, col, 0, q_duckdb_codec_rec_rows(col), &keys, &vals, NULL);
        if (e) return e;
        qd_colmap_t half[2] = {{ NULL, NULL, 0, false }, { NULL, NULL, 0, false }};
        bool entries = ray_len(keys) > 0;   /* no entry types the halves, and the declaration already has */
        bool ok = !entries || (qd_map_write(slot, keys, 0, &half[0], NULL) && qd_map_write(slot, vals, 0, &half[1], NULL));
        if (ok && entries)
            for (int i = 0; i < 2; i++) { q_duckdb_codec_map_free(&r->f[i].map); r->f[i].map = half[i]; }
        else q_duckdb_codec_maps_free(half, 2);
        ray_release(keys);
        ray_release(vals);
        return ok ? NULL : qd_rec_fail(slot, "a MAP's keys and values have no mapping");
    }
    qd_colmap_t* cms;
    ray_t*       store = NULL;
    ray_t*       e = qd_rec_open(slot, col, dec->leaf->dk_type == QDUCK_TYPE_UNION, true, &cms, NULL, &store);
    if (e) return e;
    int   nf   = (int)ray_table_ncols(store);
    bool* seen = q_duckdb_cols(nf, sizeof *seen);
    if (!seen) e = q_err(QE_WSFULL);
    for (int i = 0; !e && i < r->n; i++) {
        int c = 0;
        while (c < nf && strcmp(ray_str_ptr(ray_sym_str(ray_table_col_name(store, c))), r->f[i].name)) c++;
        if (c == nf) continue;   /* declared, never shown: the declaration's own spelling is all there is */
        seen[c] = true;
        /* the law is RECURSIVE: a nested record keeps the declaration's fields and takes the data's spelling of
         * the leaves under them — the one other place a row can defer a field is one level down */
        if (q_duckdb_codec_is_rec(r->f[i].map.leaf) && q_duckdb_codec_is_rec(cms[c].leaf) &&
            !r->f[i].map.depth && !cms[c].depth) {
            if ((e = qd_decl_merge(slot, ray_table_get_col_idx(store, c), &r->f[i].map))) break;
            continue;
        }
        q_duckdb_codec_map_free(&r->f[i].map);
        r->f[i].map = cms[c];
        cms[c]      = (qd_colmap_t){ NULL, NULL, 0, false };
    }
    for (int c = 0; !e && c < nf; c++)
        if (!seen[c]) e = qd_rec_fail(slot, "the cells carry a field the declared record type does not name");
    free(seen);
    qd_rec_close(nf, cms, store, NULL);
    return e;
}

/* THE stage map of a record column (ADR 12), at depth 0 the dict cells themselves and under a LIST the rows their
 * cells hold.  Where a column is DECLARED, its fields are the declaration's —
 * their names, their order and, for a field no row is live in, their types: a MAP's keys are data, a UNION's
 * members cannot be read off one cell, and a member or a field with no live row can be read off none of them,
 * which is why row inspection was never enough.  `lt` is that declaration as DuckDB itself read it; with nothing
 * declared, or a declaration DuckDB does not read as a record of this column's shape, the dict spells itself and
 * `kind` says which of the three to spell it as. */
ray_t* q_duckdb_codec_map_declared(int slot, ray_t* col, duck_logical_type lt, duck_type kind, qd_colmap_t* cm) {
    if (!q_duckdb_codec_is_rec(cm->leaf)) return NULL;
    qd_colmap_t dec  = { NULL, NULL, 0, false };
    duck_type   miss = 0;
    const char* why  = NULL;
    /* UNDER a list the rows are in the cells, and both derivations below read a column OF ROWS */
    ray_t* rows = cm->depth ? qd_rec_spine(slot, col, cm->depth) : NULL;
    if (rows && RAY_IS_ERR(rows)) return rows;
    ray_t* v = rows ? rows : col;
    ray_t* e = NULL;
    if (lt && qd_map_read_logical(lt, &dec, &miss, &why) &&
        q_duckdb_codec_is_rec(dec.leaf) && dec.depth == cm->depth) {
        qd_map_stage(&dec);
        if (!(e = qd_decl_merge(slot, v, &dec))) { q_duckdb_codec_map_free(cm); *cm = dec; }
        else q_duckdb_codec_map_free(&dec);
    } else {
        q_duckdb_codec_map_free(&dec);
        e = qd_map_from_data(slot, v, kind, cm);
    }
    ray_release(rows);
    return e;
}

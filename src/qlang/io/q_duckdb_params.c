/* q_duckdb_params — the `$name` scanner: the parameter markers of a SQL statement as DuckDB's lexer finds them
 * (docs/duckdb-api.md § Parameters), tested row by row against DuckDB's own answer (.duckdb.i.prepnames).  Text
 * only — no libduckdb — so the parser can call it on a box without the library.  Skipped as the lexer skips them:
 * '…' strings ('' escapes; E'…' adds backslash escapes), "…" identifiers, -- line comments, nested block comments,
 * and $tag$…$tag$ / $$…$$ dollar quotes; a $ glued to an identifier (a$x) is part of it.  A name folds to lowercase
 * as DuckDB folds an identifier, or keeps its spelling for the parser's capture; `?` is the positional after the
 * highest numbered so far. */
#include "qlang/io/q_duckdb.h"
#include "qlang/base/q_err.h"
#include "table/sym.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool scan_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool scan_ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

/* the index past the closer (cn bytes) sought from i; n when unterminated */
static size_t scan_to(const char* s, size_t n, size_t i, const char* close, size_t cn) {
    for (; i + cn <= n; i++) if (memcmp(s + i, close, cn) == 0) return i + cn;
    return n;
}

/* i past the opening quote q: qq escapes, and a backslash escapes too in an E'…' string */
static size_t scan_quoted(const char* s, size_t n, size_t i, char q, bool backslash) {
    for (; i < n; i++) {
        if (backslash && s[i] == '\\') i++;
        else if (s[i] == q) { if (i + 1 < n && s[i + 1] == q) i++; else return i + 1; }
    }
    return n;
}

static size_t scan_comment(const char* s, size_t n, size_t i) {           /* i at the opening slash-star */
    int depth = 0;
    for (; i + 1 < n; i++) {
        if (s[i] == '/' && s[i + 1] == '*') { depth++; i++; }
        else if (s[i] == '*' && s[i + 1] == '/') { i++; if (--depth == 0) return i + 1; }
    }
    return n;
}

static ray_t* scan_add(ray_t* v, const char* s, size_t n, bool fold) {
    char* low = malloc(n + 1);
    if (!low) { ray_release(v); return q_err(QE_WSFULL); }
    for (size_t k = 0; k < n; k++) low[k] = fold ? (char)tolower((unsigned char)s[k]) : s[k];
    int64_t id = ray_sym_intern_runtime(low, n);
    free(low);
    for (int64_t k = 0; k < ray_len(v); k++) if (*(const int64_t*)ray_vec_get(v, k) == id) return v;
    return ray_vec_append(v, &id);
}

static ray_t* params(const char* s, size_t n, bool fold) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 1);
    long positional = 0;
    for (size_t i = 0; v && !RAY_IS_ERR(v) && i < n;) {
        char c = s[i];
        bool estr = c == '\'' && i && (s[i - 1] == 'E' || s[i - 1] == 'e') && !(i > 1 && scan_ident_char(s[i - 2]));
        if (c == '\'' || c == '"') { i = scan_quoted(s, n, i + 1, c, estr); continue; }
        if (c == '-' && i + 1 < n && s[i + 1] == '-') { i = scan_to(s, n, i, "\n", 1); continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = scan_comment(s, n, i); continue; }
        if (c == '?') {
            char num[24];
            v = scan_add(v, num, (size_t)snprintf(num, sizeof num, "%ld", ++positional), fold);
            i++;
            continue;
        }
        if (c != '$' || (i && scan_ident_char(s[i - 1]))) { i++; continue; }
        size_t j = i + 1;
        if (j < n && s[j] == '$') { i = scan_to(s, n, j + 1, "$$", 2); continue; }
        if (j < n && isdigit((unsigned char)s[j])) {
            long k = strtol(s + j, NULL, 10);
            if (k > positional) positional = k;
            while (j < n && isdigit((unsigned char)s[j])) j++;
        } else if (j < n && scan_ident_start(s[j])) {
            while (j < n && scan_ident_char(s[j])) j++;
            if (j < n && s[j] == '$') { i = scan_to(s, n, j + 1, s + i, j + 1 - i); continue; }
        } else { i = j; continue; }
        v = scan_add(v, s + i + 1, j - i - 1, fold);
        i = j;
    }
    return v ? v : q_err(QE_WSFULL);
}

ray_t* q_duckdb_params(const char* s, size_t n)         { return params(s, n, true); }
ray_t* q_duckdb_params_spelled(const char* s, size_t n) { return params(s, n, false); }

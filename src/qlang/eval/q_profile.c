#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/eval/q_profile.h"
#include "qlang/eval/q_eval.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"
#include "core/profile.h"
#include "lang/env.h"
#include "lang/eval.h"
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t hits, total, self; } statement_t;

struct q_profile_lambda {
    q_profile_lambda_t* next;
    ray_t* lambda;
    int64_t name, file, line, n;
    statement_t statements[];
};

#define PROFILE_BUCKETS 256

struct q_profile {
    q_profile_lambda_t* functions[PROFILE_BUCKETS];
    q_profile_call_t* top;
    bool oom;
    int64_t excluded_ns;
};

_Thread_local q_profile_t* q_profile_active;

static q_profile_lambda_t* find_lambda(q_profile_t* p, ray_t* lambda, int64_t n) {
    size_t bucket = ((uintptr_t)lambda >> 6) % PROFILE_BUCKETS;
    for (q_profile_lambda_t* f = p->functions[bucket]; f; f = f->next)
        if (f->lambda == lambda) return f;
    if (p->oom) return NULL;
    if ((uint64_t)n > (SIZE_MAX - sizeof(q_profile_lambda_t)) / sizeof(statement_t)) {
        p->oom = true;
        return NULL;
    }
    q_profile_lambda_t* f = ray_calloc_raw(sizeof(*f) + (size_t)n * sizeof(statement_t));
    if (!f) { p->oom = true; return NULL; }
    ray_t* name = NULL;
    q_eval_apply_lambda_prov(lambda, &name, &f->file, &f->line);
    if (name && q_count(name) > 0) {
        f->name = ray_sym_intern_runtime(ray_data(name), (size_t)q_count(name));
        if (f->name < 0) { ray_free_raw(f); p->oom = true; return NULL; }
    }
    ray_retain(lambda);                 /* protects identity and source through reporting */
    f->lambda = lambda;
    f->n = n;
    f->next = p->functions[bucket];
    p->functions[bucket] = f;
    return f;
}

/* Debugger prompt time is excluded from all running statements. */
static int64_t measured_at(const q_profile_t* p) {
    return ray_profile_now_ns() - p->excluded_ns;
}

q_profile_pause_t q_profile_pause(void) {
    q_profile_t* p = q_profile_active;
    if (!p) return (q_profile_pause_t){0};
    int64_t since = ray_profile_now_ns();
    q_profile_active = NULL;
    return (q_profile_pause_t){p, since};
}

void q_profile_resume(q_profile_pause_t pause) {
    q_profile_t* p = pause.profile;
    if (!p) return;
    p->excluded_ns += ray_profile_now_ns() - pause.since;
    q_profile_active = p;
}

void q_profile_call_begin(q_profile_call_t* call, ray_t* lambda, int64_t statements) {
    q_profile_t* p = q_profile_active;
    *call = (q_profile_call_t){
        .function = find_lambda(p, lambda, statements),
        .parent = p->top,
    };
    p->top = call;
}

void q_profile_call_end(q_profile_call_t* call) {
    q_profile_active->top = call->parent;
}

void q_profile_stmt_begin(q_profile_call_t* call) {
    call->children = 0;
    call->statement_start = measured_at(q_profile_active);
}

void q_profile_stmt_end(q_profile_call_t* call, int64_t index) {
    int64_t elapsed = measured_at(q_profile_active) - call->statement_start;
    if (call->parent) call->parent->children += elapsed;
    if (!call->function) return;
    statement_t* s = &call->function->statements[index];
    s->hits++;
    s->total += elapsed;
    s->self += elapsed - call->children;
}

static void drop(ray_t* v) {
    if (v && RAY_IS_ERR(v)) ray_error_free(v);
    else ray_release(v);
}

typedef struct { const char* name; int8_t type; } column_t;
static const column_t summary_cols[] = {
    {"function", RAY_SYM}, {"file", RAY_SYM}, {"line", RAY_I64}, {"col", RAY_I64},
    {"hits", RAY_I64}, {"totalNs", RAY_I64}, {"selfNs", RAY_I64},
    {"statement", RAY_LIST},
};
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
enum { STATEMENT_COLUMN = COUNT_OF(summary_cols) - 1 };

static ray_t* new_column(int8_t type, int64_t cap) {
    if (type == RAY_LIST) return ray_list_new(cap);
    if (type == RAY_SYM) return ray_sym_vec_new(RAY_SYM_W64, cap);
    return ray_vec_new(type, cap);
}

typedef struct {
    const q_profile_lambda_t* function;
    const statement_t* stat;
    const char* text;
    int64_t len, line, col;
} summary_row_t;

static int total_desc(const void* lhs, const void* rhs) {
    const summary_row_t* a = lhs;
    const summary_row_t* b = rhs;
    return (a->stat->total < b->stat->total) - (a->stat->total > b->stat->total);
}

static void summary_rows(summary_row_t* rows, const q_profile_t* p) {
    for (size_t i = 0; i < PROFILE_BUCKETS; i++)
        for (const q_profile_lambda_t* f = p->functions[i]; f; f = f->next) {
            ray_t* src = q_eval_apply_lambda_src(f->lambda);
            const char* text = src ? ray_str_ptr(src) : "";
            int64_t line = f->line > 0 ? f->line : 1;
            int64_t col = 0;
            int64_t at = 0;
            for (int64_t j = 0; j < f->n; j++) {
                if (!f->statements[j].hits) continue;
                int64_t start = 0, end = 0, base_col = 0;
                q_eval_apply_lambda_span(f->lambda, j, &base_col, &start, &end);
                if (!at) col = base_col;
                for (; at < start; at++) {
                    if (text[at] == '\n') { line++; col = 1; }
                    else col++;
                }
                *rows++ = (summary_row_t){ f, &f->statements[j], text + start, end - start, line, col };
            }
        }
}

static summary_row_t* collect_rows(q_profile_t* p, int64_t* count) {
    int64_t n = 0;
    for (size_t i = 0; i < PROFILE_BUCKETS; i++)
        for (const q_profile_lambda_t* f = p->functions[i]; f; f = f->next)
            for (int64_t j = 0; j < f->n; j++) n += f->statements[j].hits > 0;
    *count = n;
    if (p->oom || !n) return NULL;
    if ((uint64_t)n > SIZE_MAX / sizeof(summary_row_t)) { p->oom = true; return NULL; }
    summary_row_t* rows = ray_alloc_raw((size_t)n * sizeof(*rows));
    if (!rows) { p->oom = true; return NULL; }
    summary_rows(rows, p);
    qsort(rows, (size_t)n, sizeof(*rows), total_desc);
    return rows;
}

static ray_t* make_summary(const summary_row_t* rows, int64_t n) {
    ray_t* columns[COUNT_OF(summary_cols)];
    bool ok = true;
    for (size_t i = 0; i < COUNT_OF(summary_cols); i++) {
        columns[i] = new_column(summary_cols[i].type, n);
        if (!columns[i] || RAY_IS_ERR(columns[i])) ok = false;
    }
    for (int64_t i = 0; ok && i < n; i++) {
        const summary_row_t* r = &rows[i];
        const statement_t* s = r->stat;
        const int64_t values[] = { r->function->name, r->function->file, r->line, r->col,
                                   s->hits, s->total, s->self };
        _Static_assert(COUNT_OF(values) == STATEMENT_COLUMN, "summary row does not match schema");
        for (size_t j = 0; j < COUNT_OF(values); j++) {
            columns[j] = ray_vec_append(columns[j], &values[j]);
            if (!columns[j] || RAY_IS_ERR(columns[j])) ok = false;
        }
        ray_t* text = ray_charv(r->text, r->len);
        if (text && !RAY_IS_ERR(text)) columns[STATEMENT_COLUMN] = ray_list_append(columns[STATEMENT_COLUMN], text);
        else ok = false;
        drop(text);
        if (!columns[STATEMENT_COLUMN] || RAY_IS_ERR(columns[STATEMENT_COLUMN])) ok = false;
    }
    ray_t* table = ok ? ray_table_new(COUNT_OF(summary_cols)) : NULL;
    for (size_t i = 0; table && !RAY_IS_ERR(table) && i < COUNT_OF(summary_cols); i++) {
        const char* name = summary_cols[i].name;
        int64_t id = ray_sym_intern_runtime(name, strlen(name));
        if (id < 0) {
            ray_release(table);
            table = NULL;
            break;
        }
        table = ray_table_add_col(table, id, columns[i]);
    }
    for (size_t i = 0; i < COUNT_OF(summary_cols); i++) drop(columns[i]);
    return table;
}

static void free_profile(q_profile_t* p) {
    for (size_t i = 0; i < PROFILE_BUCKETS; i++) {
        q_profile_lambda_t* f = p->functions[i];
        while (f) {
            q_profile_lambda_t* next = f->next;
            ray_release(f->lambda);
            ray_free_raw(f);
            f = next;
        }
    }
}

void q_profile_forked(void) {
    if (!q_profile_active) return;
    free_profile(q_profile_active);
    q_profile_active = NULL;
}

/* Keep the evaluated result owned by the caller. Errors propagate with that same ownership. */
static ray_t* finish(q_profile_t* p, ray_t* result) {
    q_profile_active = NULL;
    if (!result || RAY_IS_ERR(result)) {
        free_profile(p);
        return result ? result : q_err(QE_TYPE);
    }
    int64_t count;
    summary_row_t* rows = collect_rows(p, &count);
    ray_t* table = p->oom ? NULL : make_summary(rows, count);
    if (rows) ray_free_raw(rows);
    free_profile(p);
    if (table && !RAY_IS_ERR(table)) return table;
    drop(table);
    return q_err(QE_OOM);
}

static ray_t* apply_profile(ray_t* f, ray_t* args) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (q_profile_active) return q_err(QE_NYI);
    q_profile_t p = {0};
    ray_t* inputs[] = {f, args};
    q_profile_active = &p;
    ray_t* result = q_eval_dot_wrap(inputs, 2);
    ray_t* table = finish(&p, result);
    if (RAY_IS_ERR(table)) {
        ray_release(result);
        return table;
    }
    ray_t* pair = ray_list_new(2);
    pair = ray_list_append(pair, table);
    pair = ray_list_append(pair, result);
    ray_release(table);
    ray_release(result);
    return pair ? pair : q_err(QE_OOM);
}

void q_profile_pq_register(void) {
    const char* name = ".pq.i.profile";
    ray_t* f = ray_fn_binary(name, RAY_FN_NONE, apply_profile);
    q_env_bind_native(name, f, 2);
    drop(f);
}

ray_t* q_profile_expr(ray_t* ast) {
    if (q_profile_active) return q_err(QE_NYI);
    q_profile_t p = {0};
    q_profile_active = &p;
    ray_t* result = q_eval_apply_concrete(q_eval(ast));
    ray_t* table = finish(&p, result);
    ray_release(result);
    return table;
}

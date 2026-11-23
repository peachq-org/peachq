/* Invocation-scoped q statement profiling. The evaluator only calls the hooks while active. */
#ifndef Q_PROFILE_H
#define Q_PROFILE_H

#include <rayforce.h>

typedef struct q_profile q_profile_t;
typedef struct q_profile_lambda q_profile_lambda_t;
extern _Thread_local q_profile_t* q_profile_active;

typedef struct q_profile_call {
    q_profile_lambda_t* function;
    struct q_profile_call* parent;
    int64_t statement_start, children;
} q_profile_call_t;

void q_profile_call_begin(q_profile_call_t* call, ray_t* lambda, int64_t statements);
void q_profile_call_end(q_profile_call_t* call);
void q_profile_stmt_begin(q_profile_call_t* call);
void q_profile_stmt_end(q_profile_call_t* call, int64_t index);

/* A debugger suspension pauses collection and excludes prompt time. */
typedef struct { q_profile_t* profile; int64_t since; } q_profile_pause_t;
q_profile_pause_t q_profile_pause(void);
void q_profile_resume(q_profile_pause_t pause);

void q_profile_forked(void);
void q_profile_pq_register(void);
/* Profile an expression once and return the owned statement timing table. */
ray_t* q_profile_expr(ray_t* ast);

#endif

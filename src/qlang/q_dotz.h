/* q_dotz — the eval-time `.z.*` command-line resolver.
 *
 * `.z` is NOT a namespace or a `.Q`-style dict: there is no `.z` object to
 * enumerate.  Each `.z.<name>` is a name the EVALUATOR fills with a computed
 * value when a tree references it — the same way q fills `.z.p`.  This module
 * caches argv once at boot and MINTS the process-constant values on demand
 * (`.z.f` script file, `.z.x` args after the script, `.z.X` full raw argv) via
 * a resolver installed as the engine's name-load hook (ray_eval_set_name_hook);
 * it fires only on an env-resolution MISS, before `'name`.
 *
 * Lifecycle is owned by q_runtime_create / q_runtime_destroy. */
#ifndef Q_DOTZ_H
#define Q_DOTZ_H

#include <rayforce.h>

/* Cache argv and locate the `*.q` script once.  Cheap and idempotent — it
 * stores only pointers/indices (no `ray_t*`, so nothing to free on re-init);
 * value construction is deferred to q_dotz_resolve. */
void q_dotz_init(int argc, char** argv);

/* The name-load hook: given a sym_id, MINT and return an OWNED value for
 * `.z.f`/`.z.x`/`.z.X`, or NULL to decline.  Installed via
 * ray_eval_set_name_hook; requires an initialised sym table (holds at eval
 * time, well after q_dotz_init). */
ray_t* q_dotz_resolve(int64_t sym_id);

/* kdb `.z.p*` connection-handler alias -> the `.ipc.on.*` hook INDEX
 * (0=open 1=close 2=sync 3=async 4=auth 5=badmsg — `.z.bm` — matching
 * env.c's ray_sym_ipc_hook),
 * or -1 if `name` is not a handler alias.  Shared by the read path
 * (q_dotz_resolve) and the write path (q_setg_wrap in q_registry.c) so the two
 * spellings (`.z.pg` and `.ipc.on.sync`) resolve to ONE env slot. */
int q_dotz_ipc_hook_index(const char* name, size_t len);

/* `.z.ts` timer handler slot.  q_setg_wrap routes a `.z.ts:` assignment here;
 * q_dotz_resolve reads it back.  set RETAINS its own ref; passing NULL clears. */
void   q_dotz_zts_set(ray_t* fn);

/* `.z.exit` process-exit handler slot (same lifecycle as `.z.ts`).  fire runs
 * the handler with the exit code — called ONLY by q_exit (q_sys.c), the one
 * process-exit home; never from q_runtime_destroy. */
void   q_dotz_zexit_set(ray_t* fn);
void   q_dotz_exit_fire(int code);

/* `.z.ph` HTTP-GET handler slot (ref/dotz.md; same lifecycle as `.z.ts`).
 * q_http.c reads it per request via the BORROWED getter (retain across the
 * call — the handler may reassign `.z.ph` from inside itself). */
void   q_dotz_zph_set(ray_t* fn);
ray_t* q_dotz_zph(void);

/* `.z.ws`/`.z.wo`/`.z.wc` WebSocket handler slots (ref/dotz.md; `.z.ph`
 * lifecycle).  q_ws.c reads them via the BORROWED getters (retain across the
 * call — a handler may reassign its own slot from inside itself). */
void   q_dotz_zws_set(ray_t* fn);
void   q_dotz_zwo_set(ray_t* fn);
void   q_dotz_zwc_set(ray_t* fn);
ray_t* q_dotz_zws(void);
ray_t* q_dotz_zwo(void);
ray_t* q_dotz_zwc(void);

/* A fresh RAY_UNARY fn-value (rc=1) that, each time the poll timer fires it,
 * resolves the CURRENT `.z.ts` binding and calls it with a fresh LOCAL
 * timestamp (`.z.P`).  Unset `.z.ts` (or a stopped timer) → no-op.  Used by the
 * `\t N` handler as the repeating timer's callback. */
ray_t* q_dotz_timer_thunk(void);

/* The positional `*.q` startup script from argv, or NULL if none — qmain uses
 * this to decide whether to run a script.  Points into argv (process-lifetime). */
const char* q_dotz_script_path(void);

/* Whether `-q` (quiet mode, kdb .z.q) was on the command line — qmain reads
 * this to suppress the interactive startup banner.  Valid after q_dotz_init. */
bool q_dotz_quiet(void);

/* Clear the cached argv pointers (no owned values to release). */
void q_dotz_destroy(void);

#endif /* Q_DOTZ_H */

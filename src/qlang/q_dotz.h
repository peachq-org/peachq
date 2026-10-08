/* q_dotz — the eval-time `.z.*` command-line resolver.
 *
 * `.z` is NOT a namespace or a `.Q`-style dict: there is no `.z` object to
 * enumerate.  Each `.z.<name>` is a name the EVALUATOR fills with a computed
 * value when a tree references it — the same way q fills `.z.p`.  This module
 * caches argv once at boot and MINTS the process-constant values on demand
 * (`.z.f` the file, `.z.x` the arguments q does not consume, `.z.X` full raw argv) via
 * q_dotz_resolve, which the evaluator's name ladder calls only on an
 * env-resolution MISS, before `'name`.
 *
 * Lifecycle is owned by q_runtime_create / q_runtime_destroy. */
#ifndef Q_DOTZ_H
#define Q_DOTZ_H

#include <rayforce.h>

/* Cache argv and whether argv[1] is the file.  Cheap and idempotent — it
 * stores only pointers/indices (no `ray_t*`, so nothing to free on re-init);
 * value construction is deferred to q_dotz_resolve. */
void q_dotz_init(int argc, char** argv);

/* The name-load hook: given a sym_id, MINT and return an OWNED value for
 * `.z.f`/`.z.x`/`.z.X`, or NULL to decline.  Installed via
 * ray_eval_set_name_hook; requires an initialised sym table (holds at eval
 * time, well after q_dotz_init). */
ray_t* q_dotz_resolve(int64_t sym_id);

/* THE list of `.z` names `\x` may expunge (owner ruling 2026-09-12 over basics/syscmds.md \x + ref/dotz.md). */
bool q_dotz_expungeable(const char* name, size_t len);

/* The connection and message handlers a fresh process starts with (`.z.ph` is h.q's): captured once the core
 * bootstrap has run, and put back by a forked worker, which must not answer with the parent's own handlers. */
void q_dotz_doors_snapshot(void);
void q_dotz_doors_restore(void);

/* The settable `.z.*` handlers (`.z.ts`/`.z.exit`/`.z.ph`/`.z.pp`/`.z.pm`/`.z.ac`/`.z.ws`/`.z.wo`/`.z.wc` and the
 * connection six `.z.po`/`.z.pc`/`.z.pg`/`.z.ps`/`.z.pw`/`.z.bm`) are ORDINARY globals: every write form reaches
 * them through q_env_set, and each fire site resolves the current binding by name (q_env_get) so a re-assign takes
 * effect immediately.  Who FIRES each: `.z.ts` the poll timer and `.z.exit` q_sys_exit — both below — the `.z.p`
 * HTTP trio and `.z.ac` q_http.c, `.z.w*` q_ws.c, the connection six src/core/ipc.c. */
void   q_dotz_exit_fire(int code);

/* True for a `.z.*` name whose SETTING is 'nyi, so both write paths (`:`
 * assignment and `set`) refuse it instead of storing a value nothing reads. */

/* A fresh RAY_UNARY fn-value (rc=1) that, each time the poll timer fires it,
 * resolves the CURRENT `.z.ts` binding and calls it with a fresh LOCAL
 * timestamp (`.z.P`).  Unset `.z.ts` (or a stopped timer) → no-op.  Used by the
 * `\t N` handler as the repeating timer's callback. */
ray_t* q_dotz_timer_thunk(void);

/* One command-line option: its spelling, its parameter count, the `\` command the launcher applies it through (NULL:
 * none), and its one-character values where cmdline.md enumerates them (NULL: any). */
typedef struct {
    const char* name;
    int         nparam;
    const char* cmd;
    const char* values;
} q_dotz_opt_t;

/* The option `token` spells, or NULL — THE table of what q consumes from argv: the launcher applies each one, and
 * `.z.x`, `.z.q` and q_dotz_has_flag skip each with its parameters. */
const q_dotz_opt_t* q_dotz_opt(const char* token);

/* argv[1] when it is not an option, else NULL: `q [file] [-option [parameters] …]` (basics/cmdline.md). */
const char* q_dotz_file_arg(int argc, char** argv);

/* Whether `-q` (quiet mode, kdb .z.q) was on the command line — qmain reads
 * this to suppress the interactive startup banner.  Valid after q_dotz_init. */
bool q_dotz_quiet(void);

/* Whether the launcher was given `flag` as a FLAG (not as another flag's value) — the one arity-aware argv
 * probe, usable before q_dotz_init: qmain's `-conn` mode switch. */
bool q_dotz_has_flag(int argc, char** argv, const char* flag);

/* Wall clock in ns since the rayforce epoch (2000.01.01), UTC (local=0) or
 * local (local=1).  THE portable CLOCK_REALTIME home — Windows has no
 * clock_gettime, so every wall-clock reader in the q layer comes through here. */
int64_t q_dotz_now_ns(int local);

/* Seconds east of UTC in force at a UTC instant (seconds since 2000.01.01): a set `\o`, else the OS zone's rule. */
int64_t q_dotz_utc_offset_at(int64_t secs);

/* Clear the cached argv pointers and release the startup handler snapshot. */
void q_dotz_destroy(void);

#endif /* Q_DOTZ_H */

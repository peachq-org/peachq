/* q_sys — the unified q `\`-command dispatcher.  One line pre-parse feeds a
 * SWITCH on the command char, each case calling its handler with tailored
 * args; every kdb `\`-command is a case
 * (working / silent get-set / 'nyi).
 * CONTRACT (value-or-throw, 2026-07-16): a syscmd RETURNS AN OWNED q value
 * (NULL = silent) or an OWNED error — callers never branch on result kinds.
 * Exit (`\\`, `exit x` → q_sys_exit) is a function the shared path calls;
 * whether it may act on the PROCESS is the q_sys_own_process capability, OFF by
 * default and enabled only by the real `q` binary — so the one-process doctest
 * runner and the argv-less wasm REPL cannot be exited out from under by a
 * script.  Frozen-clean: no src/lang edits. */
#ifndef Q_SYS_H
#define Q_SYS_H

#include <rayforce.h>
#include <stddef.h>

/* Re-initialize the rng to kdb's constant startup seed (-314159i,
 * basics/syscmds.md \S) and record it as the last-initialized seed.  Called by
 * q_runtime_create; `\S n` re-initializes thereafter. */
void q_sys_seed_init(void);

/* Reset the Tier-2 `\`-command config (\c \C \g \o \W \e \s) AND the `\P`
 * display precision to their kdb defaults.  Called by q_runtime_create so a
 * config change in one .qcmd file (fresh-per-file runtime) never leaks into
 * the next. */
void q_sys_cfg_init(void);

/* `\d` current context — STATE lives in q_env (q_env.h), this is its command
 * surface.  reset = back to root (q_runtime lifecycle); prompt writes
 * "q)"/"q.foo)" into buf and returns its length (0 on overflow). */
void        q_sys_ctx_reset(void);
int         q_sys_prompt(char* buf, size_t cap);

/* The live `\g` gc mode (0 deferred / 1 immediate).  Consumed ONLY by the
 * q_ctx statement seam — the one place that acts on it. */
int q_sys_gc_mode(void);

/* The live `\e` error-trap mode (0 abort / 1 suspend / 2 collect+abort),
 * consumed by q_dbg's suspend gate and the q_ctx error display.  The setter
 * is the qmain tty-console default (kdb: console default is 1); every other
 * door keeps 0 so piped transcripts stay byte-stable. */
int  q_sys_err_trap_mode(void);
void q_sys_err_trap_set(int mode);

/* True iff a `\t N` timer is currently armed (interval > 0).  The `.z.ts`
 * forwarding thunk (q_dotz.c) consults it to no-op after a reentrant `\t 0`
 * that could not delete the in-flight (popped) timer. */
bool q_sys_timer_active(void);

/* Bind a kdb-protocol IPC listener on the runtime event poll and record it as
 * the live `\p` listener — the SINGLE HOME for both `\p N`/`\p 0W` (q_sys.c
 * h_p) and startup `-p`/`-p 0W` (qmain.c).  `port == 0` → OS-chosen ephemeral
 * (the `0W` path).  On success: reads the ACTUAL bound port back via
 * getsockname, drops any previous listener (kdb listens on ONE port), sets the
 * authoritative `\p` getter state (`g_listen_port`, so `system "p"` reports the
 * real port after EITHER path), then returns the bound port (>0).  On any
 * failure (no poll, bind/listen, or readback) returns 0 and leaves the previous
 * listener intact — never advertises port 0.  Silent
 * (prints nothing — full kdb port fidelity). */
uint16_t q_sys_listen(uint16_t port);

/* The AUTHORITATIVE live listening port (`\p` getter state): the bound port
 * (>0) while a listener is up, 0 after `\p 0` / when never bound.  qmain's
 * post-script server-mode decision keys off THIS (not a stale local `port`),
 * so a startup-script `\p 0` that closes the `-p` listener no longer strands
 * the process in a listener-less server loop. */
uint16_t q_sys_listen_port(void);

/* Execute a `\`-command line: OWNED value (NULL = silent) or OWNED error.
 * ONE setting for every door (owner ruling 2026-09-04: `\X` IS `system "X"`) —
 * an unknown token shells via popen and returns stdout as a list of char
 * vectors ('os on nonzero exit).  The intended caller is q_system_fn, which is
 * where q_parse routes a leading-`\` line. */
ray_t* q_sys_run(const char* line, size_t n);

/* Capability: may `\`-commands exit(3) the PROCESS (via q_sys_exit)?  OFF by
 * default and reset per runtime (q_sys_cfg_init); only qmain.c enables it.
 * NOT gated: the shell miss — one capture path for `\cmd` and `system "cmd"`
 * alike (owner ruling 2026-09-04), a computation returning data. */
void   q_sys_own_process(bool on);

/* The ONE process-exit home — `\\`, the `exit` verb, and remote `\\` all land
 * here.  Fires the user's `.z.exit` handler (unary, arg = exit code;
 * ref/dotz.md#zexit-action-on-exit), restores the console, then exit(code).
 * The handler cannot cancel or rewrite the exit (reentry exits with the
 * ORIGINAL code).  Capability off → returns silently WITHOUT firing `.z.exit`
 * (a doctest per-file runtime teardown is not a process exit). */
void   q_sys_exit(int code);

/* `.Q.ts[f;args]` / `-34!(f;args)` — Apply `.[f;args]` under the SAME time+space
 * measurement `\ts` uses; returns the 2-list ((ms;bytes); result) or the
 * propagated apply error (ref/dotq.md#ts-time-and-space). */
ray_t* q_sys_ts_apply(ray_t* f, ray_t* args);

/* The q-owned `system "…"` verb (bound by q_builtins_register as a QK_ENV row):
 * prepends `\` and passes straight through q_sys_run, so `system "X"` ≡ `\X`
 * for every command — one path, no special cases. */
ray_t* q_system_fn(ray_t* x);

#endif /* Q_SYS_H */

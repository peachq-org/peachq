/* q_sys — the unified q `\`-command dispatcher.  One line pre-parse feeds a
 * SWITCH on the command char (mirroring q_dotz.c's z_slot_ptr idiom), each case
 * calling its handler with tailored args; every kdb `\`-command is a case
 * (working / silent get-set / 'nyi).
 * CONTRACT (value-or-throw, 2026-07-16): a syscmd RETURNS AN OWNED q value
 * (NULL = silent) or an OWNED error — callers never branch on result kinds.
 * Exit (`\\`, `exit x` → q_exit) and the raw console shell (unknown `\cmd`)
 * are functions the shared path calls; whether they may act on the PROCESS is
 * the q_sys_own_process capability, OFF by default and enabled only by the
 * real `q` binary — so the one-process doctest runner and the argv-less wasm
 * REPL are safe with no caller-side policy.  Frozen-clean: no src/lang edits. */
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

/* `\c` console DISPLAY clipping (read by q_fmt.c's console emitter,
 * q_fmt_console).  q_con_display fills the rows/cols out-params with the live
 * `\c` size and returns true iff clipping is armed (ON by default;
 * q_sys_cfg_init).  q_con_display_disable turns it OFF — qmain calls it for a
 * pure non-tty script load so `.z.f`/script output is not clipped (a batch
 * context, not a display). */
bool q_con_display(int32_t* rows, int32_t* cols);
void q_con_display_disable(void);

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
 * real port after EITHER path) and marks the listener active, then returns the
 * bound port (>0).  On any failure (no poll, bind/listen, or readback) returns
 * 0 and leaves the previous listener intact — never advertises port 0.  Silent
 * (prints nothing — full kdb port fidelity). */
uint16_t q_sys_listen(uint16_t port);

/* The AUTHORITATIVE live listening port (`\p` getter state): the bound port
 * (>0) while a listener is up, 0 after `\p 0` / when never bound.  qmain's
 * post-script server-mode decision keys off THIS (not a stale local `port`),
 * so a startup-script `\p 0` that closes the `-p` listener no longer strands
 * the process in a listener-less server loop. */
uint16_t q_sys_listen_port(void);

/* The ONE caller guard: is this console line a `\`-command (first non-blank
 * char is `\`)?  Callers check this once, then get a value or an error. */
bool   q_sys_is_cmd(const char* line, size_t n);

/* Execute a `\`-command line: OWNED value (NULL = silent) or OWNED error.
 * capture=1 is the `system "…"` form — an unknown token shells via popen and
 * returns stdout as a list of char vectors ('os on nonzero exit); capture=0 is
 * the console form — an unknown token runs raw system(3) returning its status
 * as a long, gated by the process capability (off → silent no-op). */
ray_t* q_sys_run(const char* line, size_t n, int capture);

/* THE shared console glue (REPL / wasm / doctest): q_sys_run(capture=0), then
 * fill buf with the display text — drained console side effects first, then
 * (print_value) the value via q_fmt_console.  Returns NULL or the OWNED error;
 * buf then holds any console text that preceded it. */
ray_t* q_sys_line(const char* line, size_t n, int print_value,
                  char* buf, size_t cap);

/* Capability: may `\`-commands act on the PROCESS (exit(3) via q_exit; raw
 * console shell on an unknown `\cmd`)?  OFF by default and reset per runtime
 * (q_sys_cfg_init); only qmain.c enables it.  NOT gated: the `system "…"`
 * capture shell — a computation returning data, relied on by the doctest
 * corpus. */
void   q_sys_own_process(bool on);

/* The ONE process-exit home — `\\`, the `exit` verb, and remote `\\` all land
 * here.  Fires the user's `.z.exit` handler (unary, arg = exit code;
 * ref/dotz.md#zexit-action-on-exit), restores the console, then exit(code).
 * The handler cannot cancel or rewrite the exit (reentry exits with the
 * ORIGINAL code).  Capability off → returns silently WITHOUT firing `.z.exit`
 * (a doctest per-file runtime teardown is not a process exit). */
void   q_exit(int code);

/* The q-owned `system "…"` verb (bound by q_builtins_register as a QK_ENV row):
 * prepends `\` and passes straight through q_sys_run(capture=1), so
 * `system "X"` ≡ `\X` for every command — one path, no special cases. */
ray_t* q_system_fn(ray_t* x);

#endif /* Q_SYS_H */

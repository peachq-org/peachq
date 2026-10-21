# peachq WebAssembly build

The peachq engine compiled to WebAssembly with emscripten, run in a **Web Worker** and driven by pages through a
small client library. The build output, `build/wasm/www/`, is a directory any static server can serve.

## What's here

| File                 | Purpose |
| -------------------- | ------- |
| `peachq-client.js`   | **The page API.** Starts the Worker, evaluates lines, layers files, restarts. Pages use only this. |
| `worker.js`          | The Worker: hosts the engine, answers the client, supplies HTTP by synchronous XHR. |
| `engine.js`          | Boots the module, owns its C ABI, mounts file manifests lazily. Shared by the Worker and the node smoke. |
| `index.html`         | The minimal reference page, built on the client API. |
| `q_wasm.c`           | The C ABI: `q_wasm_init`, `q_wasm_eval` (text through `q_ctx_eval_src`, the `value`/IPC text function, answered on stdout/stderr as the console displays it), `q_wasm_qcmd` (a `.qcmd` replayed through the transcript door, scored by the host). |
| `q_wasm_http.c`      | The wasm side of the HTTP exchange seam: calls the host's `Module.peachqFetch`, rebuilds the raw response. |
| `DUCKDB_FROZEN`      | While present the build ships WITHOUT DuckDB (below) and `wasm-duckdb-suites` SKIPS loudly. Delete it with a shim rebuilt on a duckdb-wasm build that exports the v2 C API. |
| `duckdb-wasm.pin`    | The DuckDB browser build `wasm-duckdb-suites` fetches: version, and every file's sha256 and upstream URL. |
| `q_wasm_shell.c`     | The `system` mini-shell over the in-memory FS: `pwd ls cat echo mkdir rm head tail`. |
| `q_wasm_buf.h`       | The growable buffer both host adapters build their answers in. |
| `ipc_stub.c`         | Inert stubs for the `ray_ipc_*` symbols retained TUs reference — a tab has no sockets. |
| `gen-files.sh`       | Stages `examples/q/` (minus `termbox/`) as `files/` and writes `files.json`. |
| `node/NN-*.qcmd`     | The node smoke's ledgers: plain q (basics, regex, the mini-shell, the examples), then HTTP against the fixture server. |
| `smoke.js`           | The headless node check: the fixture server, then `node/*.qcmd` in order in one session inside the module. |
| `browser/NN-*.qcmd`  | The browser smoke's ledgers: files, `\l`, HTTP, DuckDB's refusal. |
| `browser-smoke.cjs`  | The headless Chromium check through the Worker: `browser/*.qcmd` in order in one session, and between them what q cannot say — the network log, the page input, responsiveness, addFiles, restart. |
| `duckdb-suites.cjs`  | `test/q/duckdb` replayed in the browser, suite by suite — a report before a release, not a gate. |
| `serve.cjs`          | The browser checks' server: `server.py` over the build plus the repo's `test/`, same-origin. |
| `server.py`          | Stdlib preview server with the `Range`/206 support lazy files need; `/duckdb/` is `build/duckdb-wasm/`. |

The build wiring lives in `../Makefile.wasm`.

## The client API

```html
<script src="wasm/peachq-client.js"></script>
<script>
  const q = await PeachQ.start({ base: 'wasm/', onStatus: (s) => console.log(s) });
  const { out, err } = await q.eval('til 5');         // stdout text, and stderr ('' unless an error was shown)
  await q.addFiles('my/files.json', 'my/files/');     // a manifest (or its URL) + where its files are served
  await q.restart();                                  // a fresh engine — also the way to stop a runaway query
</script>
```

- `start({ base, files, onStatus })` — `base` is the directory holding `worker.js` (default `./`); `files: false`
  skips mounting the build's own `files.json`; `onStatus` receives `loading` / `ready` / `busy` / `failed`.
- `eval(src)` — q text, read as `value` and an IPC request read it, in one console session. Resolves `{ out, err }`:
  the answer as the console displays it (nothing for an assignment), and the error display. Rejects only if the
  engine died. The web has only this door, never the console's one-line door.
- **An editor's Run sends the WHOLE selection in ONE `eval` call** — never split it into lines: a multi-line lambda
  is one statement only when its lines arrive together. The page draws its own `q)` echo; `eval` prints none.
- **`\d` sticks for the session**, as at the console (an IPC request would undo it). An error shows on `err` and
  the next `eval` runs normally: nothing suspends into `q))` in the browser.
- `addFiles(manifest, baseUrl)` — `manifest` is `[{ path, size?, sha256? }]` or the URL of one. Each file is
  mounted under the start directory (`/home/q`) and fetched from `baseUrl + path` (plus `?v=<sha256>`) when q
  first reads it; a later manifest replaces an earlier file. Layers survive `restart()`.
- `restart()` / `terminate()` — end the Worker; pending calls reject.

A session that hits a fatal engine error (an emscripten abort, e.g. a lazy file its server refused) goes to
`failed`; `restart()` brings it back.

## What works in a tab

- **Files**: the in-memory FS — `\l`, `read0`, `0:`, `set`/`get`, `\cd`. The session starts in `/home/q`, where the
  examples are: `\ls`, then `\l trades.q`.
- **HTTP**: `.Q.hg`, `.Q.hp`, `read0`/`read1` of `http(s)://` (ranged reads send `Range:`), and the raw handle
  `` (`:http://host) "GET / HTTP/1.1\r\n\r\n" `` all go through the browser. CORS decides what is reachable; the
  browser follows redirects, decodes gzip and drops the headers a page may not set.
- **`system` / `\cmd`**: the mini-shell above (`ls [path…]`, `head`/`tail [-n N | -N] file`, `mkdir [-p]`,
  `rm [-rf]`); no pipes, quoting, globs or redirection. Anything else answers `'os`, as a failing shell would.
- Not here: DuckDB (below), Kafka and `.crypto` (no shared library to load: `.kafka` answers its library-not-found
  error, `.crypto` `'nyi`), IPC handles, the terminal games (`examples/q/termbox/`), Ctrl-C (use `restart()`).

## DuckDB in the browser — not in this build

The desktop bridge drives DuckDB's v2 C API (`duckdb_v2_*`), and no duckdb-wasm build exports it yet, so the browser
build ships without DuckDB while `DUCKDB_FROZEN` exists. Every DuckDB door — `.duckdb.*`, `s)`, `:pq:duckdb:`
handles, `select from` a parquet file or URL — answers `'duckdb`, and `.duckdb.err[]` says
`"bridge: DuckDB is not in the browser build until duckdb-wasm ships a 2.0 bundle"`. That is the bridge's own
library-not-loaded refusal (`q_duckdb.c`'s loader, under `__EMSCRIPTEN__`), not a second error channel, and nothing is
fetched from `/duckdb/`. The v0.88 build carried DuckDB over the v1 API; that shim and its loader were deleted with this build.

To bring DuckDB back: a duckdb-wasm build that exports `duckdb_v2_*`, then a shim binding the bridge's v2 table to it
and a Worker loader, the browser smoke's DuckDB ledger turned back into a working session, and the marker deleted.

## Build and check

Prerequisites: [emscripten](https://emscripten.org) (`. ~/emsdk/emsdk_env.sh` adds `emcc` and a modern node).

```sh
make -f Makefile.wasm wasm                 # -> build/wasm/www/
make -f Makefile.wasm wasm-smoke           # node: build, then the headless check
make -f Makefile.wasm wasm-browser-smoke PLAYWRIGHT=/path/to/node_modules   # Chromium; skips without Playwright
make -f Makefile.wasm wasm-duckdb-suites PLAYWRIGHT=/path/to/node_modules   # test/q/duckdb in the browser; SKIPS while frozen
make -f Makefile.wasm duckdb-wasm          # DuckDB's browser build, checked against the pin, into build/duckdb-wasm/
python3 wasm/server.py                     # then open http://localhost:8000
```

Both smokes replay their `.qcmd` folder inside the wasm module through the transcript door the native gate
uses (`q_wasm_qcmd`, scored in `engine.js` by the row rule of `tools/qcmd/run.sh`), in file order, in one
session, after binding `ORIGIN` to their server's `http://host:port` — so a
row spells a URL `ORIGIN,"/files.json"`. They are wasm-only ledgers — `/home/q`, the lazily mounted examples, the
pipe display, a live server — and deliberately live outside `test/q`, where the native gate would find them. A `\l`
row shows what the load printed, not the statements it ran, so each load is followed by rows that pin what it built.

`smoke.js` runs under the emsdk node (`$EMSDK_NODE`; override with `NODE=`). The browser check needs Playwright
and its Chromium from the environment — this repo installs no packages.

## Notes

- **Native build unaffected.** The three `src/` seams (`q_http_client.c`'s exchange, `q_sys.c`'s shell capture,
  `q_duckdb.c`'s library load) are `#if defined(__EMSCRIPTEN__)` branches; the first two call into this directory, the
  third refuses.
- **No libffi, no ipc.c.** The FFI config headers are per native target and a tab has nothing to dlopen;
  `src/core/ipc.c` does not compile under the wasm sysroot — `ipc_stub.c` satisfies the linker.
- **Two languages.** RE2 and fmt are C++: C and C++ compile separately and the link is `em++`.
- **Single-threaded**, with an 8 MB stack (a native thread's size; emscripten's 64 KB default overflows in the
  sort and display paths).
- **No `-ffinite-math-only`**: the engine encodes float nulls as NaN.

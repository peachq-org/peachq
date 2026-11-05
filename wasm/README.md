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
| `q_wasm_duckdb.c`    | The DuckDB v2 C-API table (`duck_api_t`) bound across to DuckDB's own wasm module, which the host loads on first use: handles pass as 32-bit values, strings and buffers are copied across, the replacement-scan callback comes back through a trampoline. |
| `duck-loader.js`     | Loads DuckDB's published browser build synchronously in the Worker, mounts q's filesystem into its, and gives its v2 instances the bundle's extension provider. |
| `duckdb-wasm.pin`    | The DuckDB browser build a peachq build loads: version, and every file's sha256 and upstream URL. |
| `q_wasm_shell.c`     | The `system` mini-shell over the in-memory FS: `pwd ls cat echo mkdir rm head tail`. |
| `q_wasm_buf.h`       | The growable buffer both host adapters build their answers in. |
| `ipc_stub.c`         | Inert stubs for the `ray_ipc_*` symbols retained TUs reference — a tab has no sockets. |
| `gen-files.sh`       | Stages `examples/q/` (minus `termbox/`) as `files/` and writes `files.json`. |
| `node/NN-*.qcmd`     | The node smoke's ledgers: plain q (basics, regex, the mini-shell, the examples), then HTTP against the fixture server. |
| `smoke.js`           | The headless node check: the fixture server, then `node/*.qcmd` in order in one session inside the module. |
| `browser/NN-*.qcmd`  | The browser smoke's ledgers: files, `\l`, HTTP, DuckDB. |
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
- **DuckDB**: `.duckdb.*`, `s)`, `:pq:duckdb:` handles and `select from` a parquet file or URL — the desktop bridge,
  over DuckDB's own browser build (below).
- Not here: Kafka and `.crypto` (no shared library to load: `.kafka` answers its library-not-found error, `.crypto`
  `'nyi`), IPC handles, the terminal games (`examples/q/termbox/`), Ctrl-C (use `restart()`).

## DuckDB in the browser

The first DuckDB call loads DuckDB's published `duckdb-base.wasm` (@duckdb/duckdb-wasm, pinned in `duckdb-wasm.pin`)
into the Worker, synchronously and inside that call: nothing is fetched before it, and the session carries on
without a restart. The files come from `../duckdb/<version>/` beside the Worker — `/wasm/duckdb/<version>/` on the
site, published by hand, versioned and immutable, by `tools/duckdb-wasm-publish.sh` — and DuckDB's extensions
(parquet, json, httpfs) autoload from `extensions/` there, never from DuckDB's own servers. The bridge drives the
same v2 C API as on the desktop, on that module's exports (`q_wasm_duckdb.c`); main is an instance it opens itself:
local paths read and write q's filesystem (mounted into DuckDB's emscripten FS), `http(s)://` and `s3://` go through
httpfs over the XMLHttpRequest client the bundle registers for every database.

The loader (`duck-loader.js`) reaches the bundle at three anchors, each failing loudly when a new bundle moves it:
the bindings class `createDuckDB` constructs, the emscripten factory's run (made synchronous), and the bundle's own
exported symbols that install its extension provider — DuckDB-wasm gives extension loading to its WebDB alone, so an
instance opened over the v2 API gets the same provider through DuckDB's static-capability door
(`duckdb_static_extension.h`), as the bundle's HTTP client does.

The pin is a duckdb-wasm development build (`1.33.1-dev114.0`, the npm `next` tag) because it is the first to embed
DuckDB v2.0.0 and export the v2 API; it moves to a stable duckdb-wasm in one small PR when one ships.

Divergences from the desktop (`make -f Makefile.wasm wasm-duckdb-suites` fails on any other count):

- **Single-threaded.** The build has no threads, so `threads` above 1 is refused (`handles`, `main`: 3 rows each).
  Everything else in `test/q/duckdb` answers as the desktop does, `version()` included (both DuckDB v2.0.0).

## Build and check

Prerequisites: [emscripten](https://emscripten.org) (`. ~/emsdk/emsdk_env.sh` adds `emcc` and a modern node).

```sh
make -f Makefile.wasm wasm                 # -> build/wasm/www/
make -f Makefile.wasm wasm-smoke           # node: build, then the headless check
make -f Makefile.wasm wasm-browser-smoke PLAYWRIGHT=/path/to/node_modules   # Chromium; skips without Playwright
make -f Makefile.wasm wasm-duckdb-suites PLAYWRIGHT=/path/to/node_modules   # test/q/duckdb in the browser (report)
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
  `q_duckdb.c`'s library load) are `#if defined(__EMSCRIPTEN__)` branches that call into this directory.
- **No libffi, no ipc.c.** The FFI config headers are per native target and a tab has nothing to dlopen;
  `src/core/ipc.c` does not compile under the wasm sysroot — `ipc_stub.c` satisfies the linker.
- **Two languages.** RE2 and fmt are C++: C and C++ compile separately and the link is `em++`.
- **Single-threaded**, with an 8 MB stack (a native thread's size; emscripten's 64 KB default overflows in the
  sort and display paths).
- **No `-ffinite-math-only`**: the engine encodes float nulls as NaN.

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
| `q_wasm.c`           | The C ABI: `q_wasm_init`, `q_wasm_eval` (one line through `q_ctx_run_line`, answered on stdout/stderr), `q_wasm_qdoc` (a `.qcmd` through the qdoc runner). |
| `q_wasm_http.c`      | The wasm side of the HTTP exchange seam: calls the host's `Module.peachqFetch`, rebuilds the raw response. |
| `q_wasm_shell.c`     | The `system` mini-shell over the in-memory FS: `pwd ls cat echo mkdir rm head tail`. |
| `q_wasm_buf.h`       | The growable buffer both host adapters build their answers in. |
| `ipc_stub.c`         | Inert stubs for the `ray_ipc_*` symbols retained TUs reference — a tab has no sockets. |
| `gen-files.sh`       | Stages `examples/q/` (minus `termbox/`) as `files/` and writes `files.json`. |
| `smoke.qcmd`         | The wasm ledger: plain-q rows (basics, regex, the mini-shell, the examples) in the wasm REPL's display. |
| `smoke.js`           | The headless node check: runs `smoke.qcmd` inside the module, then the rows q text cannot express (HTTP against a fixture server, byte checks, the Range header). |
| `browser-smoke.cjs`  | The headless Chromium check through the Worker: lazy loading, XHR, responsiveness, restart. |
| `server.py`          | Stdlib preview server with the `Range`/206 support lazy files need. |

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
- `eval(line)` — one line of q, as typed at `q)`. Resolves `{ out, err }`; rejects only if the engine died.
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
- Not here: IPC handles, DuckDB, the terminal games (`examples/q/termbox/`), Ctrl-C (use `restart()`).

## Build and check

Prerequisites: [emscripten](https://emscripten.org) (`. ~/emsdk/emsdk_env.sh` adds `emcc` and a modern node).

```sh
make -f Makefile.wasm wasm                 # -> build/wasm/www/
make -f Makefile.wasm wasm-smoke           # node: build, then the headless check
make -f Makefile.wasm wasm-browser-smoke PLAYWRIGHT=/path/to/node_modules   # Chromium; skips without Playwright
python3 wasm/server.py                     # then open http://localhost:8000
```

`smoke.qcmd` runs inside the wasm module through the same qdoc runner as `./qdoctest` (`q_wasm_qdoc`), so its
rows are ordinary `.qcmd` rows. It is a wasm-only ledger — `/home/q`, the lazily mounted examples and the pipe
display — and deliberately lives outside `test/q`, where the native gate would find it. A `\l` row's echo is not
captured by the runner, so each example load is followed by rows that pin what it built.

`smoke.js` runs under the emsdk node (`$EMSDK_NODE`; override with `NODE=`). The browser check needs Playwright
and its Chromium from the environment — this repo installs no packages.

## Notes

- **Native build unaffected.** The two `src/` seams (`q_http_client.c`'s exchange, `q_sys.c`'s shell capture) are
  `#if defined(__EMSCRIPTEN__)` branches that call into this directory.
- **No libffi, no ipc.c.** The FFI config headers are per native target and a tab has nothing to dlopen;
  `src/core/ipc.c` does not compile under the wasm sysroot — `ipc_stub.c` satisfies the linker.
- **Two languages.** RE2 and fmt are C++: C and C++ compile separately and the link is `em++`.
- **Single-threaded**, with an 8 MB stack (a native thread's size; emscripten's 64 KB default overflows in the
  sort and display paths).
- **No `-ffinite-math-only`**: the engine encodes float nulls as NaN.

# openq WebAssembly build

A browser-runnable q REPL: the openq engine (parser + q datatypes over the
rayforce array core) compiled to WebAssembly with emscripten, driven by a small
self-contained web page. Mirrors upstream rayforce's `wasm.rayforcedb.com` demo,
retargeted to this `src/`-based tree.

## What's here

| File          | Purpose                                                            |
| ------------- | ------------------------------------------------------------------ |
| `q_wasm.c`    | The browser C ABI. Drives openq's real pipeline (`q_parse` → `q_lower` → `ray_eval` → materialize → `q_fmt`). |
| `ipc_stub.c`  | Inert stubs for the handful of `ray_ipc_*` symbols retained TUs reference — a browser tab has no sockets. |
| `index.html`  | Self-contained REPL page. Loads `peachq.js`, `ccall`s the ABI.   |
| `server.py`   | Stdlib preview server (correct `application/wasm` MIME).           |
| `peachq.js` / `peachq.wasm` | Build artifacts (generated; git-ignored).       |

The build wiring lives in `../Makefile.wasm` (a **separate** file — the root
`Makefile` is a frozen-base file, so it is deliberately left untouched).

## Exported ABI

Three stable C entry points (see `q_wasm.c`), plus `malloc`/`free`:

```c
int   q_wasm_init(void);          /* create the q runtime once (idempotent) */
char* q_wasm_eval(const char* q);  /* eval one q line -> malloc'd formatted result */
void  q_wasm_free(char* p);        /* free a q_wasm_eval result */
```

`q_wasm_eval` never returns NULL: errors come back as `"parse error"` or
`"error: <code>"` strings so the JS side always has something to print.

## Build

Prerequisites: [emscripten](https://emscripten.org). Install + activate:

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
. ~/emsdk/emsdk_env.sh          # adds emcc + a modern bundled node to PATH
```

Then, from the repo root:

```sh
make -f Makefile.wasm wasm      # -> wasm/peachq.js + wasm/peachq.wasm
```

## Run

```sh
python3 wasm/server.py          # then open http://localhost:8000
```

Type a q expression and press Enter — e.g. `2+3` → `5`, `til 5` → `0 1 2 3 4`,
`sum 1 2 3 4` → `10`.

### Headless check

The emsdk-bundled node (v18+; the emscripten JS uses optional chaining that
Node 12 can't parse) can drive the ABI directly:

```sh
node -e '
require("./wasm/peachq.js")().then(M => {
  M.ccall("q_wasm_init","number",[],[]);
  const p = M.ccall("q_wasm_eval","number",["string"],["2+3"]);
  console.log("2+3 =>", M.UTF8ToString(p));
  M.ccall("q_wasm_free",null,["number"],[p]);
});'
# 2+3 => 5
```

## Notes / constraints

- **Native build unaffected.** This target compiles the same `src/*/*.c`
  library sources with `emcc` instead of `clang`; it does not touch the root
  `Makefile`, the frozen manifest, or any source file.
- **No networking.** `src/core/ipc.c` (TCP server/client, `select`/`fd_set`) is
  excluded from the WASM source set — it has no meaning in a browser and does
  not compile under emscripten's sysroot. `ipc_stub.c` satisfies the linker.
- **Single-threaded.** WASM is single-threaded by construction; the build forces
  `RAYFORCE_CORES=0` so the pool never tries to spawn workers.
- **Fast-math.** The demo uses `-msimd128` + the vectorization-enabling
  fast-math flags, but **not** `-ffinite-math-only` (which upstream's target had):
  this build compiles the same engine, and that engine encodes F64/F32 nulls as
  NaN (`x != x` checks). Assuming no-NaN would mis-evaluate float nulls (`0Nf`)
  in the browser, so it is dropped to match the native build's correctness.

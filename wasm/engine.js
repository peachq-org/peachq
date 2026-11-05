/* The engine side of the wasm REPL, shared by the Worker (worker.js) and the node smoke
 * (smoke.js): boots the emscripten module, owns its C ABI (q_wasm.c) and mounts file
 * manifests.  No DOM and no messaging — the host supplies the factory and the fetch. */
(function (root) {
    'use strict';

    const HOME = '/home/q';
    const PROMPT = /^q(\.[A-Za-z][A-Za-z0-9_]*)?\)/;

    function checkPath(path) {
        if (typeof path !== 'string' || /[\\\x00-\x1f]/.test(path) ||
            !path.split('/').every((p) => p && p !== '.' && p !== '..')) {
            throw new Error('invalid manifest path: ' + JSON.stringify(path));
        }
    }

    /* factory: createPeachQ.  fetch: the Module.peachqFetch hook (q_wasm_http.c).
     * locateFile: where peachq.wasm lives.  duckLoad: the Module.peachqDuckLoad hook (q_wasm_duckdb.c),
     * absent where DuckDB is not offered. */
    async function boot({ factory, fetch, locateFile, duckLoad }) {
        let out = [], err = [], seq = [];
        const M = await factory({
            locateFile,
            print: (s) => { out.push(s); seq.push(s); },
            printErr: (s) => { err.push(s); seq.push(s); },
            peachqFetch: fetch,
            peachqDuckLoad: duckLoad,
        });
        M.FS.mkdirTree(HOME);
        M.FS.chdir(HOME);
        if (M.ccall('q_wasm_init', 'number', [], []) !== 0) throw new Error('q runtime init failed');
        out = []; err = [];
        const enc = new TextEncoder();

        return {
            home: HOME,

            /* q text, as `value` reads it, in this session: what it printed to stdout and stderr ('' when clean). */
            eval(src) {
                const bytes = enc.encode(String(src));
                const p = M._malloc(bytes.length + 1);
                M.HEAPU8.set(bytes, p);
                M.HEAPU8[p + bytes.length] = 0;
                out = []; err = []; seq = [];
                try {
                    M._q_wasm_eval(p);
                } finally {
                    M._free(p);
                }
                return { out: out.join('\n'), err: err.join('\n') };
            },

            /* A .qcmd transcript replayed in this session (`q f.qcmd`) and scored by the row rule of
             * tools/qcmd/run.sh, stdout and stderr read as one stream.  Answers the failing-row count
             * (-1 when no row ran) and a report of the failures. */
            qcmd(text) {
                const path = '/tmp/ledger.qcmd';
                M.FS.writeFile(path, String(text));
                out = []; err = []; seq = [];
                M.ccall('q_wasm_qcmd', 'number', ['string'], [path]);
                const want = [], got = [];
                for (const l of String(text).replace(/\r/g, '').split('\n')) {
                    if (PROMPT.test(l)) want.push({ line: l, block: [] });
                    else if (want.length && !/^\/[ \t]/.test(l)) want[want.length - 1].block.push(l);
                }
                for (const s of seq) {
                    if (PROMPT.test(s)) got.push({ line: s, block: [] });
                    else if (got.length) got[got.length - 1].block.push(s);
                }
                const trim = (b) => b.map((l) => l.replace(/[ \t\r]+$/, '')).join('\n').replace(/^[ \t\n]+|[ \t\n]+$/g, '');
                const report = [];
                want.forEach((w, i) => {
                    const g = got[i], exp = trim(w.block);
                    let ok, act;
                    if (!g) { ok = false; act = '(no replay row)'; }
                    else if (g.line !== w.line) { ok = false; act = 'prompt: ' + g.line; }
                    else if (exp.startsWith("'")) {
                        const cls = exp.split('\n')[0].replace(/[ \t]+$/, '').slice(1);
                        const e = (g.block.find((l) => l.startsWith("'")) || '').replace(/[ \t\r]+$/, '');
                        act = e || trim(g.block);
                        ok = !!e && (cls === '' || cls === 'error' || e === "'" + cls);
                    } else { act = trim(g.block); ok = act === exp; }
                    if (!ok) report.push(`  ${w.line}\n    want: ${exp}\n    got:  ${act}`);
                });
                return { failed: want.length ? report.length : -1, report: report.join('\n') };
            },

            /* Mount [{path, size?, sha256?}] under dir (default HOME) as lazy files read from
             * baseUrl + path on first read; a later manifest replaces an earlier file.
             * Answers the paths mounted. */
            addFiles(manifest, baseUrl, dir = HOME) {
                if (!Array.isArray(manifest)) throw new Error('manifest must be an array');
                manifest.forEach((f) => checkPath(f && f.path));
                const overHttp = typeof XMLHttpRequest !== 'undefined';
                return manifest.map((f) => {
                    const full = dir.replace(/\/$/, '') + '/' + f.path;
                    const cut = full.lastIndexOf('/');
                    M.FS.mkdirTree(full.slice(0, cut) || '/');
                    if (M.FS.analyzePath(full).exists) M.FS.unlink(full);
                    let url = baseUrl + (overHttp ? f.path.split('/').map(encodeURIComponent).join('/') : f.path);
                    if (overHttp && f.sha256) url += '?v=' + f.sha256;
                    const node = M.FS.createLazyFile(full.slice(0, cut) || '/', full.slice(cut + 1), url, true, false);
                    /* without XHR (node) the url is a disk path, and emscripten's lazy node cannot even
                     * be stat'ed until loaded: read it now */
                    if (!overHttp) M.FS.forceLoadFile(node);
                    return f.path;
                });
            },
        };
    }

    const api = { boot, HOME };
    if (typeof module === 'object' && module.exports) module.exports = api;
    else root.PeachQEngine = api;
})(typeof self !== 'undefined' ? self : this);

/* The engine side of the wasm REPL, shared by the Worker (worker.js) and the node smoke
 * (smoke.js): boots the emscripten module, owns its C ABI (q_wasm.c) and mounts file
 * manifests.  No DOM and no messaging — the host supplies the factory and the fetch. */
(function (root) {
    'use strict';

    const HOME = '/home/q';

    function checkPath(path) {
        if (typeof path !== 'string' || /[\\\x00-\x1f]/.test(path) ||
            !path.split('/').every((p) => p && p !== '.' && p !== '..')) {
            throw new Error('invalid manifest path: ' + JSON.stringify(path));
        }
    }

    /* factory: createPeachQ.  fetch: the Module.peachqFetch hook (q_wasm_http.c).
     * locateFile: where peachq.wasm lives. */
    async function boot({ factory, fetch, locateFile }) {
        let out = [], err = [];
        const M = await factory({
            locateFile,
            print: (s) => out.push(s),
            printErr: (s) => err.push(s),
            peachqFetch: fetch,
        });
        M.FS.mkdirTree(HOME);
        M.FS.chdir(HOME);
        if (M.ccall('q_wasm_init', 'number', [], []) !== 0) throw new Error('q runtime init failed');
        out = []; err = [];
        const enc = new TextEncoder();

        return {
            home: HOME,

            /* One line of q: what it printed to stdout and to stderr ('' when clean). */
            eval(src) {
                const bytes = enc.encode(String(src));
                const p = M._malloc(bytes.length + 1);
                M.HEAPU8.set(bytes, p);
                M.HEAPU8[p + bytes.length] = 0;
                out = []; err = [];
                try {
                    M._q_wasm_eval(p);
                } finally {
                    M._free(p);
                }
                return { out: out.join('\n'), err: err.join('\n') };
            },

            /* A .qcmd transcript run in this session by the native qdoc runner: the failing-row
             * count (-1 when no row ran) and the runner's report of the failures. */
            qdoc(text) {
                const path = '/tmp/ledger.qcmd';
                M.FS.writeFile(path, String(text));
                out = []; err = [];
                const failed = M.ccall('q_wasm_qdoc', 'number', ['string'], [path]);
                return { failed, report: out.join('\n') };
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

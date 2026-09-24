/* The Web Worker that hosts the peachq engine, so a long query never freezes the page and
 * the engine may block: q's HTTP rides a synchronous XHR and lazy files a synchronous Range
 * XHR, both legal only off the page thread.  peachq-client.js is the page side; the
 * protocol is {id, op, ...} in and {id, result} | {id, error, fatal} out. */
'use strict';
importScripts('engine.js', 'peachq.js');

const HTTP_TIMEOUT_MS = 30000;

/* Module.peachqFetch (q_wasm_http.c): the browser follows redirects and decodes gzip
 * itself and drops the headers it forbids a page to set; CORS decides what is reachable. */
function xhrFetch(method, url, headers, body) {
    const x = new XMLHttpRequest();
    x.open(method, url, false);
    x.responseType = 'arraybuffer';
    x.timeout = HTTP_TIMEOUT_MS;
    for (const line of headers.split('\r\n')) {
        const i = line.indexOf(':');
        if (i <= 0) continue;
        try { x.setRequestHeader(line.slice(0, i).trim(), line.slice(i + 1).trim()); } catch (_) { /* forbidden */ }
    }
    x.send(body.length ? body : null);
    if (x.status === 0) throw new Error('network error');
    return {
        status: x.status,
        statusText: x.statusText,
        headers: x.getAllResponseHeaders(),
        body: new Uint8Array(x.response || new ArrayBuffer(0)),
    };
}

let engine = null;

async function start({ files }) {
    const here = new URL('.', self.location.href).href;
    engine = await PeachQEngine.boot({
        factory: createPeachQ,
        fetch: xhrFetch,
        locateFile: (p) => here + p,
    });
    if (files) {
        const r = await fetch(here + 'files.json', { cache: 'no-cache' });
        if (!r.ok) throw new Error('files.json: HTTP ' + r.status);
        engine.addFiles(await r.json(), here + 'files/');
    }
    return { home: engine.home };
}

self.onmessage = async (e) => {
    const { id, op } = e.data;
    try {
        let result;
        if (op === 'start') result = await start(e.data);
        else if (!engine) throw new Error('engine not started');
        else if (op === 'eval') result = engine.eval(e.data.src);
        else if (op === 'addFiles') result = engine.addFiles(e.data.manifest, e.data.baseUrl);
        else throw new Error('unknown op ' + op);
        self.postMessage({ id, result });
    } catch (err) {
        /* an emscripten abort (a lazy file the server refused, an out-of-memory) kills the module */
        const msg = String((err && err.message) || err);
        self.postMessage({ id, error: msg, fatal: err instanceof WebAssembly.RuntimeError || /^Aborted/.test(msg) });
    }
};

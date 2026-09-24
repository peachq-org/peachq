/* The page side of the wasm REPL: the engine runs in worker.js, and a page drives it only
 * through this API — it never touches the module.
 *
 *   const q = await PeachQ.start({ base: 'wasm/', onStatus: (s) => … });
 *   const { out, err } = await q.eval('til 5');      // err is '' unless stderr was written
 *   await q.addFiles('repl/files.json', 'repl/files/');   // or an array manifest
 *   await q.restart();                               // a fresh engine; added files come back
 *
 * status: 'loading' | 'ready' | 'busy' | 'failed'.  A 'failed' session answers every call
 * with a rejection until restart(). */
(function (root) {
    'use strict';

    const abs = (url) => new URL(url, document.baseURI).href;

    class Session {
        constructor(opts) {
            this.opts = opts;
            this.base = abs(opts.base || './');
            this.layers = [];
            this.status = 'loading';
            this.worker = null;
            this.pending = new Map();
            this.seq = 0;
        }

        setStatus(s) {
            if (this.status === s) return;
            this.status = s;
            if (this.opts.onStatus) this.opts.onStatus(s);
        }

        call(msg) {
            if (!this.worker) return Promise.reject(new Error('peachq session is ' + this.status));
            const id = ++this.seq;
            return new Promise((resolve, reject) => {
                this.pending.set(id, { resolve, reject });
                this.worker.postMessage(Object.assign({ id }, msg));
            });
        }

        onMessage({ id, result, error, fatal }) {
            const p = this.pending.get(id);
            if (!p) return;
            this.pending.delete(id);
            if (fatal) this.fail(new Error(error));
            if (error === undefined) p.resolve(result); else p.reject(new Error(error));
            if (this.status === 'busy' && this.pending.size === 0) this.setStatus('ready');
        }

        stop(err) {
            if (this.worker) this.worker.terminate();
            this.worker = null;
            for (const p of this.pending.values()) p.reject(err);
            this.pending.clear();
        }

        fail(err) {
            this.stop(err);
            this.setStatus('failed');
        }

        /* A restart during a boot supersedes it: only the current worker's outcome touches the session. */
        boot() {
            const w = new Worker(this.base + 'worker.js');
            this.worker = w;
            this.setStatus('loading');
            w.onmessage = (e) => this.onMessage(e.data);
            w.onerror = (e) => { e.preventDefault(); if (this.worker === w) this.fail(new Error(e.message || 'worker failed')); };
            this.ready = (async () => {
                try {
                    const info = await this.call({ op: 'start', files: this.opts.files !== false });
                    this.home = info.home;
                    for (const [manifest, baseUrl] of this.layers) await this.call({ op: 'addFiles', manifest, baseUrl });
                } catch (err) {
                    if (this.worker === w) this.fail(err);
                    throw err;
                }
                if (this.worker === w) this.setStatus('ready');
            })();
            return this.ready;
        }

        async eval(src) {
            await this.ready;
            if (this.status === 'ready') this.setStatus('busy');
            return this.call({ op: 'eval', src: String(src) });
        }

        /* Mount a host manifest ([{path, size?, sha256?}], or the URL of one) lazily under
         * the start directory: a file is fetched from baseUrl + path when q first reads it. */
        async addFiles(manifest, baseUrl) {
            await this.ready;
            if (typeof manifest === 'string') {
                const r = await fetch(abs(manifest), { cache: 'no-cache' });
                if (!r.ok) throw new Error(manifest + ': HTTP ' + r.status);
                manifest = await r.json();
            }
            const layer = [manifest, abs(baseUrl)];
            const paths = await this.call({ op: 'addFiles', manifest: layer[0], baseUrl: layer[1] });
            this.layers.push(layer);
            return paths;
        }

        /* There is no interrupt: a runaway query is stopped by replacing the engine. */
        restart() {
            this.stop(new Error('peachq restarted'));
            return this.boot();
        }

        terminate() {
            this.fail(new Error('peachq terminated'));
        }
    }

    /* options: base (the directory holding worker.js — default './'), files (mount the
     * build's own files.json, default true), onStatus(status). */
    async function start(options = {}) {
        const s = new Session(options);
        await s.boot();
        return s;
    }

    root.PeachQ = { start };
})(typeof self !== 'undefined' ? self : this);

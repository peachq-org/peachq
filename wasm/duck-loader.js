/* DuckDB's own published browser build (wasm/duckdb-wasm.pin), instantiated synchronously from inside a q evaluation:
 * q_wasm_duckdb.c calls Module.peachqDuckLoad on the first DuckDB call, and the worker answers with loadDuckSync.  The
 * bundle's emscripten factory is reached through the bindings class createDuckDB would construct, its instantiateWasm
 * replaced by a blocking compile and its run made synchronous, so the module is live when the call returns.  The
 * bridge then drives DuckDB's v2 C API on that module's exports; its emscripten FS sees q's through a proxy mount,
 * and http(s) goes through the XMLHttpRequest client the bundle registers for every database it opens. */
(function (root) {
    'use strict';

    /* The three text patches the bundle needs; each fails loudly if a new bundle no longer has its anchor. */
    const CREATE = /async function [\w$]+\([\w$]+,([\w$]+),([\w$]+)\)\{return await [\w$]+\(\),new ([\w$]+)\(\1,\2,[\w$]+\.base\.mainModule\)\}/;
    const FS_ANCHOR = 'Module.ccall=ccall,';
    const RUN_ANCHOR = 'wasmExports=await createWasm(),await run(),Module}';

    function patchBundle(text) {
        const m = CREATE.exec(text);
        if (!m) throw new Error('duckdb-wasm bundle: the createDuckDB shape is not recognised');
        if (!text.includes(FS_ANCHOR)) throw new Error('duckdb-wasm bundle: the Module FS anchor is missing');
        if (!text.includes(RUN_ANCHOR)) throw new Error('duckdb-wasm bundle: the factory run anchor is missing');
        return text.split(FS_ANCHOR).join(FS_ANCHOR + 'Module.FS=FS,')
                   .split(RUN_ANCHOR).join('createWasm(),run(),Module}') +
               '\n;module.exports.__BINDINGS=' + m[3] + ';';
    }

    /* Emscripten's PROXYFS, which their bundle does not carry: their FS mounts directories of q's. */
    function proxyFs(FS, host) {
        const wrap = (f) => {
            try { return f(); } catch (e) {
                if (e && typeof e.errno === 'number') throw new FS.ErrnoError(e.errno);
                throw e;
            }
        };
        const P = {
            mount(m) { return P.createNode(null, '/', host.lstat(m.opts.root).mode, 0); },
            createNode(parent, name, mode) {
                const node = FS.createNode(parent, name, mode);
                node.node_ops = P.node_ops;
                node.stream_ops = P.stream_ops;
                return node;
            },
            realPath(node) {
                const parts = [];
                while (node.parent !== node) { parts.push(node.name); node = node.parent; }
                parts.push(node.mount.opts.root);
                return parts.reverse().join('/').replace(/\/+/g, '/');
            },
            node_ops: {
                getattr: (node) => wrap(() => host.lstat(P.realPath(node))),
                setattr(node, attr) {
                    wrap(() => {
                        const p = P.realPath(node);
                        if (attr.mode !== undefined) { host.chmod(p, attr.mode); node.mode = attr.mode; }
                        if (attr.size !== undefined) host.truncate(p, attr.size);
                        if (attr.timestamp !== undefined) host.utime(p, attr.timestamp, attr.timestamp);
                    });
                },
                lookup: (parent, name) => wrap(() => P.createNode(parent, name, host.lstat(P.realPath(parent) + '/' + name).mode)),
                mknod(parent, name, mode) {
                    const node = P.createNode(parent, name, mode);
                    const p = P.realPath(node);
                    wrap(() => (FS.isDir(mode) ? host.mkdir(p, mode) : host.writeFile(p, '', { mode })));
                    return node;
                },
                rename(o, d, n) { wrap(() => host.rename(P.realPath(o), P.realPath(d) + '/' + n)); o.name = n; },
                unlink: (parent, name) => wrap(() => host.unlink(P.realPath(parent) + '/' + name)),
                rmdir: (parent, name) => wrap(() => host.rmdir(P.realPath(parent) + '/' + name)),
                readdir: (node) => wrap(() => host.readdir(P.realPath(node))),
                symlink: (parent, n, old) => wrap(() => host.symlink(old, P.realPath(parent) + '/' + n)),
                readlink: (node) => wrap(() => host.readlink(P.realPath(node))),
            },
            stream_ops: {
                open(s) { s.nfd = wrap(() => host.open(P.realPath(s.node), s.flags)); },
                close(s) { wrap(() => host.close(s.nfd)); },
                read: (s, buf, off, len, pos) => wrap(() => host.read(s.nfd, buf, off, len, pos)),
                write: (s, buf, off, len, pos) => wrap(() => host.write(s.nfd, buf, off, len, pos)),
                llseek(s, offset, whence) {
                    let pos = offset;
                    if (whence === 1) pos += s.position;
                    else if (whence === 2 && FS.isFile(s.node.mode)) pos += host.lstat(P.realPath(s.node)).size;
                    if (pos < 0) throw new FS.ErrnoError(28);
                    return pos;
                },
            },
        };
        return P;
    }

    /* A module of one function, nparams i32s in and an i32 out when result, that calls its one import: placed in
     * DuckDB's function table it is a callback DuckDB invokes, and the import is JS.  Answers the table slot. */
    function trampoline(X, nparams, result, fn) {
        const section = (id, bytes) => [id, bytes.length, ...bytes];
        const params = Array.from({ length: nparams }, (_, i) => [0x20, i]).flat();
        const bytes = Uint8Array.from([
            0, 97, 115, 109, 1, 0, 0, 0,
            ...section(1, [1, 0x60, nparams, ...Array(nparams).fill(0x7f), result, ...(result ? [0x7f] : [])]),
            ...section(2, [1, 3, 101, 110, 118, 2, 99, 98, 0, 0]),
            ...section(3, [1, 0]),
            ...section(7, [1, 1, 102, 0, 1]),
            ...section(10, [1, params.length + 4, 0, ...params, 0x10, 0, 0x0b]),
        ]);
        const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env: { cb: fn } });
        const i = X.__indirect_function_table.grow(1);
        X.__indirect_function_table.set(i, inst.exports.f);
        return i;
    }

    /* The bundle gives extension loading to its WebDB alone (webdb.cc sets the WebDB config's extension provider), so
     * an instance opened over the v2 API gets the same provider through the door DuckDB offers every database it
     * opens: a static capability (duckdb_static_extension.h, layout 2) whose database callback installs it, through
     * the bundle's own exported symbols.  Each is named here so a bundle that no longer exports one fails loudly. */
    const SYMBOL = {
        config: '_ZN6duckdb8DBConfig9GetConfigERNS_16DatabaseInstanceE',
        provider: '_ZN6duckdb15make_shared_ptrINS_3web21WasmExtensionProviderEJEEENS_10shared_ptrIT_Lb1EEEDpOT0_',
        install: '_ZN6duckdb8DBConfig28SetExternalExtensionProviderERKNS_10shared_ptrINS_25ExternalExtensionProviderELb1EEE',
        register: 'duckdb_register_static_extension',
    };

    function provideExtensions(X) {
        for (const k of Object.keys(SYMBOL))
            if (typeof X[SYMBOL[k]] !== 'function') throw new Error('duckdb-wasm bundle: ' + SYMBOL[k] + ' is not exported');
        const onDatabase = trampoline(X, 1, 0, (db) => {
            const sp = X.malloc(8);   /* a shared_ptr the config copies; this one is kept with the instance */
            X[SYMBOL.provider](sp);
            X[SYMBOL.install](X[SYMBOL.config](db), sp);
        });
        const name = new TextEncoder().encode('peachq_extension_provider\0');
        const namep = X.malloc(name.length);
        new Uint8Array(X.memory.buffer, namep, name.length).set(name);
        const describe = trampoline(X, 1, 1, (d) => {
            const dv = new DataView(X.memory.buffer);
            if (dv.getUint32(d, true) < 2) return 1;
            dv.setUint32(d, 2, true);
            dv.setUint32(d + 12, namep, true);
            dv.setUint32(d + 36, onDatabase, true);
            return 0;
        });
        if (X[SYMBOL.register](describe) !== 0) throw new Error('duckdb-wasm: registering the extension provider failed');
    }

    /* base: the URL of the pinned version directory.  get(url, responseType): a blocking fetch.  hostFS: q's FS.
     * rscan(fn, info, ctx, err): q's module entered from a replacement scan.  warn(text): q's error stream. */
    function loadDuckSync({ base, get, hostFS, mounts, rscan, warn }) {
        const src = patchBundle(get(base + 'duckdb-browser-blocking.cjs', 'text'));
        const mod = { exports: {} };
        const req = (n) => { if (n === 'apache-arrow') return {}; throw new Error('duckdb-wasm bundle: require ' + n); };
        new Function('module', 'exports', 'require', src)(mod, mod.exports, req);
        const D = mod.exports;
        const db = new D.__BINDINGS(new D.VoidLogger(), D.BROWSER_RUNTIME, base + 'duckdb-base.wasm');
        let X = null, M = null;
        db.instantiateWasm = function (imports, success) {
            globalThis.DUCKDB_RUNTIME = D.BROWSER_RUNTIME;
            const wm = new WebAssembly.Module(new Uint8Array(get(base + 'duckdb-base.wasm', 'arraybuffer')));
            const inst = new WebAssembly.Instance(wm, imports);
            X = inst.exports;
            success(inst, wm);
            return inst.exports;
        };
        db.instantiateImpl({ print: () => {}, printErr: () => {}, onRuntimeInitialized() { M = this; } });
        if (!M) throw new Error('duckdb-wasm: the runtime did not initialise synchronously');
        const P = proxyFs(M.FS, hostFS);
        const mounted = new Set();
        const mount = (p) => {
            try { M.FS.mkdirTree(p); } catch (_) { /* exists */ }
            M.FS.mount(P, { root: p }, p);
            mounted.add(p);
        };
        mounts.forEach(mount);
        provideExtensions(X);
        const tramps = new Map();
        const rscanTrampoline = (fn) => {
            let i = tramps.get(fn);
            if (i === undefined) {
                i = trampoline(X, 3, 0, (info, ctx, err) => rscan(fn, info, ctx, err));
                tramps.set(fn, i);
            }
            return i;
        };
        /* Before each statement their FS gains every top-level directory q has made since (never one DuckDB's FS
         * owns itself), so an absolute path means the same on both sides, and follows q's current directory, so a
         * relative path does too; a directory it still cannot enter refuses the statement. */
        const THEIRS = new Set(['/dev', '/proc']);
        const before = () => {
            for (const n of hostFS.readdir('/')) {
                const p = '/' + n;
                if (n === '.' || n === '..' || THEIRS.has(p) || mounted.has(p) || !hostFS.isDir(hostFS.stat(p).mode)) continue;
                try { mount(p); } catch (_) { /* theirs already */ }
            }
            const cwd = hostFS.cwd();
            try {
                if (M.FS.cwd() !== cwd) M.FS.chdir(cwd);
                return true;
            } catch (_) {
                warn('duckdb-wasm: DuckDB cannot enter ' + cwd + ', where q stands');
                return false;
            }
        };
        return { X, mem: X.memory, repo: base + 'extensions', before, trampoline: rscanTrampoline };
    }

    root.loadDuckSync = loadDuckSync;
})(self);

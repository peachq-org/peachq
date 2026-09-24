/* DuckDB's own published browser build (wasm/duckdb-wasm.pin), instantiated synchronously from inside a q evaluation:
 * q_wasm_duckdb.c calls Module.peachqDuckLoad on the first DuckDB call, and the worker answers with loadDuckSync.  The
 * bundle's factory is reached through the class createDuckDB would construct, and its instantiateWasm is replaced by a
 * blocking compile, so the module is live when the call returns.  Main is their WebDB, whose file runtime sends local
 * paths to q's filesystem and http(s)/s3 to theirs; their emscripten FS sees q's through a proxy mount. */
(function (root) {
    'use strict';

    /* The two text patches the bundle needs; each fails loudly if a new bundle no longer has its anchor. */
    const CREATE =
        /async function ([\w$]+)\(([\w$]+),([\w$]+),([\w$]+)\)\{return\(await ([\w$]+)\(\)\)\.wasmExceptions&&\2\.eh\?new ([\w$]+)\(/;
    const FS_ANCHOR = 'Module.ccall=ccall,';
    const NODE_FS = 1;      /* their DuckDBDataProtocol.NODE_FS: the protocol our runtime claims for local paths */
    const READ_WRITE = 3;   /* their DuckDBAccessMode.READ_WRITE: a WebDB opened on a path is otherwise read-only */
    const F = { WRITE: 2, CREATE: 8, CREATE_NEW: 16, NULL_IF_NOT_EXISTS: 128, EXCLUSIVE_CREATE: 512, NULL_IF_EXISTS: 1024 };

    function patchBundle(text) {
        const m = CREATE.exec(text);
        if (!m) throw new Error('duckdb-wasm bundle: the createDuckDB shape is not recognised');
        if (!text.includes(FS_ANCHOR)) throw new Error('duckdb-wasm bundle: the Module FS anchor is missing');
        return text.split(FS_ANCHOR).join(FS_ANCHOR + 'Module.FS=FS,') + '\n;module.exports.__EH=' + m[6] + ';';
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

    const WILD = /[*?[]/;

    function segmentRe(seg) {
        let re = '';
        for (let i = 0; i < seg.length; i++) {
            const c = seg[i];
            if (c === '*') re += '[^/]*';
            else if (c === '?') re += '[^/]';
            else if (c === '[') {
                const neg = seg[i + 1] === '!';
                const end = seg.indexOf(']', i + (neg ? 3 : 2));   /* a ] first in the class is literal */
                if (end < 0) { re += '\\['; continue; }
                const body = seg.slice(i + (neg ? 2 : 1), end).replace(/[\\\]^]/g, '\\$&');
                re += '[' + (neg ? '^' : '') + body + ']';
                i = end;
            } else re += c.replace(/[.+^${}()|\\\]]/g, '\\$&');
        }
        return new RegExp('^' + re + '$');
    }

    /* A glob over q's filesystem, one path segment at a time: * ? [..] within a segment, ** for any depth. */
    function globPaths(host, pattern) {
        const abs = pattern.startsWith('/');
        const segs = pattern.split('/').filter((s) => s !== '');
        const isDir = (p) => { try { return host.isDir(host.stat(p).mode); } catch (_) { return false; } };
        const join = (d, n) => (d === '' ? n : d === '/' ? '/' + n : d + '/' + n);
        const list = (d) => {
            try { return host.readdir(d === '' ? '.' : d).filter((n) => n !== '.' && n !== '..'); } catch (_) { return []; }
        };
        let found = [abs ? '/' : ''];
        segs.forEach((seg, i) => {
            const last = i === segs.length - 1;
            const next = [];
            for (const d of found) {
                if (seg === '**') {
                    const walk = (p) => {
                        next.push(p);
                        for (const n of list(p)) {
                            const q = join(p, n);
                            if (isDir(q)) walk(q);
                            else if (last) next.push(q);
                        }
                    };
                    walk(d);
                } else if (!WILD.test(seg)) {
                    const p = join(d, seg);
                    if (last ? host.analyzePath(p).exists : isDir(p)) next.push(p);
                } else {
                    const re = segmentRe(seg);
                    for (const n of list(d)) if (re.test(n) && (last || isDir(join(d, n)))) next.push(join(d, n));
                }
            }
            found = next;
        });
        return found.filter((p) => p !== '' && !isDir(p)).sort();
    }

    function hostRuntime(D, BR, host) {
        const fds = new Map();
        const remote = (p) => /^(https?|s3):\/\//i.test(p);
        const exists = (p) => host.analyzePath(p).exists;
        const fail = (mod, e) => { D.failWith(mod, String((e && e.message) || e)); return 0; };
        const rt = Object.assign({}, BR);
        const byId = (name, local) => function (mod, id, ...a) {
            const fi = BR.getFileInfo(mod, id);
            if (!fi || fi.dataProtocol !== NODE_FS) return BR[name](mod, id, ...a);
            try { return local(mod, fi, ...a); } catch (e) { return fail(mod, e); }
        };
        const byPath = (name, local) => function (mod, p, n, ...a) {
            const path = D.readString(mod, p, n);
            if (remote(path)) return BR[name](mod, p, n, ...a);
            try { return local(mod, path, ...a); } catch (e) { return fail(mod, e); }
        };
        rt.getDefaultDataProtocol = () => NODE_FS;
        /* DuckDB's FileFlags, as their runtime answers them: a null handle where the flags ask for one, else
         * {size, buffer (none), mtime} as three doubles. */
        rt.openFile = byId('openFile', (mod, fi, flags) => {
            const p = fi.dataUrl || fi.fileName;
            const there = exists(p);
            if (there ? flags & F.NULL_IF_EXISTS : flags & F.NULL_IF_NOT_EXISTS) return 0;
            if (there && flags & F.EXCLUSIVE_CREATE) throw new Error('file already exists: ' + p);
            if (!there && !(flags & (F.CREATE | F.CREATE_NEW))) throw new Error('no such file: ' + p);
            const s = host.open(p, !there || flags & F.CREATE_NEW ? 'w+' : flags & F.WRITE ? 'r+' : 'r');
            fds.set(fi.fileId, { s, path: p });
            const st = host.stat(p);
            const out = mod._malloc(24);
            mod.HEAPF64[out >> 3] = st.size;
            mod.HEAPF64[(out >> 3) + 1] = 0;
            mod.HEAPF64[(out >> 3) + 2] = st.mtime.getTime() / 1e3;
            return out;
        });
        rt.closeFile = byId('closeFile', (mod, fi) => {
            const f = fds.get(fi.fileId);
            fds.delete(fi.fileId);
            if (f) host.close(f.s);
            return 0;
        });
        rt.readFile = byId('readFile', (mod, fi, buf, n, at) => host.read(fds.get(fi.fileId).s, mod.HEAPU8, buf, n, at));
        rt.writeFile = byId('writeFile', (mod, fi, buf, n, at) => host.write(fds.get(fi.fileId).s, mod.HEAPU8, buf, n, at));
        rt.truncateFile = byId('truncateFile', (mod, fi, n) => { host.truncate(fi.dataUrl || fi.fileName, n); return 0; });
        rt.getLastFileModificationTime = byId('getLastFileModificationTime',
            (mod, fi) => host.stat(fi.dataUrl || fi.fileName).mtime.getTime() / 1e3);
        rt.checkFile = byPath('checkFile', (mod, p) => exists(p) && !host.isDir(host.stat(p).mode));
        rt.checkDirectory = byPath('checkDirectory', (mod, p) => exists(p) && host.isDir(host.stat(p).mode));
        rt.createDirectory = byPath('createDirectory', (mod, p) => { host.mkdirTree(p); return 0; });
        rt.removeDirectory = byPath('removeDirectory', (mod, p) => { host.rmdir(p); return 0; });
        rt.removeFile = byPath('removeFile', (mod, p) => { if (exists(p)) host.unlink(p); return 0; });
        rt.moveFile = function (mod, a, an, b, bn) {
            const from = D.readString(mod, a, an), to = D.readString(mod, b, bn);
            if (remote(from)) return BR.moveFile(mod, a, an, b, bn);
            try { host.rename(from, to); return true; } catch (e) { return fail(mod, e); }
        };
        rt.glob = byPath('glob', (mod, pat) => {
            for (const p of WILD.test(pat) ? globPaths(host, pat) : exists(pat) ? [pat] : [])
                mod.ccall('duckdb_web_fs_glob_add_path', null, ['string'], [p]);
            return 0;
        });
        return { rt, localPaths: () => new Set([...fds.values()].map((f) => f.path)) };
    }

    /* base: the URL of the pinned version directory.  get(url, responseType): a blocking fetch.  hostFS: q's FS. */
    function loadDuckSync({ base, get, hostFS, mounts }) {
        const src = patchBundle(get(base + 'duckdb-browser-blocking.cjs', 'text'));
        const mod = { exports: {} };
        const req = (n) => { if (n === 'apache-arrow') return {}; throw new Error('duckdb-wasm bundle: require ' + n); };
        new Function('module', 'exports', 'require', src)(mod, mod.exports, req);
        const D = mod.exports;
        const { rt, localPaths } = hostRuntime(D, D.BROWSER_RUNTIME, hostFS);
        const db = new D.__EH(new D.VoidLogger(), rt, 'duckdb-eh.wasm');
        let X = null, mem = null, M = null;
        db.instantiateWasm = function (imports, success) {
            globalThis.DUCKDB_RUNTIME = rt;
            const wm = new WebAssembly.Module(new Uint8Array(get(base + 'duckdb-eh.wasm', 'arraybuffer')));
            const inst = new WebAssembly.Instance(wm, imports);
            X = inst.exports;
            mem = imports.env && imports.env.memory;
            success(inst, wm);
            return inst.exports;
        };
        db.instantiateImpl({ print: () => {}, printErr: () => {}, onRuntimeInitialized() { M = this; } });
        if (!M) throw new Error('duckdb-wasm: the runtime did not initialise synchronously');
        db._instance = M;
        globalThis.DUCKDB_BINDINGS = db;
        mem = mem || X.memory || M.wasmMemory;
        const P = proxyFs(M.FS, hostFS);
        for (const p of mounts) {
            try { M.FS.mkdirTree(p); } catch (_) { /* exists */ }
            M.FS.mount(P, { root: p }, p);
        }

        const cstr = (s) => {
            const b = new TextEncoder().encode(s + '\0');
            const p = X.malloc(b.length);
            new Uint8Array(mem.buffer, p, b.length).set(b);
            return p;
        };
        const ostr = (p) => {
            const h = new Uint8Array(mem.buffer);
            let e = p;
            while (h[e]) e++;
            return new TextDecoder().decode(h.subarray(p, e));
        };
        /* One statement on a C-API connection; answers the first cell as text, or throws DuckDB's error. */
        const exec = (con, sql) => {
            const res = X.malloc(64), q = cstr(sql);
            new Uint8Array(mem.buffer, res, 64).fill(0);
            try {
                if (X.duckdb_query(con, q, res) !== 0) throw new Error(ostr(X.duckdb_result_error(res)));
                const v = X.duckdb_value_varchar(res, 0, 0, 0, 0);
                const s = v ? ostr(v) : null;
                if (v) X.duckdb_free(v);
                return s;
            } finally {
                X.duckdb_destroy_result(res);
                X.free(res);
                X.free(q);
            }
        };
        /* A C-API duckdb_connection is WebDB::Connection + 8 (their connect answers the WebDB object); checked on
         * every open by reading back a setting through it, so a bundle whose layout moved fails here, not later. */
        const CONN_OFFSET = 8;
        const connect = () => db.connect()._conn + CONN_OFFSET;
        const disconnect = (con) => db.disconnect(con - CONN_OFFSET);
        const repo = new URL('extensions', base).href;
        const ident = (s) => '"' + s.replace(/"/g, '""') + '"';
        const literal = (s) => "'" + s.replace(/'/g, "''") + "'";
        /* Extensions (autoloaded or LOADed) come from the pinned directory beside the bundle, never DuckDB's own. */
        const setup = 'SET custom_extension_repository = ' + literal(repo);
        /* The open-only options a WebDB takes in its own config; every other pair is a SET once it is open, as a
         * running DuckDB takes it (one that is open-only elsewhere fails there, with DuckDB's own message). */
        const ON_OPEN = {
            access_mode: (c, v) => { c.accessMode = { automatic: 1, read_only: 2, read_write: 3 }[v.toLowerCase()]; },
            allow_unsigned_extensions: (c, v) => { c.allowUnsignedExtensions = /^(true|1|t|on|yes)$/i.test(v); },
        };
        const open = (path, pairs) => {
            /* Their page buffer registers a file under the flags of its first open, the read-only existence probe;
             * direct I/O keeps database files out of it, or a checkpoint's write fails 'File is not opened in write
             * mode' and a re-ATTACH after DETACH 'already locked exclusively'. */
            const cfg = path ? { path, accessMode: READ_WRITE, useDirectIO: true } : { path: ':memory:', useDirectIO: true };
            const sets = [];
            for (const [k, v] of pairs) {
                const on = ON_OPEN[k.toLowerCase()];
                if (on) on(cfg, v);
                else sets.push('SET ' + ident(k) + ' = ' + literal(v));
            }
            db.open(cfg);
            const con = connect();
            try {
                let got;
                try {
                    exec(con, setup);
                    got = exec(con, "SELECT current_setting('custom_extension_repository')");
                } catch (e) {
                    throw new Error('duckdb-wasm layout: WebDB::Connection + ' + CONN_OFFSET + ' is not a connection (' + e.message + ')');
                }
                if (got !== repo) throw new Error('duckdb-wasm layout: WebDB::Connection + ' + CONN_OFFSET + ' read back ' + got);
                if (sets.length) exec(con, sets.join('; '));
            } finally {
                disconnect(con);
            }
        };
        const close = () => db.reset();
        /* Before each statement.  Their page buffer also keeps a file no handle holds any more, under the flags it
         * was read with, so a later write to that path fails the same way: drop each such local file (dropFile leaves
         * one in use alone).  And their FS follows q's current directory. */
        const beforeQuery = () => {
            for (const p of localPaths()) try { db.dropFile(p); } catch (_) { /* in use */ }
            try { if (M.FS.cwd() !== hostFS.cwd()) M.FS.chdir(hostFS.cwd()); } catch (_) { /* outside the mounts */ }
        };
        return { X, mem, open, close, connect, disconnect, beforeQuery };
    }

    root.loadDuckSync = loadDuckSync;
})(self);

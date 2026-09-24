/* The headless check of the wasm build under node: real q through wasm/engine.js, the same
 * engine code the Worker runs.  The plain-q rows are wasm/smoke.qcmd, run inside the module
 * by the native qdoc runner; the rows here need JS.  HTTP goes through a node Module.peachqFetch (a child
 * process doing fetch) to a fixture server that is a second child, since the hook blocks
 * this process; the build's files.json is mounted as lazy files read from disk.
 *
 *   node wasm/smoke.js build/wasm/www
 *
 * The same file is the fixture server (--serve) and the one-request fetch child (--fetch). */
'use strict';
const path = require('path');
const fs = require('fs');
const zlib = require('zlib');
const { spawn, spawnSync } = require('child_process');

const BIN = Buffer.from(Array.from({ length: 128 }, (_, i) => 0x80 + i));
const TEXT = 'hello peachq';

function serve() {
    const http = require('http');
    let lastRange = '';
    const server = http.createServer((req, res) => {
        const body = [];
        req.on('data', (c) => body.push(c));
        req.on('end', () => {
            if (req.url === '/echo') return res.end(Buffer.concat(body));
            if (req.url === '/gz') {
                res.writeHead(200, { 'Content-Encoding': 'gzip' });
                return res.end(zlib.gzipSync('zipped text'));
            }
            if (req.url === '/lastrange') return res.end(lastRange);
            if (req.url === '/redirect') {
                res.writeHead(302, { Location: '/t.txt' });
                return res.end();
            }
            const data = { '/b.bin': BIN, '/t.txt': Buffer.from(TEXT) }[req.url];
            if (!data) { res.writeHead(404); return res.end(); }
            if (req.headers.range) lastRange = req.headers.range;
            const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
            let chunk = data, status = 200, extra = {};
            if (m) {
                const from = +m[1], to = m[2] ? Math.min(+m[2], data.length - 1) : data.length - 1;
                chunk = data.subarray(from, to + 1);
                status = 206;
                extra = { 'Content-Range': `bytes ${from}-${to}/${data.length}` };
            }
            res.writeHead(status, Object.assign({ 'Content-Length': chunk.length, 'Accept-Ranges': 'bytes' }, extra));
            res.end(req.method === 'HEAD' ? undefined : chunk);
        });
    });
    server.listen(0, '127.0.0.1', () => console.log(server.address().port));
}

async function fetchChild() {
    const q = JSON.parse(fs.readFileSync(0, 'utf8'));
    const headers = q.headers.split('\r\n').filter(Boolean).map((l) => {
        const i = l.indexOf(':');
        return [l.slice(0, i).trim(), l.slice(i + 1).trim()];
    });
    const body = Buffer.from(q.body, 'base64');
    const r = await fetch(q.url, { method: q.method, headers, body: body.length ? body : undefined });
    const text = [...r.headers].map(([k, v]) => `${k}: ${v}\r\n`).join('');
    const bytes = Buffer.from(await r.arrayBuffer());
    process.stdout.write(JSON.stringify({ status: r.status, statusText: r.statusText, headers: text,
                                          body: bytes.toString('base64') }));
}

function nodeFetch(method, url, headers, body) {
    const r = spawnSync(process.execPath, [__filename, '--fetch'], {
        input: JSON.stringify({ method, url, headers, body: Buffer.from(body).toString('base64') }),
        maxBuffer: 64 << 20, timeout: 30000,
    });
    if (r.status !== 0) throw new Error('fetch failed');
    const j = JSON.parse(r.stdout);
    return { status: j.status, statusText: j.statusText, headers: j.headers, body: new Uint8Array(Buffer.from(j.body, 'base64')) };
}

function startServer() {
    const child = spawn(process.execPath, [__filename, '--serve'], { stdio: ['ignore', 'pipe', 'inherit'] });
    return new Promise((resolve, reject) => {
        child.stdout.once('data', (d) => resolve({ child, port: parseInt(String(d), 10) }));
        child.once('exit', () => reject(new Error('fixture server died')));
    });
}

/* What a .qcmd row cannot say: the fixture server's port, bytes compared as bytes,
 * and whether the Range header really went out. */
function cases(P) {
    const H = `http://127.0.0.1:${P}`;
    return [
        [`count read1 \`:${H}/b.bin`, '128'],
        [`(read1 \`:${H}/b.bin)~"x"$128+til 128`, '1b'],
        [`-8#read1 \`:${H}/b.bin`, '0x' + BIN.subarray(-8).toString('hex')],
        [`read1 (\`:${H}/b.bin;2;3)`, '0x828384'],
        [`.Q.hg "${H}/lastrange"`, '"bytes=2-4"'],
        [`.Q.hg "${H}/t.txt"`, `"${TEXT}"`],
        [`.Q.hp["${H}/echo";"text/plain";"ping"]`, '"ping"'],
        [`.Q.hg "${H}/gz"`, '"zipped text"'],
        [`.Q.hg "${H}/redirect"`, `"${TEXT}"`],
        [`12#(\`:${H}) "GET /t.txt HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"`, '"HTTP/1.1 200"'],
        [`-4#(\`:${H}) "POST /echo HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 4\\r\\n\\r\\npong"`, '"pong"'],
        [`read1 \`:${H}/missing`, { err: "'io" }],
        ['.Q.hg "http://127.0.0.1:1/"', { err: "'conn" }],
    ];
}

async function main() {
    if (parseInt(process.versions.node, 10) < 18) {
        console.error(`node ${process.versions.node} cannot parse the emscripten glue — use $EMSDK_NODE`);
        process.exit(1);
    }
    const www = path.resolve(process.argv[2] || path.join(__dirname, '..', 'build', 'wasm', 'www'));
    const { boot } = require('./engine.js');
    const { child, port } = await startServer();
    let failed = 0, total = 0;
    try {
        const q = await boot({ factory: require(path.join(www, 'peachq.js')), fetch: nodeFetch });
        q.addFiles(JSON.parse(fs.readFileSync(path.join(www, 'files.json'), 'utf8')), path.join(www, 'files') + path.sep);

        const ledger = fs.readFileSync(path.join(__dirname, 'smoke.qcmd'), 'utf8');
        const rows = ledger.split('\n').filter((l) => l.startsWith('q)')).length;
        const qd = q.qdoc(ledger);
        total += rows;
        failed += qd.failed < 0 ? rows : qd.failed;
        console.log(`${qd.failed === 0 ? 'ok' : 'FAIL'}  wasm/smoke.qcmd  ${rows - Math.max(qd.failed, 0)}/${rows} rows`);
        if (qd.failed) console.error(qd.report);   /* the runner's FAIL rows, among what the \l rows echoed */

        for (const [src, want] of cases(port)) {
            total++;
            const { out, err } = q.eval(src);
            const ok = typeof want === 'object' ? err.startsWith(want.err) : out === want && err === '';
            if (ok) console.log(`ok    ${src}`);
            else {
                failed++;
                console.error(`FAIL  ${src}\n  want ${JSON.stringify(want)}\n  out  ${JSON.stringify(out)}\n  err  ${JSON.stringify(err)}`);
            }
        }
    } finally {
        child.kill();
    }
    console.log(`${total - failed}/${total} passed`);
    process.exit(failed ? 1 : 0);
}

if (process.argv[2] === '--serve') serve();
else if (process.argv[2] === '--fetch') fetchChild().catch((e) => { console.error(String(e)); process.exit(1); });
else main().catch((e) => { console.error(e); process.exit(1); });

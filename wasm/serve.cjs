/* The browser checks' server: wasm/server.py over the build's directory with the repo's test/ beside it, so a
 * fixture is same-origin for the Worker (a lazy file or a DuckDB read needs no CORS).  The directory is a sibling of
 * the build's, made of links, so the build itself — what a release ships — never gains a file. */
'use strict';
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

function serve(www, name) {
    const dir = path.join(www, '..', name);
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir);
    for (const n of fs.readdirSync(www)) fs.symlinkSync(path.join(www, n), path.join(dir, n));
    fs.symlinkSync(path.join(__dirname, '..', 'test'), path.join(dir, 'test'));
    const child = spawn('python3', [path.join(__dirname, 'server.py'), '0', dir], { stdio: ['ignore', 'pipe', 'ignore'] });
    return new Promise((resolve, reject) => {
        child.stdout.once('data', (d) => {
            const m = /localhost:(\d+)/.exec(String(d));
            if (m) resolve({ child, origin: `http://localhost:${m[1]}` }); else reject(new Error('server said: ' + d));
        });
        child.once('exit', () => reject(new Error('server.py died')));
    });
}

module.exports = { serve };

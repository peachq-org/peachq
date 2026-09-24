/* The browser check of the wasm build: headless Chromium opens the reference page
 * (index.html, served by server.py) and drives the engine through peachq-client.js and the
 * Worker.  It pins what node cannot see: files are listed before any is fetched and fetched
 * only when read, HTTP rides the Worker's synchronous XHR, and the page stays responsive
 * while q computes.
 *
 *   node wasm/browser-smoke.cjs build/wasm/www [/path/to/node_modules]
 *
 * Playwright is resolved from that node_modules, else the usual way (NODE_PATH included);
 * without it this check SKIPS (exit 0). */
'use strict';
const path = require('path');
const { spawn } = require('child_process');

let chromium;
try {
    ({ chromium } = require(require.resolve('playwright', process.argv[3] ? { paths: [process.argv[3]] } : undefined)));
} catch (_) {
    console.log('SKIP  wasm browser smoke: playwright not found (name a node_modules that has it, or set NODE_PATH)');
    process.exit(0);
}

const www = path.resolve(process.argv[2] || path.join(__dirname, '..', 'build', 'wasm', 'www'));
const EXAMPLES = ['adverbs.q', 'csv.q', 'prices.csv', 'trades.q'];

function startServer() {
    const child = spawn('python3', [path.join(__dirname, 'server.py'), '0', www], { stdio: ['ignore', 'pipe', 'ignore'] });
    return new Promise((resolve, reject) => {
        child.stdout.once('data', (d) => {
            const m = /localhost:(\d+)/.exec(String(d));
            if (m) resolve({ child, port: +m[1] }); else reject(new Error('server said: ' + d));
        });
        child.once('exit', () => reject(new Error('server.py died')));
    });
}

async function main() {
    const { child, port } = await startServer();
    const origin = `http://localhost:${port}`;
    const browser = await chromium.launch();
    let failed = 0, total = 0;
    const check = (name, ok, detail = '') => {
        total++;
        if (ok) console.log(`ok    ${name}`);
        else { failed++; console.error(`FAIL  ${name}${detail ? '\n  ' + detail : ''}`); }
    };
    try {
        const context = await browser.newContext();
        const fetched = [];
        context.on('request', (r) => fetched.push(new URL(r.url()).pathname));
        const page = await context.newPage();
        const run = (src) => page.evaluate((s) => q.eval(s), src);
        const fileFetches = () => fetched.filter((p) => p.startsWith('/files/'));

        await page.goto(origin + '/index.html');
        await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, { timeout: 60000 });
        check('the page boots the engine in a Worker', await page.evaluate(() => typeof createPeachQ === 'undefined'));

        const ls = await run('\\ls');
        check('\\ls lists the examples', EXAMPLES.every((f) => ls.out.includes(`"${f}"`)), JSON.stringify(ls));
        check('... before any file is fetched', fileFetches().length === 0, fileFetches().join(' '));

        const load = await run('\\l trades.q');
        check('\\l trades.q from the start directory', /vwap/.test(load.out) && load.err === '', JSON.stringify(load).slice(0, 300));
        check('... fetched that file and no other', fileFetches().every((p) => p === '/files/trades.q') && fileFetches().length > 0,
              fileFetches().join(' '));
        const n = await run('count trades');
        check('count trades', n.out === '10000', JSON.stringify(n));

        await page.fill('#inp', 'sum 1 2 3');
        await page.press('#inp', 'Enter');
        await page.waitForFunction(() => [...document.querySelectorAll('#out .res')].some((d) => d.textContent === '6'));
        check('the page input evaluates a line', true);

        const hg = await run(`0<count .Q.hg "${origin}/files.json"`);
        check('.Q.hg through the Worker XHR', hg.out === '1b', JSON.stringify(hg));
        const slice = await run(`read1 (\`:${origin}/files/prices.csv;0;4)`);
        check('read1 slice is a Range request, bytes exact', slice.out === '0x64617465', JSON.stringify(slice));
        const refused = await run('.Q.hg "http://localhost:1/"');
        check('an unreachable host answers \'conn', refused.err.startsWith("'conn"), JSON.stringify(refused));

        const busy = page.evaluate(() => q.eval('\\t do[3000000;a:1]'));
        await page.waitForFunction(() => document.getElementById('status').textContent === 'busy');
        const t0 = Date.now();
        const answered = await page.evaluate(() => document.getElementById('status').textContent);
        const lag = Date.now() - t0;
        const ms = +(await busy).out;
        check('the page answers while q computes', answered === 'busy' && lag < 200 && ms > 4 * lag,
              `page answered in ${lag} ms, q ran ${ms} ms`);

        await page.evaluate(() => q.addFiles([{ path: 'more/adverbs.q' }], 'files/'));
        const more = await run('\\ls more');
        check('addFiles layers a host manifest', more.out === '"adverbs.q"', JSON.stringify(more));

        await page.evaluate(() => q.restart());
        const gone = await run('count trades');
        check('restart gives a fresh engine', gone.err.startsWith("'trades"), JSON.stringify(gone));
        const kept = await run('\\ls more');
        check('... and keeps the added files', kept.out === '"adverbs.q"', JSON.stringify(kept));

        await page.evaluate(() => Promise.allSettled([q.restart(), q.restart()]));
        const again = await run('2+3');
        check('a restart during a restart leaves one live engine', again.out === '5' &&
              await page.evaluate(() => q.status) === 'ready', JSON.stringify(again));
    } finally {
        await browser.close();
        child.kill();
    }
    console.log(`${total - failed}/${total} passed`);
    process.exit(failed ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });

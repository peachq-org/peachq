/* The browser check of the wasm build: headless Chromium opens the reference page
 * (index.html, served by server.py) and drives the engine through peachq-client.js and the
 * Worker.  The q is wasm/browser/NN-name.qcmd, replayed in order in ONE session by the native
 * qdoc runner inside the Worker, with ORIGIN bound to this server.  This file keeps what q text
 * cannot say: the network log between ledgers (files listed before any is fetched and fetched
 * only when read; DuckDB's browser build loaded on its first use, from the pinned local copy
 * (make -f Makefile.wasm duckdb-wasm); every request off localhost refused), and the page and
 * client — the input box, responsiveness while q computes, addFiles, restart.
 *
 *   node wasm/browser-smoke.cjs build/wasm/www [/path/to/node_modules]
 *
 * Playwright is resolved from that node_modules, else the usual way (NODE_PATH included);
 * without it this check SKIPS (exit 0). */
'use strict';
const fs = require('fs');
const path = require('path');
const { serve } = require('./serve.cjs');

let chromium;
try {
    ({ chromium } = require(require.resolve('playwright', process.argv[3] ? { paths: [process.argv[3]] } : undefined)));
} catch (_) {
    console.log('SKIP  wasm browser smoke: playwright not found (name a node_modules that has it, or set NODE_PATH)');
    process.exit(0);
}

const www = path.resolve(process.argv[2] || path.join(__dirname, '..', 'build', 'wasm', 'www'));
const LEDGERS = path.join(__dirname, 'browser');

async function main() {
    const { child, origin } = await serve(www, 'browser-smoke');
    const browser = await chromium.launch();
    let failed = 0, total = 0;
    const check = (name, ok, detail = '') => {
        total++;
        if (ok) console.log(`ok    ${name}`);
        else { failed++; console.error(`FAIL  ${name}${detail ? '\n  ' + detail : ''}`); }
    };
    let rows = 0, rowsFailed = 0;
    try {
        const context = await browser.newContext();
        const fetched = [], offsite = [];
        context.on('request', (r) => fetched.push(new URL(r.url()).pathname));
        await context.route(/.*/, (r) => {
            if (new URL(r.request().url()).hostname === 'localhost') return r.continue();
            offsite.push(r.request().url());
            return r.abort();
        });
        const page = await context.newPage();
        const run = (src) => page.evaluate((s) => q.eval(s), src);
        const fileFetches = () => fetched.filter((p) => p.startsWith('/files/'));
        const duckFetches = () => fetched.filter((p) => p.startsWith('/duckdb/'));
        const ledger = async (name) => {
            const text = fs.readFileSync(path.join(LEDGERS, name), 'utf8');
            const n = text.split('\n').filter((l) => /^q[\w.]*\)/.test(l)).length;
            const r = await page.evaluate((t) => q.call({ op: 'qdoc', text: t }), text);
            const bad = r.failed < 0 ? n : r.failed;
            rows += n;
            rowsFailed += bad;
            console.log(`${bad ? 'FAIL' : 'ok  '}  wasm/browser/${name}  ${n - bad}/${n} rows`);
            if (bad) console.error(r.report);
        };

        await page.goto(origin + '/index.html');
        await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, { timeout: 60000 });
        check('the page boots the engine in a Worker', await page.evaluate(() => typeof createPeachQ === 'undefined'));
        await run(`ORIGIN:"${origin}"`);

        await ledger('01-files.qcmd');
        check('... before any file is fetched', fileFetches().length === 0, fileFetches().join(' '));
        await ledger('02-load.qcmd');
        check('\\l fetched that file and no other', fileFetches().every((p) => p === '/files/trades.q') && fileFetches().length > 0,
              fileFetches().join(' '));

        await page.fill('#inp', 'sum 1 2 3');
        await page.press('#inp', 'Enter');
        await page.waitForFunction(() => [...document.querySelectorAll('#out .res')].some((d) => d.textContent === '6'));
        check('the page input evaluates a line', true);

        await ledger('03-http.qcmd');

        const busy = page.evaluate(() => q.eval('\\t do[3000000;a:1]'));
        await page.waitForFunction(() => document.getElementById('status').textContent === 'busy');
        const t0 = Date.now();
        const answered = await page.evaluate(() => document.getElementById('status').textContent);
        const lag = Date.now() - t0;
        const ms = +(await busy).out;
        check('the page answers while q computes', answered === 'busy' && lag < 200 && ms > 4 * lag,
              `page answered in ${lag} ms, q ran ${ms} ms`);

        check('no DuckDB request before its first use', duckFetches().length === 0, duckFetches().join(' '));
        await ledger('04-duckdb.qcmd');
        check('... and DuckDB loaded in the Worker', duckFetches().length > 0);
        const jsonExt = /\/duckdb\/[^/]+\/extensions\/v[\d.]+\/wasm_eh\/json\.duckdb_extension\.wasm$/;
        check('the json extension autoloaded, from the pinned directory', fetched.some((p) => jsonExt.test(p)));
        check('nothing was fetched from off localhost', offsite.length === 0, offsite.join(' '));

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
    console.log(`${rows - rowsFailed}/${rows} ledger rows, ${total - failed}/${total} checks passed`);
    process.exit(failed || rowsFailed ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });

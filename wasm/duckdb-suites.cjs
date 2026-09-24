/* The DuckDB ledgers (test/q/duckdb/*.qcmd) replayed in the browser: each suite runs in a fresh Worker through the
 * native qdoc runner (q_wasm_qdoc), with the parquet fixtures mounted where the suites read them.  A by-hand check
 * of the browser's DuckDB before a release, never part of make q-test: it fails when a suite's failing-row count is
 * not the recorded divergence's (wasm/README.md), and prints every failing row either way.
 *
 *   node wasm/duckdb-suites.cjs build/wasm/www [/path/to/node_modules] [suite ...]
 *
 * Needs Playwright (as browser-smoke.cjs) and DuckDB's browser build (make -f Makefile.wasm duckdb-wasm). */
'use strict';
const fs = require('fs');
const path = require('path');
const { serve } = require('./serve.cjs');

let chromium;
try {
    ({ chromium } = require(require.resolve('playwright', process.argv[3] ? { paths: [process.argv[3]] } : undefined)));
} catch (_) {
    console.log('SKIP  wasm duckdb suites: playwright not found (name a node_modules that has it, or set NODE_PATH)');
    process.exit(0);
}

const repo = path.join(__dirname, '..');
const www = path.resolve(process.argv[2] || path.join(repo, 'build', 'wasm', 'www'));
const ledgers = path.join(repo, 'test', 'q', 'duckdb');
const suites = process.argv.length > 4 ? process.argv.slice(4)
    : fs.readdirSync(ledgers).filter((f) => f.endsWith('.qcmd')).map((f) => f.slice(0, -5)).sort();
const FIXTURES = 'test/data/parquet';
/* The recorded divergences, as failing rows per suite: threads > 1, s3 secrets, the stdlib loaded at start. */
const DIVERGES = { handles: 3, main: 3, pqgate: 2, remote: 10 };

async function main() {
    const manifest = fs.readdirSync(path.join(repo, FIXTURES)).map((n) => ({ path: FIXTURES + '/' + n }));
    const { child, origin } = await serve(www, 'duckdb-suites');
    const browser = await chromium.launch();
    let passed = 0, total = 0, unexpected = 0;
    try {
        for (const s of suites) {
            const text = fs.readFileSync(path.join(ledgers, s + '.qcmd'), 'utf8');
            const rows = text.split('\n').filter((l) => /^q[\w.]*\)/.test(l)).length;
            const context = await browser.newContext();
            await context.route(/.*/, (r) => (new URL(r.request().url()).hostname === 'localhost' ? r.continue() : r.abort()));
            const page = await context.newPage();
            await page.goto(origin + '/index.html');
            await page.waitForFunction(() => document.getElementById('status').textContent === 'ready', null, { timeout: 60000 });
            await page.evaluate(([m, base]) => q.addFiles(m, base), [manifest, origin + '/']);
            await page.evaluate(() => q.eval('\\classic 1'));
            const t0 = Date.now();
            const r = await page.evaluate((x) => q.call({ op: 'qdoc', text: x }), text)
                .catch((e) => ({ failed: -1, report: String(e) }));
            const ok = r.failed < 0 ? 0 : rows - r.failed;
            const expected = r.failed === (DIVERGES[s] || 0);
            passed += ok;
            total += rows;
            unexpected += !expected;
            console.log(`${expected ? 'ok  ' : 'FAIL'}  duckdb/${s}  ${ok}/${rows}` +
                        `${DIVERGES[s] ? ` (${DIVERGES[s]} recorded divergences)` : ''}  ${Date.now() - t0} ms`);
            if (r.failed) console.log(r.report);
            await context.close();
        }
    } finally {
        await browser.close();
        child.kill();
    }
    console.log(`${passed}/${total} rows in the browser; ${unexpected} suite(s) off their recorded count`);
    process.exit(unexpected ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });

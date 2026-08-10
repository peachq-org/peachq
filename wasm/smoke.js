/* The browser build had no test of any kind, which is how it shipped an RE2
 * stub that answered 'regex to every pattern for months unnoticed. */
'use strict';
const path = require('path');

if (parseInt(process.versions.node, 10) < 18) {
    console.error(`node ${process.versions.node} cannot parse the emscripten glue —` +
                  ` source emsdk_env.sh, or pass NODE=/path/to/node`);
    process.exit(1);
}

const CASES = [
    ['2+3', '5'],
    ['sum 1 2 3 4', '10'],
    ['\\l pq', ''],

    ['"abc" rlike "b"', '1b'],
    ['"abc" rlike "z"', '0b'],
    ['"abc" rlike "^a.c$"', '1b'],
    ['`AAPL`MSFT`IBM rlike "^[AM]"', '110b'],

    ['.regexp.matches["abc";"b"]', '1b'],
    ['.regexp.full_match["abc";"ab"]', '0b'],
    ['.regexp.extract["order-1234";"[0-9]+"]', '"1234"'],
    ['.regexp.replace_all["a1b2";"[0-9]";"#"]', '"a#b#"'],
    ['.regexp.escape["a.b"]', '"a\\\\.b"'],
    ['.regexp.version', '"v1.4.5"'],
];

require(path.join(__dirname, 'peachq.js'))().then((M) => {
    if (M.ccall('q_wasm_init', 'number', [], []) !== 0) {
        console.error('FAIL: q_wasm_init');
        process.exit(1);
    }
    const evalq = (src) => {
        const p = M.ccall('q_wasm_eval', 'number', ['string'], [src]);
        const s = M.UTF8ToString(p);
        M.ccall('q_wasm_free', null, ['number'], [p]);
        return s.trim();
    };

    let failed = 0;
    for (const [src, want] of CASES) {
        const got = evalq(src);
        /* Never a pass, whatever a row expects: it is what a build without RE2
         * answers to every pattern. */
        if (got !== want || got === 'error: regex') {
            failed++;
            console.error(`FAIL  ${src}\n  want ${JSON.stringify(want)}\n  got  ${JSON.stringify(got)}`);
        } else {
            console.log(`ok    ${src}  =>  ${got}`);
        }
    }
    console.log(`${CASES.length - failed}/${CASES.length} passed`);
    process.exit(failed ? 1 : 0);
});

/ h.q — openq's `.h` namespace CONSTANTS tier. ALWAYS-ON (kdb defines `.h` at startup),
/ authored from the PUBLISHED qdocs (ref/doth.md, CC BY 4.0). Embedded via build-time
/ codegen (tools/gen-bootstrap.sh -> src/qlang/h_gen.h) and loaded at q_runtime_create
/ AFTER q.q+dotq.q; unconditional (never gated on `l pq`). One definition per line (the
/ loader is line-at-a-time). Keep ZERO backslashes here — codegen bakes lines verbatim into
/ a C string literal, so any backslash (even in a comment) must be a valid C escape.
/ CONSTANT members only — the `.h` FUNCTION members (.h.ht/.h.hu/.h.cd/.h.hn/...) are parked
/ behind the string model (C3) and are deliberately NOT defined here.

/ ---- Markup / web-console constants (ref/doth.md) ----
.h.br:"<br>";
.h.c0:`024C7E;
.h.c1:`958600;
.h.d:" ";
/ .h.logo: kdb ships the KX logo HTML here; openq carries NO KX branding -> empty (divergence).
.h.logo:"";
/ .h.HOME (webserver root): ref/doth.md documents it as the root-path string with no fixed
/ default; openq's landed static server (#217) serves ./html under the process cwd, so we pin
/ the openq-observable value to keep that byte-identical while making it repointable like kdb.
.h.HOME:"html";
/ .h.ty (MIME types, ref/doth.md): symbol(ext)->content-type string. The 7 doc-listed entries
/ take the doc's values; the remainder is #217's C-table superset (web essentials the doc lacks).
/ csv/xml/xls intentionally hold the doc values (differ from the C fallback) — kdb-doc fidelity.
.h.ty:`htm`html`csv`txt`xml`xls`gif`css`js`mjs`png`jpg`jpeg`svg`ico`webp`json`pdf`wasm`woff`woff2!("text/html";"text/html";"text/comma-separated-values";"text/plain";"text/plain";"application/msexcel";"image/gif";"text/css";"application/javascript";"application/javascript";"image/png";"image/jpeg";"image/jpeg";"image/svg+xml";"image/x-icon";"image/webp";"application/json";"application/pdf";"application/wasm";"font/woff";"font/woff2");

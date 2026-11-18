# Integrating qlint: qStudio, VS Code and other tools

How a tool uses peachq's linter, outline and language server. The user-facing description of the linter itself is
`qlint.md`; this page is for the people wiring it into an editor or a pipeline.

Three doors, one engine:

| door | for | protocol |
|---|---|---|
| `q -stdio` | qStudio and other embedders (Java, .NET, Python …) | kdb IPC over stdin/stdout; call `.lint.*` and `.pq.outline` like any query |
| `q -lsp` | VS Code, Neovim, Emacs, Helix, Zed … | Language Server Protocol over stdin/stdout: diagnostics, outline, definition, references, hover, signature help, completion |
| `q -lint` | CI and scripts | command line: text, JSON or SARIF, exit codes |

All three run **no user startup code** under `-lint`/`-lsp` (no `QINIT`, no startup script, no `-eval`; only the
built-in standard library), judge **kdb's grammar** whatever the session's own mode, and never run the code they read
(the one exception: literal-only pieces of pure built-ins such as `6+"hi"`, evaluated under `reval`, which blocks
writes). Platforms: Linux, macOS, Windows 10 1803 or later. `.pq.version` (a string) names the build.

**Preview in this release:** `q -lsp`, the workspace calls `.lint.ws.*` and `.pq.outline` may still change between
releases. Stable from this release: `q -lint` (flags, output formats, exit codes), the rule ids, and `.lint.text`,
`.lint.path`, `.lint.run`, `.lint.rules` and `.lint.explain`.

## qStudio: `q -stdio`

1. **Start** `q -stdio` (or `q.exe -stdio`). Pipe stdin, stdout and stderr. Every normal flag applies; a script
   argument (`q -stdio init.q`) runs before the link is served.
2. **Handshake** (mandatory): write `user:password` (both may be empty), one capability byte, then `0x00`, e.g.
   `"\x03\x00"`. peachq answers one byte, `min(yours, 3)`: that byte is the ready signal.
3. **Framing**: standard kdb IPC. Byte 0 of the 8-byte header is the byte order: `01` little-endian or `00`
   big-endian, both accepted. Replies are always little-endian. msgtype 0 async / 1 sync / 2 response, compressed flag
   (never set on this link), `00`, uint32 total length. One response per sync, none per async, in order; errors come
   back as a `-128h` response. `neg[.z.w]` pushes arrive as async frames between responses. `kx.c` works unchanged.
4. **stdout is frames only.** Everything q prints (`-1`, `show`, `0N!`, notices) goes to stderr: drain stderr on its own
   thread, always, or the child blocks when the pipe buffer fills.
5. **Interrupt**: Linux/macOS `kill -INT <pid>` (`ProcessHandle.pid()`); the running request answers `'stop` and the
   link stays up. Windows: not available yet.
6. **Exit**: close stdin; `.z.exit` runs, pending frames flush, the process exits 0.

### Calls qStudio makes over the link

**Lint an editor buffer**: `.lint.text "<source>"` (a string is always source):

| column | meaning |
|---|---|
| `line col` | 1-based start |
| `endline endcol` | 1-based **inclusive** end |
| `rule` | rule id (below) |
| `severity` | `` `error `` / `` `warning `` / `` `info `` |
| `msg` | message |

Columns are **UTF-8 byte offsets** by default; Java strings are UTF-16, so ask for UTF-16 code units instead with the
`positions` option below.

**Lint files or a folder**: `` .lint.path `:C:/proj/src `` (a symbol is always a path): the same table with a leading
`file` column; honours `lint.config` files. **Options**: `.lint.run[target; opts]`, `opts` mapping rule → `1b`/`0b` or
a severity, e.g. ``.lint.run[src; `UNUSED_PARAM`COND_EVENARGS!(1b;`warning)]``, plus two option keys:

| key | value | effect |
|---|---|---|
| `positions` | `` `utf8 `` (default) or `` `utf16 `` | `col`/`endcol` in bytes, or in UTF-16 code units (a character past U+FFFF counts 2) |
| `globals` | symbol list | names defined elsewhere, which every rule takes as defined (a namespace named there is never judged) |

e.g. ``.lint.run[src; `UNDECLARED_VAR`globals`positions!(1b; `.tst.tstPath`xml.node; `utf16)]``. A rule id this build
does not have is **ignored**, with one notice on stderr, so stored rule choices survive moving between builds. Every file one `.lint.path`/`.lint.run` call lints is **one project**:
a name any of them defines is defined in all, and a namespace any defines into is known, so lint a folder rather than its files one by one.
**Rules**: `.lint.rules[]` → `rule severity on summary`; `` .lint.explain `RULE `` → help text.

**Speed.** Re-lint the whole buffer after a 150–300 ms pause. Time grows with the file's size and with its content
(how many operators and calls it holds, more than its line count): on one desktop, a 680-line process script took
90 ms, 1,070 lines (80 KB) of dense test code 280 ms, and 4,000 one-line functions 740 ms. On large files lengthen the pause, or lint on
save.

**Outline, hover, go-to-definition**: `.pq.outline "<source>"` → `` `file`defs!(fileinfo; defs) ``:

- `fileinfo`: `` `doc`tags `` for the file header.
- `defs`: one row per top-level definition, documented or not:

| column | meaning |
|---|---|
| `name`, `ns` | fully qualified name (`\d` followed as text, from the root), its namespace |
| `kind` | `` `function `` `` `projection `` `` `table `` `` `view `` `` `value `` (from syntax, never by evaluating) |
| `params` | declared parameters, or the implicit `x y z` used |
| `line col endline endcol` | the whole definition (1-based, inclusive end, byte columns) |
| `nameline namecol` | where the name starts (selection range) |
| `doc`, `tags` | the qdoc header ("" when undocumented) and its tags, exactly what `\l` would register |
| `types` | one per parameter: the type its signature checks (`` {[x:`j;y]…} `` gives `` `j` ``), `` ` `` when unchecked |

**Stability**: preview in this release (see the top of this page). Once stable, `.pq.outline`'s columns and its
`kind` values are only ever added, never renamed or removed; ignore a column or kind you do not know. It never evaluates, never follows `\l` and never
touches the help store. For one buffer it is the whole outline; for a project, use the workspace calls below.

### The workspace: `.lint.ws.*`

The same index `q -lsp` answers from, for a tool that keeps its own editor buffers. It holds every file you give it,
its outline rows, the names it uses and its globals; **an update replaces or deletes one file's rows, and a query reads
them and runs nothing**. Positions are **byte offsets** into the file's UTF-8 text (`at`), spans `start`/`end` with an
exclusive end; convert from your editor's character offsets (Java: UTF-16) yourself.

| call | does |
|---|---|
| `` .lint.ws.set[`:C:/proj/a.q; text] `` | an open buffer: its text stands for the file until `.lint.ws.drop` |
| `` .lint.ws.read `:C:/proj/a.q`:C:/proj/b.q `` | files from disk: re-indexed when changed, dropped when gone; open buffers are left alone |
| `` .lint.ws.drop `:C:/proj/a.q `` | delete a file's rows (a closed buffer: call `.lint.ws.read` after it to fall back to disk) |
| `.lint.ws.files[]` | `([] file; open)`: what the index holds |
| `` .lint.ws.text `:a.q `` | the text it holds |
| `` .lint.ws.lint[`:a.q; opts] `` | `.lint.run` of that text with the workspace as its project: every global the index defines is defined |
| `` .lint.ws.symbols `:a.q `` | the outline rows plus `start end` (definition), `at to` (name) and `signature` (``f[x:`j; y]``) |
| `` .lint.ws.defs[`:a.q; at] `` | where the name at `at` is defined: `([] file; start; end; name; kind)`, this file first, then open files, then the rest; a local's parameter or first assignment; nothing for a built-in |
| `` .lint.ws.refs[`:a.q; at] `` | every use: `([] file; start; end; def)`; a local's only inside its own lambda |
| `` .lint.ws.hover[`:a.q; at] `` | markdown: the signature, the doc, the tags; a built-in's help; `""` for nothing |
| `` .lint.ws.sig[`:a.q; at] `` | inside `f[…]`: `` `label`params`docs`doc`active ``, or `::` |
| `` .lint.ws.complete[`:a.q; at] `` | `` `start`end`items`incomplete ``: replace bytes `start` to `end` (the name at the cursor) with an item's `label`; `items` is `([] label; kind; detail; doc; tier)`, those starting with the name up to `at`, at most 500, by `tier` (0 local, parameter or column; 1 this file's; 2 another file's; 3 built-in) then label; none inside a comment or a string; `incomplete` when typing more could offer more (nothing typed yet, or over 500) |

`set`, `read` and `drop` answer the globals whose definitions came or went, so you know which other open buffers to
lint again. While a buffer has a syntax error, a definition its last good text had keeps its last row, so the outline
holds still while a line is typed. A query reads a file it answers from again if the disk changed under it. A name built
from a string (`` value "f 1" ``, `` `f set 1 `` is a definition) is never a reference.

## VS Code and other editors: `q -lsp`

- **Launch**: `q -lsp`, transport stdio; extra arguments are ignored (`--stdio` is harmless). stdout carries protocol
  frames only; drain stderr.
- **initialize** answers `{"capabilities":{"positionEncoding":"utf-8"|"utf-16","textDocumentSync":1,
  "documentSymbolProvider":true,"definitionProvider":true,"referencesProvider":true,"hoverProvider":true,
  "signatureHelpProvider":{"triggerCharacters":["[",";"]},"completionProvider":{"triggerCharacters":["."]},
  "executeCommandProvider":{"commands":["qlint.reindex"]}},
  "serverInfo":{"name":"qlint","version":"<peachq version>"}}`. The encoding is the first of
  `capabilities.general.positionEncodings` that the server speaks, in the client's order, else `utf-16`: list
  `["utf-8"]` first for byte columns. **A VS Code client must offer `utf-16` only, or nothing**:
  `vscode-languageclient` 9 refuses any other answer and the client fails to start.
- **Sync**: full. Send `didOpen` / `didChange` (whole text) / `didClose`.
- **Diagnostics**: pushed `textDocument/publishDiagnostics` after an open or change: `range` 0-based, end
  exclusive; `severity` 1 error / 2 warning / 3 info; `code` = rule id; `source` `"qlint"`; `version` echoed.
  `didClose` → `[]`. Messages waiting together are read together, and an open or change that a later open, change
  or close of the same document follows, with no request between them, is not linted: a burst of changes gets one
  publish, at the newest version
  (200 back-to-back changes of an 80 KB file: one lint, 0.4 s). Publishes stay in order.
- **Config**: for `file:` URIs, the `lint.config` chain from the document's folder up to the nearest folder holding
  `.git`, checked on every change; other schemes (`untitled:`) use defaults. A config that cannot be read, or is a
  directory, is skipped with a stderr notice naming it, and the rest of the chain applies. A config's notices
  (unreadable, a malformed line, a parameter object ignored) print once, and again only when its content changes.
- **Memory** stays level: the server's resident size does not grow with the number of edits or requests (about
  200 bytes per name no earlier edit used, which q never frees), and holds the workspace index (uqf: 75 MB).
- **Lifecycle**: `shutdown` → `null`; `exit` → 0 after shutdown, else 1; stdin EOF → 1. Before `initialize` → -32002,
  after `shutdown` → -32600, unknown → -32601.
- **Workspace.** The folders are `workspaceFolders`, else `rootUri`, else `rootPath`. After `initialized`, every `*.q`
  file under them is indexed from disk in pieces of about 50 ms between messages, so requests are answered meanwhile
  (uqf, 989 files, 11 MB: about 9 s in all, 75 MB resident). A client that offers `window.workDoneProgress` is asked for a token
  (`window/workDoneProgress/create`) and sees `$/progress` begin, report and end. Open documents are authoritative: an
  unsaved edit changes every answer at once. A client that offers
  `workspace.didChangeWatchedFiles.dynamicRegistration` is asked (`client/registerCapability`) to watch `**/*.q`;
  peachq watches no files itself. A watched create or change reads the file again from disk and a delete drops it;
  an event for a file an editor holds open is ignored. Without watcher events, a request still reads again a file it
  answers from if it changed on disk; the command `qlint.reindex` (`workspace/executeCommand`) walks the folders again.
- **Left out by default**, from startup indexing and watched events, the same folders qStudio skips: any part of the
  path below the workspace folder matching `^\..*|^target$|^node_modules$|^__pycache__$|^site-packages$|^venv$|^build$|^dist$|^__MACOSX$|.*-ve$`
  (a name starting with `.`, such as `.git` or `.venv`; `target`, `node_modules`, `__pycache__`, `site-packages`,
  `venv`, `build`, `dist`, `__MACOSX`; a name ending `-ve`). A left-out folder is never listed. A file you open is
  linted wherever it is, a left-out folder included; `.gitignore` is not read.
- **initializationOptions**: `{"exclude": ["lib/*", "*/vendor/*"]}`: q `like` patterns matched against the path
  relative to its workspace folder, left out as well as the folders above. A folder's trailing `/` is ignored. A filesystem root (`/`, `C:/`) as a folder is never walked at startup (its files count as
  workspace files when opened or watched).
- **documentSymbol**: hierarchical. Each `\d` namespace is a Namespace (3) holding its definitions; function and
  projection are Function (12), table Struct (23), view Property (7), value Variable (13). `range` is the whole
  definition, `selectionRange` its name, `detail` the signature. During a syntax error the last good outline answers.
- **definition**: a global's definitions (an undotted name under `\d .ns` is `.ns.name`, never the root's, as q reads it), this
  document first, then open documents, then the workspace; a local's parameter or first assignment; `[]` for a
  built-in. **references**: a global's uses everywhere, a local's in its own lambda; `includeDeclaration` honoured.
- **hover** (markdown): ``f[x:`j; y]``, the qdoc text, a line per tag; a built-in's `.help` text.
- **signatureHelp** inside `f[` (triggers `[` and `;`): the signature, each parameter with its `@param` text, the
  active one; built-ins from their rank. **completion** (trigger `.`): the lambda's parameters and locals, the
  workspace's definitions (a `\d` namespace's by short name too), keywords, `.ns.` members (the workspace's and the
  built-in `.q .Q .z .h .j` and peachq namespaces'), and a table literal's columns inside `select … from t`; never an
  operator glyph, and nothing inside a comment or a string. Each item carries a `textEdit` replacing the name typed so
  far and a `sortText` that orders it by scope: locals first, then this file's names, the workspace's, the built-ins.
  `isIncomplete` is true while nothing is typed or more than 500 match. Nothing is ever evaluated. TorQ's `wdb.q` (40 KB): about
  35 ms per request, end to end.
- **Cross-file diagnostics**: when a change adds or removes a global, every other open document is linted again
  (UNDEFINED_REFERENCE, UNDECLARED_VAR and the rules that resolve a name read the whole project).
- **Not offered**: rename, workspace symbols, code actions, pull diagnostics, incremental sync.

Minimal VS Code client: `vscode-languageclient`'s `LanguageClient` with `serverOptions = { command: "q", args:
["-lsp"] }` and `documentSelector: [{ language: "q" }]`. Turn off any other q linter to avoid duplicate findings.

## CI: `q -lint`

`q -lint <paths…> [-format text|json|sarif]`. Exit 0 clean, 1 if any error-severity finding, 2 if any input is
unreadable. Text is `file:line:col: severity RULE msg` plus a summary line. SARIF 2.1.0 (UTF-16 columns, exclusive
ends) is what GitHub code scanning reads. `-help` prints usage. Rules are switched only through `lint.config`.

## Rules (`.lint.rules[]`)

| rule | severity | default |
|---|---|---|
| SYNTAX, ASSIGN_RESERVED_WORD, LITERAL_ERROR, COND_EVENARGS, TOO_MANY_ARGUMENTS, PARAM_TYPE_MISMATCH, TYPED_PARAM_ERROR | error | on |
| UNDEFINED_REFERENCE | error | off |
| UNUSED_VAR, DEPRECATED_DATETIME, MALFORMED_SUPPRESSION | warning | on |
| ENGINE_UNSUPPORTED | info | on |
| NOT_FILE_SYMBOL, TIME_OF_DAY_COMPARE, COND_TWO_ARG_IF, NESTED_LAMBDA_OUTER_READ, WHERE_SELF_COMPARE, DUPLICATE_COLUMN, TRAP_EAGER_CALL, COND_SAME_BRANCHES, EMPTY_IF | warning | on |
| UNUSED_PARAM | info | off |
| UNDECLARED_VAR | warning | off |

Ids are KX QLint's where one exists, PARAM_TYPE_MISMATCH and UNDEFINED_REFERENCE are qStudio's; TYPED_PARAM_ERROR and the hazard ids are peachq's. A shipped id is
never renamed or reused; new rules are added, so map unknown ids to a generic marker. `ENGINE_UNSUPPORTED` is valid q
that peachq's parser cannot read yet (`'nyi`): not your bug, and its statement goes unchecked; it never fails `q -lint`.

## Configuration and suppression

- `lint.config` (KX format), one rule per line: `RULE : true|false[, severity]`. A file applies to its folder and every
  folder below; the nearest file that sets a rule wins; the search stops at the folder holding `.git`. Parameter
  objects (`{…}`) are read and ignored with a notice.
- In source: `/ @qlintsuppress RULE` (whole file), `RULE(n)` (this line and the next n), `all` (every rule); `/` or
  `//`, with or without a space. One rule per comment; a comment naming no rule, a lower-case id, a list (`A B`,
  `A, B`) or a bad count is a `MALFORMED_SUPPRESSION` warning at the comment rather than a silent no-op.

## Which features need which libraries

`q` (`q.exe`) alone runs the language, the standard library, IPC, HTTP, `-stdio`, `-lsp`, `-lint`, `.lint.*` and
`.pq.outline`: none of them loads a library. Optional features load one on first use, looked for by these rules
(Windows: `duckdb.dll` etc. beside `q.exe` is found):

| feature | library | where peachq looks |
|---|---|---|
| DuckDB: `.duckdb.*`, `s)` SQL, `.parquet.*`, `` `:pq:duckdb: `` handles | `duckdb.dll` / `libduckdb.so` / `libduckdb.dylib` (x86-64 needs AVX2) | `PEACHQ_DUCKDB_LIB` alone if set, else `$QHOME`, beside `q`, then the system path |
| TLS: `https://` (`.Q.hg`/`.Q.hp`, HTTP client, S3), TLS IPC, `-26!`, `.crypto` digests | OpenSSL `libssl` + `libcrypto` (3.x or 1.1; on Windows `libssl-3-x64.dll`, `libcrypto-3-x64.dll`) | the system's library search (on Windows that includes `q.exe`'s folder) |
| Kafka: `.kafka.*` | `librdkafka` 2.0 or later (`librdkafka.dll`) | `PEACHQ_KAFKA_LIB` if set, else `$QHOME`, beside `q`, then the system path |
| `2:`, `.ffi` | whatever library you name | the path you give |

peachq itself never loads libcurl; a `libcurl.dll` beside `q.exe` is not needed by anything above. A static Linux build
cannot load libraries at all, and says so when a feature needs one.


# peachq

An open-source **implementation of the q programming language**, MIT-licensed,
built on **rayforce** - a pure C17, zero-dependency columnar engine by Anton
Kundenko ([credits below](#credits-standing-on-rayforce)).

It aims for close behavioural compatibility with q, measured the only way that
means anything: q-observable behaviour - `parse` display, type shorts, error
text, and console formatting.

[Website](https://peachq.org) ·
[Try online](https://peachq.org/repl) ·
[Download](https://peachq.org/download) ·
[Compatibility](https://peachq.org/compatibility) ·
[Roadmap](https://peachq.org/roadmap)


> [!IMPORTANT]
> PeachQ is useful but still early. It is not yet a drop-in replacement for
> production kdb+/q systems. Check the compatibility dashboard and known
> limitations before evaluating it for a workload.


## A session

Real output from `./q`:

```q
q)kt:([s:`ibm`msft] px:187.5 402.1)     / keyed table literal
q)kt[`msft]
px| 402.1
q)select avg px by s from ([]s:`a`b`a`b;px:1.5 2.5 3.5 4.5)
s| px
-| ---
a| 2.5
b| 3.5
q)2024.01.15D10:30:00 + 0D01:15:00      / timestamp + timespan
2024.01.15D11:45:00.000000000
q){x*x} each til 5                       / lambdas, adverbs
0 1 4 9 16
q)count each group `ibm`msft`ibm
ibm | 2
msft| 1
q)(+/)1 2 3 4                            / derived verbs are first-class values
10
q)parse "2+3"                            / kdb-identical parse display
(+;2;3)
q)"D"$"2024.01.15"
2024.01.15
q)-8!42                                  / kdb wire serialization
0x0100000011000000f92a00000000000000
q)meta ([]a:1 2;b:`x`y)
c| t f a
-| -----
a| j
b| s
```

## Download & run

Prebuilt `q` binaries are on the [latest release](https://github.com/peachq-org/peachq/releases/latest).

**Linux** (static, runs on any distro):

```bash
curl -fsSL https://github.com/peachq-org/peachq/releases/latest/download/peachq-linux-x86_64.tar.gz | tar xz
./q
```

**macOS** (Apple Silicon):

```bash
curl -fsSL -o peachq.tar.gz https://github.com/peachq-org/peachq/releases/latest/download/peachq-macos-arm64.tar.gz
tar xzf peachq.tar.gz
xattr -d com.apple.quarantine ./q   # clear the macOS Gatekeeper quarantine
./q
```

**Windows**: download `peachq-windows-x86_64.zip` from the [latest release](https://github.com/peachq-org/peachq/releases/latest), extract it, and run `q.exe`.

## Build from source

```bash
make            # optimized build: ./q
make rayforce   # optional: the underlying rayfall engine REPL
make debug      # unoptimized + sanitizers
./q             # q REPL;  printf '2+3\n' | ./q  for piped mode
./q -p 5000     # kdb-wire server - point qStudio or a kdb client at it
```

## What works today

- **Datatypes:** all q atom/vector types including the full temporal family
  (date, time, timestamp, timespan, month, minute, second, datetime), byte, guid.
  Type tags are kdb's numbers; `type` and `meta` report kdb's chars.
- **Core language:** verbs and adverbs (`/ \ ' \: /:`), lambdas `{x+y}`,
  projections, composition, derived verbs as first-class 104h values, dicts,
  tables, keyed tables (literal, `!`/`0!`, `xkey`, row lookup, upsert).
- **qSQL:** `select` (where / by / aggregates) and functional `?`/`!` core;
  `exec`/`update`/`delete` partial.
- **IPC:** speaks the kdb wire - `-8!`/`-9!`, `./q -p PORT` server,
  `hopen`/`hclose` client; qStudio connects and runs queries.
- **Platforms:** Linux, macOS, Windows; browser via WebAssembly.

## How it's built

1. **No q interpreter.** The parser reads q and builds engine `ray_t` trees
   directly with registry function values embedded at verb heads. One lowering
   pass turns q-only shapes into pure array applications; the engine's
   evaluator/DAG does all execution.
2. **One manifest for all verbs.** Every q verb is a single row in the op
   manifest - lexer and runtime registry both derive from it.
3. **q datatypes are native.** Types the engine lacked (the temporal family,
   byte, guid) were added to its own type system, and its type tags ARE kdb's
   type numbers.

## Credits: standing on rayforce

peachq exists because of **rayforce** and one person in particular:
**Anton Kundenko**. Anton built the columnar engine at
the heart of this project - the dataframe core, the vector kernels, the DAG
evaluator - and it is genuinely a work of art. Fast, lean, zero-dependency C that
makes q's whole model *possible*. peachq is a q language layer riding on top of
that engine; the hard, beautiful part underneath is his.

Huge thanks to Anton, and to the rest of the rayforce crew - Serhii Savchuk,
Hetoku, and everyone whose commits shaped the engine. The bulk of this codebase
is theirs, and the commit history preserves every one of them verbatim
(`git shortlog -sne`).

The q layer on top - parser, q datatypes, lowering pass, registry, formatter,
and kdb-wire IPC - was written by Ryan Hamilton. Full attribution: [AUTHORS](AUTHORS).

## Contributing

This repo is a **read-only release mirror** of a private development repo, so
pull requests here can't be merged - but bug reports and ideas are very welcome
via [Issues](../../issues). See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Portions Apache-2.0 and MIT - see [NOTICE](NOTICE) and
[AUTHORS](AUTHORS).

## Trademarks

*q* and *kdb+* are trademarks of KX Systems, Inc. peachq is an independent,
community project and is **not** affiliated with, endorsed by, or sponsored by
KX Systems. Any references to kdb+/q describe interoperability and behavioural
compatibility only.

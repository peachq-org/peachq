# peachq errors

peachq signals the errors on the [kx errors page](https://code.kx.com/q/basics/errors/) with the same text. This page
lists the ones kx q does not have: what raises each, and what to do about it. Each heading is a link target, so
`errors#badfunc` goes straight to its entry.

## badfunc

A function received over IPC, or read by `-9!`, has source text that is not a single lambda. For example, a client
sent `{x~)}`, which does not parse, or `{x}[…]`, which applies the lambda. Only a lambda definition is accepted, so
nothing the sender wrote runs while the message is being read.

The connection stays open: a synchronous request gets `'badfunc` back, and an asynchronous one is dropped. kx q calls
its nearest error `bad lambda`. Fix the function on the sending side.

## corrupt

A kdb+ data file (a splayed column, its companion files, a compressed block) is truncated or inconsistent with its
own header, or a file given to `-11!` does not start with a log header. Re-copy or regenerate the file.

## csv

A CSV load failed: a malformed file, a cell that does not read as its column's fixed type, a zero-byte file, or a
ragged row with no lever set. [Bad rows](bad-rows.md) lists the levers that let a load continue past bad rows.

## duckdb

A request made through `.duckdb`, `.parquet` or a `.duckdb` handle failed, either in DuckDB itself (an SQL error, a
missing table) or in peachq's bridge to it (a value with no DuckDB type, a name it cannot address). `.duckdb.err[]`
returns the reason for the last failure (`.duckdb.err[h]` for one connection).

## format

`.str.format` could not apply its template, for example because a `{}` field has no matching argument or a format
spec is invalid. Check the template against the argument list.

## index

An assignment addressed a position that does not exist, for example `a[10]:1` or `a[0 99]:7 7` when `a` has 10
items. Check the index against `count`; to add items, use `,`.

## io

A file or socket operation failed: a path that cannot be opened or written, a port that cannot be listened on, a
write to a closed handle. kx q usually reports these as `path: OS error text`. Check the path, its permissions, and
whether the port is free.

## oom

peachq could not allocate memory for the operation. kx q reports the same situation as `'wsfull`. Work on smaller
pieces, or free memory in the session.

## option

A `.csv` or `.j` loader received an option key it does not recognise, one it recognises but does not yet implement,
or an unknown format specifier (for example `%Z`, since there is no timezone database). See
[Loading](loading.md) and [Bad rows](bad-rows.md).

## printf

`.str.printf` could not apply its format string, for example because a `%` conversion has no matching argument or
the wrong kind of argument. Check the conversions against the argument list.

## regex

A `.regexp` function or `rlike` was given a pattern that RE2 cannot compile (unbalanced parentheses, or a
backreference, which RE2 does not support), or a replacement it cannot apply. See [Regular expressions](lib/regexp.md).

## static-dlopen

The static (musl) build of peachq cannot load shared libraries, so `2:`, `.ffi` and TLS (which loads OpenSSL) are
unavailable. The message reads `static-dlopen: this build cannot load shared libraries - use a -glibc download: `
followed by a link to this entry.
Install the `-glibc` download for your platform instead; it behaves the same in every other way.

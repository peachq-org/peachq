# Reading zip archives

peachq reads the entries of a zip archive in place, without unpacking it first. It works like DuckDB's
`SELECT * FROM 'zip://data.zip/trades.csv'`: a `zip://` path names one entry. Every file reader accepts that path,
and the `.zip` namespace lists what an archive holds.

This is a peachq extension. kx q has no zip reader. In kx you would unpack to a file, or stream through a fifo with
`unzip -p` (see [Named pipes](https://code.kx.com/q/kb/named-pipes/)); peachq does that streaming for you.

Every function and parameter: [.zip API](https://peachq.org/docs/api/zip.q.html).

## The zip:// path

```q
q)`:t.csv 0: ("sym,px";"a,1.5";"b,2.5");
q)system"zip -q data.zip t.csv";
q)read0 `:zip://data.zip/t.csv
"sym,px"
"a,1.5"
"b,2.5"
q)select from `:zip://data.zip/t.csv
| sym | px    |
|     | float |
|-----|-------|
| "a" | 1.5   |
| "b" | 2.5   |
```

The archive is everything up to and including the first `.zip/`, in any case (`.ZIP/` works too). The entry is
everything after that, matched exactly, case included, against the names `.zip.info` lists: `` `:zip://data.zip/dir/t.csv ``.
An absolute archive path keeps its slash: `` `:zip:///data/archive.zip/t.csv ``.

| Reader | On a `zip://` entry |
|---|---|
| `read1`, `read0` | the entry's bytes or lines, as for a file, including the `(file;offset;length)` forms |
| `0:` (Load CSV, Load Fixed) | the entry's rows |
| `hcount` | the entry's uncompressed size, from the archive's directory |
| `get`, `select from` | the format the entry's suffix names (`.csv`, `.json`, …), as for a file |
| `.csv.read`, `.j.read` | the entry, read once |
| `hopen` | a handle that reads the entry forward: `read1 (h;n)` gives the next `n` bytes, and the empty vector at the end |
| `.Q.fs`, `.Q.fsn`, `.Q.fps` | the entry's lines, streamed |

**An entry streams forward.** DEFLATE data cannot seek, so `.Q.fs` over a `zip://` entry reads it once, front to
back, through a handle. A large entry costs one read buffer of memory, not the entry. A ranged `read1` such as
`` read1 (`:zip://data.zip/big.csv;1000000;100) `` is answered correctly, but it decompresses everything before
the offset each time it is called. Loop with `.Q.fs` or a handle, never with offsets.

`.csv.read` and `.j.read` decompress the entry into memory once and read it from there.

**A read that reaches the end of an entry checks it.** The decompressed size, the number of compressed bytes used
and the CRC-32 must all match the archive's directory record, or the read signals `'corrupt`. The check runs before
the last bytes are handed over, so a whole read never returns wrong data. A stream such as `.Q.fs` or a handle
delivers the bytes before the end as they arrive, and still signals `'corrupt` when it reaches the end. A ranged read
that stops before the end does not check the CRC.

**`zip://` is read-only.** Writing to one, with `0:`, `1:`, `set` or a handle from `hopen`, signals `'nyi`.

## The two functions

`.zip` is part of the standard library, and loads the first time you use one of its names.

### .zip.info

`.zip.info[file;opts]` lists the archive's entries in the order its directory holds them:

```q
q).zip.info[`:data.zip;()!()]
| name    | size | csize | method | crc        | modified                      |
|         | long | long  | symbol | long       | timestamp                     |
|---------|------|-------|--------|------------|-------------------------------|
| "t.csv" | 19   | 19    | stored | 4106140718 | 2026.11.13D19:23:56.000000000 |
```

| Column | |
|---|---|
| `name` | the entry's name, a string; a directory's ends in `/` |
| `size` | uncompressed bytes, as `hcount` of its `zip://` path gives |
| `csize` | the bytes the archive stores |
| `method` | `` `stored `` or `` `deflate ``, which peachq reads; otherwise the method's name (`` `bzip2 ``, `` `zstd ``, …) |
| `crc` | the CRC-32 the archive records |
| `modified` | the entry's time stamp, in the local time zip wrote it |

### .zip.read

`.zip.read[file;entry;opts]` returns one entry's bytes. It is `read1` of the entry's `zip://` path, so the two give
the same answer. Use `"c"$` to get text:

```q
q)"c"$.zip.read[`:data.zip;"t.csv";()!()]
"sym,px\na,1.5\nb,2.5\n"
```

`file` is a file symbol or a string. `entry` is a string or a symbol.

Neither function has any options yet: `opts` is `::` or a dict, and any key signals `'option`.

## Errors

| Error | When |
|---|---|
| `'io` | the archive is missing or cannot be read |
| `'domain` | the archive has no such entry (`'domain. data.zip has no entry x.csv`), or the path has no `.zip/` or no entry after it |
| `'corrupt` | the file is not a zip archive, its directory is damaged, or an entry's data does not match its record |
| `'nyi` | an encrypted entry, a method other than stored and deflate, a multi-disk archive, or a write |

## What is supported

Archives written by any zip tool: stored and DEFLATE entries, zip64 sizes and offsets past 4 GiB, and entries whose
sizes follow their data in a data descriptor (what `zip` writes to a pipe). If a name appears twice, the first
directory record wins.

Not read: encryption, the bzip2, LZMA, zstd and other methods, and archives split over several disks. Not yet: `\l` of
a script inside an archive, and `get` of a serialized q object or a parquet file stored in one (a q object there is
`'io`).

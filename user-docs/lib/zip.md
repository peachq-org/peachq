# Reading archives and gzip files

peachq reads the entries of a zip or tar archive in place, without unpacking it first, and decompresses a `.gz` file
when it becomes a table. You name an entry by writing its path through the archive, as if the archive were a folder:
`` `:data.zip/trades.csv ``. Every file reader accepts that path, over every transport, so
`` `:s3://bucket/data.zip/trades.csv `` and `` `:https://host/data.tar.gz/trades.csv `` work too. The `.zip`
namespace lists what a zip archive holds.

This is a peachq extension. kx q has no archive reader. In kx you would unpack to a file, or stream through a fifo
with `unzip -p` (see [Named pipes](https://code.kx.com/q/kb/named-pipes/)); peachq does that streaming for you.

Every function and parameter: [.zip API](https://peachq.org/docs/api/zip.q.html).

## An archive in a path

```q
q)`:t.csv 0: ("sym,px";"a,1.5";"b,2.5");
q)system"zip -q data.zip t.csv";
q)read0 `:data.zip/t.csv
"sym,px"
"a,1.5"
"b,2.5"
q)select from `:data.zip/t.csv
| sym | px    |
|     | float |
|-----|-------|
| "a" | 1.5   |
| "b" | 2.5   |
```

**Archives are detected, never spelled as a scheme.** A path's scheme names only its transport: a local file,
`s3://`, `https://`. A component whose name ends `.zip`, `.tar`, `.tar.gz` or `.tgz` (in any case) is an archive when
it is a FILE, and is walked into: the rest of the path is the entry, matched exactly, case included, against the
names the archive holds (`` `:data.zip/dir/t.csv ``). When such a component is a DIRECTORY, it is an ordinary folder.
Finding out costs one `stat` on a local path, or one `HEAD` request on S3 or HTTPS, and only for components named like
an archive.

The separator is `/`. `#` and `!` cannot separate an archive from its entry, because either ends a q symbol
literal: `` `:a.zip#x `` parses as take.

An archive inside an archive is walked into the same way: `` `:outer.zip/inner.zip/t.csv ``. Each read of the inner
archive decompresses the outer entry from its start, so keep nesting for small archives.

| Reader | On an archive entry |
|---|---|
| `read1`, `read0` | the entry's bytes or lines, as for a file, including the `(file;offset;length)` forms |
| `0:` (Load CSV, Load Fixed) | the entry's rows |
| `hcount` | the entry's uncompressed size |
| `get`, `select from` | the format the entry's suffix names (`.csv`, `.json`, `.csv.gz`, …), as for a file |
| `.csv.read`, `.j.read` | the entry: `.csv.read` streams it, `.j.read` reads the document whole |
| `hopen` | a handle that reads the entry forward: `read1 (h;n)` gives the next `n` bytes, and the empty vector at the end |
| `.Q.fs`, `.Q.fsn`, `.Q.fps` | the entry's lines, streamed |

**`select from` an archive reads its one entry.** `` select from `:one.zip `` is `select from` the archive's only
file. An archive holding several files signals `'type` and names them, so you can pick one:

```q
q)select from `:three.zip
'type. three.zip holds 3 entries: t.csv u.csv v.json
q)select from `:three.zip/u.csv
```

**An entry streams forward.** Compressed data cannot seek, so `.Q.fs` over an entry reads it once, front to back,
through a handle. A large entry costs one read buffer of memory, not the entry. A ranged `read1` such as
`` read1 (`:data.zip/big.csv;1000000;100) `` is answered correctly, but it decompresses everything before the offset
each time it is called. Loop with `.Q.fs` or a handle, never with offsets. A tar archive has no index, so finding an
entry reads the headers in front of it; in a `.tar.gz` that means decompressing everything in front of it.

**A read that reaches the end of a zip entry checks it.** The decompressed size, the number of compressed bytes used
and the CRC-32 must all match the archive's directory record, or the read signals `'corrupt`. The check runs before
the last bytes are handed over, so a whole read never returns wrong data. A stream such as `.Q.fs` or a handle
delivers the bytes before the end as they arrive, and still signals `'corrupt` when it reaches the end. A ranged read
that stops before the end does not check the CRC. A tar entry is checked by its header's checksum, and a gzip member
by its CRC-32 and length.

**Archives are read-only.** Writing to an entry, with `0:`, `1:`, `set` or a handle from `hopen`, signals `'nyi`.

## gzip files

A table door decompresses a file whose name ends `.gz`: `select from`, `get` of a text file, `.csv.read` and `.j.read`.
This follows DuckDB's `compression='auto'`. `.csv.read` and `.j.read` take a `` `compression `` option to say otherwise:
`` `auto `` (the default), `` `gzip `` or `` `none ``.

```q
q)select from `:trades.csv.gz
q).csv.read[`:trades.dat;::;::;enlist[`compression]!enlist`gzip]
```

`read0` and `read1` return the bytes as stored, as kdb does: `` 2#read1 `:trades.csv.gz `` is `0x1f8b`. A `.gz`
made of several gzip members, as `cat a.gz b.gz > c.gz` makes, is ONE file, and decompresses to the whole
concatenation. A gzip file is decompressed as it is read: a local one costs a read buffer, not the file, while an
`s3://` or `https://` one is fetched whole first and then decompressed as it is read.

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
| `size` | uncompressed bytes, as `hcount` of the entry's path gives |
| `csize` | the bytes the archive stores |
| `method` | `` `stored `` or `` `deflate ``, which peachq reads; otherwise the method's name (`` `bzip2 ``, `` `zstd ``, …) |
| `crc` | the CRC-32 the archive records |
| `modified` | the entry's time stamp, in the local time zip wrote it |

### .zip.read

`.zip.read[file;entry;opts]` returns one entry's bytes. It is `read1` of the entry's path, so the two give the same
answer. Use `"c"$` to get text:

```q
q)"c"$.zip.read[`:data.zip;"t.csv";()!()]
"sym,px\na,1.5\nb,2.5\n"
```

`file` is a file symbol or a string, and its name ends `.zip`. `entry` is a string or a symbol.

Neither function has any options yet: `opts` is `::` or a dict, and any key signals `'option`.

## Errors

| Error | When |
|---|---|
| `'io` | the archive is missing or cannot be read |
| `'domain` | the archive has no such entry (`'domain. data.zip has no entry x.csv`) |
| `'type` | `select from` an archive that holds more than one file |
| `'corrupt` | the file is not an archive, its directory or a header is damaged, an entry's data does not match its record, or a gzip member will not decompress |
| `'nyi` | an encrypted zip entry, a method other than stored and deflate, a multi-disk archive, or a write |

## What is supported

Zip archives written by any zip tool: stored and DEFLATE entries, zip64 sizes and offsets past 4 GiB, and entries
whose sizes follow their data in a data descriptor (what `zip` writes to a pipe). If a name appears twice, the first
directory record wins. Tar archives in the POSIX ustar layout, with GNU long names and pax `path` records, plain or
gzip-compressed.

`get` of a serialized q object stored in an archive reads it, as for a file.

Not read: encryption, the bzip2, LZMA, zstd and other methods, and archives split over several disks. Not yet: `\l` of
a script inside an archive, and `get` of a parquet file stored in one.

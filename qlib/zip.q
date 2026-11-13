/ Read zip archives: list what an archive holds, and read one entry's bytes.
/ The reading itself is the `zip://` path every file reader takes - read0, read1, 0:, hcount, hopen, .Q.fs and
/ .csv.read all accept `:zip://archive.zip/inner/path; these two functions are a thin door over it.
/ .
/ @eg
/ .zip.info[`:data.zip;()!()]
/ "c"$.zip.read[`:data.zip;"trades.csv";()!()]
/ select from `:zip://data.zip/trades.csv
/ .Q.fs[{0N!count x}]`:zip://data.zip/trades.csv

.pq.load_natives`zip;

/ The archive's entries, one row each, in central-directory order.
/ @param file the archive, `:data.zip or "data.zip"
/ @param opts :: or a dict; no options yet, so any key signals 'option
/ @return a table name (string) size (long, uncompressed bytes) csize (long, stored bytes) method (symbol: `stored,
/ `deflate, or the APPNOTE name of a method peachq cannot read) crc (long, the CRC-32) modified (timestamp, the
/ entry's DOS stamp, local time)
/ @throws io the archive is missing or unreadable
/ @throws corrupt the file is not a zip archive, or its directory is damaged
/ @throws nyi a multi-disk archive
/ @eg .zip.info[`:data.zip;()!()]
.zip.info:{[file;opts] .zip.i.opts opts; .zip.i.info file};

/ One entry's bytes, the uncompressed content as read1 gives it; "c"$ makes it text.
/ @param file the archive, `:data.zip or "data.zip"
/ @param entry the entry's name as .zip.info lists it, a string or a symbol ("dir/t.csv")
/ @param opts :: or a dict; no options yet, so any key signals 'option
/ @return a byte vector
/ @throws domain the archive has no such entry
/ @throws corrupt the entry's data, size or CRC-32 does not match its directory record
/ @throws nyi an encrypted entry, or a method other than stored and deflate
/ @eg "c"$.zip.read[`:data.zip;"trades.csv";()!()]
.zip.read:{[file;entry;opts] .zip.i.opts opts;
  read1 `$":zip://",(1_string hsym $[10h=type file;`$file;file]),"/",$[10h=type entry;entry;string entry]};

/ opts is :: or a dict, and no key is known yet
.zip.i.opts:{[opts] if[not (::)~opts; if[99h<>type opts; '`type]; if[count opts; '`option]]};

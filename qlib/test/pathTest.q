/ .path conformance: lexical path structure, and never storage.  Every assertion here is pure text
/ manipulation — no test creates, reads or removes a file, and no path below needs to exist.
/ Sources: the peachq .path/.fs safe-v1 design notes, and pathlib's parent/name/stem/suffix rules.
/ Errors are asserted by CLASS through assertThrows, never by pinning a stack trace.
system "d .pathTest";

testPathAcceptsEveryInputForm:{
    .qunit.assertEquals[.path.path "foo/bar.csv"; `:foo/bar.csv; "a string becomes a path"];
    .qunit.assertEquals[.path.path `foo/bar.csv; `:foo/bar.csv; "a plain symbol becomes a path"];
    .qunit.assertEquals[.path.path `:foo/bar.csv; `:foo/bar.csv; "a path is already a path — conversion is idempotent"];
    .qunit.assertEquals[.path.path "a"; `:a; "a one-character string is a path, not a char atom"];
    .qunit.assertEquals[.path.path "c"; `:c; "a char atom is a path too"];
    .qunit.assertEquals[type .path.path "foo/bar.csv"; -11h; "the canonical value is a symbol atom"]};

testStringDropsTheLeadingColon:{
    .qunit.assertEquals[.path.string `:foo/bar.csv; "foo/bar.csv"; "string answers the text without q's `: marker"];
    .qunit.assertEquals[.path.string "foo/bar.csv"; "foo/bar.csv"; "a string round-trips through string unchanged"];
    .qunit.assertEquals[.path.string `:/data/quotes.csv; "/data/quotes.csv"; "an absolute path keeps its leading slash"];
    .qunit.assertEquals[type .path.string `:foo/bar.csv; 10h; "string answers a string"]};

testRelativePath:{
    .qunit.assertEquals[.path.parent `:data/trades/quotes.csv; `:data/trades; "parent drops the final component"];
    .qunit.assertEquals[.path.name `:data/trades/quotes.csv; `quotes.csv; "name is the final component, as a symbol"];
    .qunit.assertEquals[.path.stem `:data/trades/quotes.csv; `quotes; "stem is the final component without its suffix"];
    .qunit.assertEquals[.path.suffix `:data/trades/quotes.csv; `.csv; "suffix keeps its leading dot, as pathlib spells it"];
    .qunit.assertEquals[.path.join (`:data;`trades;`quotes.csv); `:data/trades/quotes.csv; "join builds the same path back"]};

testAbsolutePath:{
    .qunit.assertEquals[.path.path "/data/trades/quotes.csv"; `:/data/trades/quotes.csv; "an absolute string converts"];
    .qunit.assertEquals[.path.parent `:/data/trades/quotes.csv; `:/data/trades; "parent stays absolute"];
    .qunit.assertEquals[.path.name `:/data/trades/quotes.csv; `quotes.csv; "name ignores the leading slash"];
    .qunit.assertEquals[.path.stem `:/data/trades/quotes.csv; `quotes; "stem ignores the leading slash"];
    .qunit.assertEquals[.path.suffix `:/data/trades/quotes.csv; `.csv; "suffix ignores the leading slash"];
    .qunit.assertEquals[.path.join (`:/data;`trades;`quotes.csv); `:/data/trades/quotes.csv; "join keeps the first component absolute"]};

/ THE Windows row: a drive letter puts a colon at position 1, which is why no host:port heuristic may exist.
testWindowsDrivePathSurvivesEveryFunction:{
    .qunit.assertEquals[.path.path `:c:/temp/x.csv; `:c:/temp/x.csv; "a drive path converts unharmed"];
    .qunit.assertEquals[.path.path "c:/temp/x.csv"; `:c:/temp/x.csv; "a drive path converts from a string too"];
    .qunit.assertEquals[.path.string `:c:/temp/x.csv; "c:/temp/x.csv"; "string keeps the drive letter and its colon"];
    .qunit.assertEquals[.path.parent `:c:/temp/x.csv; `:c:/temp; "parent keeps the drive"];
    .qunit.assertEquals[.path.name `:c:/temp/x.csv; `x.csv; "name is unaffected by the drive"];
    .qunit.assertEquals[.path.stem `:c:/temp/x.csv; `x; "stem is unaffected by the drive"];
    .qunit.assertEquals[.path.suffix `:c:/temp/x.csv; `.csv; "suffix is unaffected by the drive"];
    .qunit.assertEquals[.path.join (`:c:/temp;`x.csv); `:c:/temp/x.csv; "join rebuilds the drive path"]};

/ The BARE drive root reads by the POSIX rules this file applies everywhere: "c:/" is a directory named
/ "c:" carrying a trailing slash.  Telling a drive from a directory needs a <letter>: heuristic, which is
/ the very shape rule 2 forbids, so v1 pins this reading rather than special-casing the separator.
testBareDriveRootReadsAsADirectoryName:{
    / `c: cannot be WRITTEN as a symbol literal — a trailing colon ends the token — so it is built.
    .qunit.assertEquals[.path.string `:c:/; "c:/"; "the text is carried through untouched"];
    .qunit.assertEquals[.path.name `:c:/; `$"c:"; "the drive letter reads as the final component"];
    .qunit.assertEquals[.path.parent `:c:/; `:.; "and so it parents as a bare name, not as a root"]};

/ The deliberate cost of the "two or more characters before ://" rule, which exists so a Windows drive
/ letter can never read as a scheme.  A one-character scheme is legal on paper and absent in practice; a
/ one-character drive is universal, so the trade goes to Windows.  These rows pin the side we chose.
testOneCharacterSchemeIsAPath:{
    .qunit.assertEquals[.path.path "c://temp"; `$":c://temp"; "a drive with a doubled slash stays a path"];
    .qunit.assertEquals[.path.path "x://host/file"; `$":x://host/file"; "so a one-character scheme is NOT refused"];
    .qunit.assertThrows[.path.path; "xy://host/file"; "domain*"; "two characters before :// is a scheme again"]};

/ Only ONE leading ":" is q's path marker, because a POSIX filename may itself begin with a colon.
testLeadingColonIsNotStrippedTwice:{
    .qunit.assertEquals[.path.path ":foo"; `:foo; "one colon is the marker and is consumed"];
    .qunit.assertEquals[.path.name `$"::foo"; `:foo; "the second colon belongs to the filename"];
    .qunit.assertEquals[.path.path "::https://e/a"; `$"::https://e/a"; "so this is a path named \":https:\", not a URL"]};

/ THE rule-2 row: this is what fails if someone later adds a host:port heuristic.  `:localhost:5000 is
/ indistinguishable from a relative path to a folder named "localhost:5000", which is a legal POSIX name.
testHostPortIsTreatedAsAPath:{
    .qunit.assertEquals[.path.path `:localhost:5000; `:localhost:5000; "a host:port shape is a PATH, never rejected"];
    .qunit.assertEquals[.path.string `:localhost:5000; "localhost:5000"; "its text is the whole component"];
    .qunit.assertEquals[.path.name `:localhost:5000; `localhost:5000; "the colon is part of the name"];
    .qunit.assertEquals[.path.suffix `:localhost:5000; `; "a colon does not make a suffix"];
    .qunit.assertEquals[.path.parent `:localhost:5000; `:.; "it parents as a bare name does"]};

/ A `:pq: handle is a different KIND of object: the trailing slash alone distinguishes the table form from
/ the connection form, so .path.parent would silently convert one into the other.
testProviderHandlesAreRefused:{
    .qunit.assertThrows[.path.path; `:pq:duckdb:mkt:/data/market.duckdb:trade/; "domain*"; "the table form is refused"];
    .qunit.assertThrows[.path.path; `:pq:duckdb:mkt:/data/market.duckdb; "domain*"; "the connection form is refused"];
    .qunit.assertThrows[.path.parent; `:pq:duckdb:mkt:/data/market.duckdb:trade/; "domain*"; "parent cannot strip the trailing slash"];
    .qunit.assertThrows[.path.name; `:pq:duckdb:mkt:cfg:trade/; "domain*"; "name is refused rather than guessed"];
    .qunit.assertThrows[.path.suffix; `:pq:duckdb:mkt:/data/market.duckdb; "domain*"; "suffix cannot read .duckdb out of a config"];
    .qunit.assertThrows[.path.string; `:pq:duckdb:mkt:cfg; "domain*"; "every function routes through the one gate"];
    .qunit.assertEquals[.path.path "pq"; `:pq; "a bare pq names a directory, and the scheme needs a provider after it"];
    .qunit.assertEquals[.path.path `pq; `:pq; "the symbol form the same way"];
    .qunit.assertEquals[.path.name `:build/pq; `pq; "so a real path can end in one"]};

testSchemeUrlsAreRefused:{
    .qunit.assertThrows[.path.path; `:s3://bucket/k.csv; "domain*"; "an object-store URL is not a lexical path"];
    .qunit.assertThrows[.path.path; `:tcps://h:5010; "domain*"; "an IPC handle is not a lexical path"];
    .qunit.assertThrows[.path.parent; `:s3://bucket/k.csv; "domain*"; "parent is refused too"];
    .qunit.assertThrows[.path.name; `:tcps://h:5010; "domain*"; "name is refused too"];
    .qunit.assertThrows[.path.path; "https://example.com/a.csv"; "domain*"; "the gate reads a string argument the same way"]};

testNonPathValuesAreRefused:{
    .qunit.assertThrows[.path.path; 42; "type*"; "a long is not path-like"];
    .qunit.assertThrows[.path.join; `:a; "type*"; "join wants a list of components, not one path"];
    .qunit.assertThrows[.path.join; (); "length*"; "join needs at least one component"]};

testEdgeShapes:{
    .qunit.assertEquals[.path.suffix `:data/README; `; "no dot means no suffix"];
    .qunit.assertEquals[.path.stem `:data/README; `README; "a suffixless name is all stem"];
    .qunit.assertEquals[.path.suffix `:data/archive.tar.gz; `.gz; "only the LAST dot makes the suffix"];
    .qunit.assertEquals[.path.stem `:data/archive.tar.gz; `archive.tar; "the earlier dots stay in the stem"];
    .qunit.assertEquals[.path.suffix `:data/.bashrc; `; "a leading dot is not a suffix — pathlib's rule"];
    .qunit.assertEquals[.path.stem `:data/.bashrc; `.bashrc; "a dotfile is all stem"];
    .qunit.assertEquals[.path.suffix `:data/x.; `; "a trailing dot is not a suffix either"];
    .qunit.assertEquals[.path.name `:data/trades/; `trades; "a trailing slash does not make an empty name"];
    .qunit.assertEquals[.path.parent `:data/trades/; `:data; "a trailing slash does not shift the parent"];
    .qunit.assertEquals[.path.name `:x.csv; `x.csv; "a bare filename is its own name"];
    .qunit.assertEquals[.path.parent `:x.csv; `:.; "a bare filename parents to `:. as pathlib does"]};

testRoot:{
    .qunit.assertEquals[.path.path "/"; `:/; "the root converts"];
    .qunit.assertEquals[.path.string `:/; enlist "/"; "the root is one slash — a one-character STRING, not the char atom"];
    .qunit.assertEquals[.path.parent `:/; `:/; "the root is its own parent"];
    .qunit.assertEquals[.path.name `:/; `; "the root has no name"];
    .qunit.assertEquals[.path.stem `:/; `; "the root has no stem"];
    .qunit.assertEquals[.path.suffix `:/; `; "the root has no suffix"];
    .qunit.assertEquals[.path.parent `:/data; `:/; "a top-level entry parents to the root"];
    .qunit.assertEquals[.path.join (`:/;`x); `:/x; "joining onto the root does not double the slash"]};

/ Every function except join maps over a list of paths, elementwise.
testVectorisation:{
    .qunit.assertEquals[.path.path ("a/b.csv";`:c/d.txt); `:a/b.csv`:c/d.txt; "a mixed list converts elementwise"];
    .qunit.assertEquals[.path.path `:a/b.csv`:c/d.txt; `:a/b.csv`:c/d.txt; "a symbol vector converts elementwise"];
    .qunit.assertEquals[.path.string `:a/b.csv`:c/d.txt; ("a/b.csv";"c/d.txt"); "string answers one string per path"];
    .qunit.assertEquals[.path.parent `:a/b.csv`:c/d.txt; `:a`:c; "parent maps"];
    .qunit.assertEquals[.path.name `:a/b.csv`:c/d.txt; `b.csv`d.txt; "name maps"];
    .qunit.assertEquals[.path.stem `:a/b.csv`:c/d.txt; `b`d; "stem maps"];
    .qunit.assertEquals[.path.suffix `:a/b.csv`:c/d.txt; `.csv`.txt; "suffix maps"];
    .qunit.assertEquals[.path.name (); (); "an empty general list maps to an empty general list"];
    .qunit.assertThrows[.path.name; (`:a/b.csv;`:s3://bucket/k.csv); "domain*"; "one bad element fails the whole vector"]};

/ join's list argument is the component SEQUENCE, never a vector of paths to map over.
testJoin:{
    .qunit.assertEquals[.path.join (`:data;`trades;`quotes.csv); `:data/trades/quotes.csv; "symbols join"];
    .qunit.assertEquals[.path.join ("data";"trades"); `:data/trades; "strings join"];
    .qunit.assertEquals[.path.join (`:data;"trades";`quotes.csv); `:data/trades/quotes.csv; "mixed forms join"];
    .qunit.assertEquals[.path.join `:data`trades; `:data/trades; "a symbol vector is a component sequence"];
    .qunit.assertEquals[.path.join enlist `:data; `:data; "one component joins to itself"];
    .qunit.assertEquals[.path.join enlist `:/; `:/; "one component keeps the root — its slash is the path, not a separator"];
    .qunit.assertEquals[.path.join enlist `:c:/; `:c:/; "one component keeps a drive root the same way"];
    .qunit.assertEquals[.path.join (`:data/;`trades); `:data/trades; "a trailing slash does not double up"];
    .qunit.assertEquals[.path.join (`:data;`;`trades); `:data/trades; "an empty component collapses away"];
    .qunit.assertEquals[.path.join (.path.parent `:data/quotes.csv; .path.name `:data/quotes.csv); `:data/quotes.csv; "parent and name rejoin"];
    .qunit.assertThrows[.path.join; (`:data;`:s3://bucket); "domain*"; "join gates every component"]};

/ .path is LEXICAL: it must answer for paths that do not exist, and must never consult storage.
testLexicalOnly:{
    .qunit.assertEquals[.path.parent `:no/such/dir/file.csv; `:no/such/dir; "a nonexistent path still has a parent"];
    .qunit.assertEquals[.path.suffix `:no/such/file.csv; `.csv; "a nonexistent path still has a suffix"];
    / `:~/… cannot be WRITTEN as a symbol literal — ~ ends the token — so the expected value is built.
    .qunit.assertEquals[.path.path "~/notes.txt"; `$":~/notes.txt"; "~ is left alone — no expansion"];
    .qunit.assertEquals[.path.name `$":~/notes.txt"; `notes.txt; "~ is an ordinary component"];
    .qunit.assertEquals[.path.path "a/../b"; `:a/../b; "..  is left alone — no normalisation"];
    .qunit.assertEquals[.path.name `:a/../b; `b; ".. is an ordinary component"];
    .qunit.assertEquals[.path.parent `:a/../b; `:a/..; "parent does not resolve .."];
    .qunit.assertEquals[.path.path "b.csv"; `:b.csv; "a relative path is never made absolute"];
    .qunit.assertEquals[.path.parent `:; `:.; "the EMPTY path is relative, so it parents to `:. and never to the root"];
    .qunit.assertEquals[.path.parent `$""; `:.; "a null symbol parents the same way"]};

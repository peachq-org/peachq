/ .duckdb conformance: the type-contract projection .duckdb.types — the C QD_TYPES[] table surfaced as
/ data.  A pure projection: no test here needs the DuckDB library, a connection, or a file, so the suite
/ runs wherever the stdlib loads.  Rows are pinned by CONTENT (the append-only contract rows), never by
/ position or total count, so contract additions extend the table without touching these tests.
system "d .duckdbTest";

testShape:{
    t:.duckdb.types[];
    .qunit.assertEquals[cols t; `dtype`ktype`logical`canon; "the four contract columns, in order"];
    .qunit.assertEquals[`char$exec t from meta t; "scsb"; "sym, char, sym, boolean columns"];
    .qunit.assertTrue[0<count t; "the table carries the contract rows"]};

testKnownRows:{
    t:.duckdb.types[];
    .qunit.assertEquals[exec first ktype from t where dtype=`BIGINT,canon; "j"; "BIGINT reads as long"];
    .qunit.assertEquals[exec first ktype from t where dtype=`VARCHAR,canon; "C"; "canonical VARCHAR is the string column"];
    .qunit.assertEquals[exec first canon from t where logical=`symbol; 0b; "the symbol row is a non-canon VARCHAR rider"]};

/ the derivation consumers will use: canon rows keyed by dtype must be a FUNCTION (one row per dtype),
/ so `first` picks nothing arbitrary and the dict answers deterministically
testCanonDerivation:{
    c:select from .duckdb.types[] where canon;
    .qunit.assertEquals[count c; count distinct c`dtype; "one canon row per dtype — first is well-defined"];
    d:(c`dtype)!c`ktype;
    .qunit.assertEquals[d`BIGINT; "j"; "the derived dtype->char dict answers"];
    .qunit.assertEquals[d`TIMESTAMP_NS; "p"; "and the temporal rows ride along"]};

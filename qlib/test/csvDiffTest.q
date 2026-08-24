/ DuckDB differential harness for .csv.read (csv-read PR-2).  Every fixture file is loaded twice  - 
/ once through .csv.read, once through DuckDB's own read_csv over the .duckdb bridge - and compared
/ cell-for-cell, order preserved, no float tolerance.  DuckDB is the conformance oracle for PARSING;
/ peachq's type law is deliberately its own, so a difference passes ONLY through a named divergence
/ class the fixture explicitly allows:
/   sym     explicit-sym q columns (types "s") where DuckDB gives VARCHAR (string-of-sym vs string  -
/           sniffed text is ALWAYS a string column since the always-C ruling, so sym never auto-fires)
/   narrow  integer-width typing differences (compared with both sides widened to long)
/   nullnan null-vs-NaN representation (null masks and non-null values compared separately)
/   overtolerant  (owner-granted 2026-08-23) pq refuses ('csv/'io) where a LIVE DuckDB succeeds via its
/           irreproducible lenient value-truncation (the end_quote class).  We error; we do not chase
/           duck's truncation quirks.  Covers ONLY that direction - never a duck refusal, never a
/           value/shape mismatch, never a dead oracle.  Granted per fixture; the granting PR records
/           each fixture's justification.
/   bytes   (owner-granted 2026-08-23) the MIRROR of overtolerant: a LIVE DuckDB rejects the file where
/           pq loads it byte-transparently.  Chars ARE bytes (string-C3), so invalid-UTF-8 and latin-1
/           payloads survive the round trip; we add no validator whose only job is to fail.  Same
/           narrowness: that direction only, never a value/shape mismatch, never a dead oracle.
/   clock   (owner-granted 2026-08-23) our time LATTICE - hh:mm is minute (u), hh:mm:ss[.f] is second
/           (v) or timespan (n) - against DuckDB's single TIME.  Both sides compare as timespan, so the
/           CLASS may differ but the instant may not; an unequal instant stays red under it.
/   emptyfile (owner-granted 2026-08-23) a ZERO-BYTE file: DuckDB invents a one-column VARCHAR schema
/           from nothing, we say 'csv.  We never invent a schema where the file states none.  Narrower
/           than overtolerant, which it otherwise resembles: the file must actually be zero bytes.
/   blankline (owner-granted 2026-08-23) a blank physical line is a row of nulls to DuckDB and nothing
/           to us.  We keep the skip.  The grant removes from the DuckDB side exactly the rows whose
/           every cell is null-or-empty, and only where that count equals the file's own blank-line
/           count - so a dropped DATA row can never ride it; everything else compares as usual.
/ The vocabulary is CLOSED - new classes are owner-granted, never invented here.  Since the endgame
/ sweep (2026-08-23) this suite is ALL GREEN and stays so: a fixture that would need a new class is
/ not carried red here - it either moves to csvTest.q as a pq-only pin of the current behaviour, or
/ leaves the corpus as deliberately undefined for this release.
/ types_match reports the RAW type agreement so an allowed divergence stays visible in the table.
/ Every fixture is ONE self-contained test function calling .csvDiffTest.chk - re-running a fixture
/ is calling its test function.  chk appends the row to .csvDiffTest.Results and the full table
/ prints when the namespace finishes.  Vendored fixture tests are named EXACTLY after the upstream
/ file, test_<basename sans .csv> with non-identifier chars folded to _ (test_sample_0 for
/ sample-0.csv) - a deliberate break from camelCase: traceability to the corpus wins.  Provenance:
/ qlib/test/fixtures/csv/README.md.  Hand-fixture tests write their own bytes (1: is byte-exact and
/ returns the file symbol) into the run cwd under a csvdiff_ prefix, swept after the namespace.
/ No DuckDB on the resolution ladder means the oracle calls error and the fixtures fail: intentional.

/ reload-safe: a reload between the namespace hooks must not orphan an open connection
.csvDiffTest.Duck:@[value;`.csvDiffTest.Duck;0N];
.csvDiffTest.DuckChars:@[value;`.csvDiffTest.DuckChars;(`$())!""];
.csvDiffTest.Results:();
.csvDiffTest.NOOPT:()!();
.csvDiffTest.VDIR:"fixtures/csv/duckdb/";

.csvDiffTest.i.tc:{[t] exec t from meta t};

/ ONE spelling of the DuckDB dialect options, derived from the SAME opts dict .csv.read receives  - 
/ hand-written SQL fragments would drift; the empty-sym padding key is ignored, matching .csv.read's law
.csvDiffTest.i.dopt1:{[opts;k]
    v:opts k;
    $[k in `delim`quote`escape`comment;", ",string[k],"='",(raze {$[x="'";"''";enlist x]} each (),v),"'";
      k in `header`ignore_errors`null_padding`strict_mode`store_rejects;", ",string[k],"=",$[v;"true";"false"];
      k=`sample_size;", sample_size=",string v;
      k in `dateformat`timestampformat;", ",string[k],"='",v,"'";
      'domain]};
.csvDiffTest.i.dopts:{[opts]
    raze .csvDiffTest.i.dopt1[opts;] each (key opts) except `};

.csvDiffTest.i.sql:{[f] "SELECT * FROM read_csv('",(f`file),"'",(.csvDiffTest.i.dopts f`opts),")"};

/ verdict for one column pair: `ok exact, the class that bridges it, or `red.  Known lossiness INSIDE
/ the sym class: q's null sym strings to "" and the bridge hands DuckDB's VARCHAR NULL and '' both as
/ "", so empty-vs-null text is invisible under sym - inherent to sym-promotion, recorded, not silent
/ zero rows = zero observable parse output: the row counts already matched, both payloads are bare (),
/ and pq's read result carries no type declaration to compare - vacuously ok, types_match stays raw
.csvDiffTest.i.cmp_col:{[cp;tp;cd;td]
    $[0=count cp; `ok;
      (tp=td) and cp~cd; `ok;
      (tp="s") and td="C"; $[all (string each cp)~'cd; `sym; `red];
      (tp in "hij") and td in "hij"; $[(`long$cp)~`long$cd; `narrow; `red];
      (tp in "uvn") and td="t"; $[(`timespan$cp)~`timespan$cd; `clock; `red];
      (tp=td) and tp in "hijef"; $[((null cp)~null cd) and (cp where not null cp)~cd where not null cd; `nullnan; `red];
      `red]};

.csvDiffTest.i.cmp_idx:{[pcols;ptc;dcols;dtc;i] .csvDiffTest.i.cmp_col[pcols i;ptc i;dcols i;dtc i]};

/ a zero-row exec result loses column types (every column is a bare ()), so ask DESCRIBE - the same
/ bridge, just SQL - for the declared types instead of guessing.  The dtype->char dict is DERIVED from
/ .duckdb.types (the C QD_TYPES[] contract table): canon rows, one per dtype - the selection
/ duckdbTest.q pins as well-defined.  An unknown spelling stays loud ("?" can never match).
.csvDiffTest.i.duck_char:{[s]
    d:.csvDiffTest.DuckChars;
    $[(`$s) in key d;d `$s;"?"]};
.csvDiffTest.i.duck_tc:{[f]
    t:.duckdb.i.exec[.csvDiffTest.Duck;"DESCRIBE SELECT * FROM read_csv('",(f`file),"'",(.csvDiffTest.i.dopts f`opts),")"];
    .csvDiffTest.i.duck_char each t`column_type};

.csvDiffTest.i.mk_row:{[cm;km;tm;ec;hm;al;p]
    `count_match`cols_match`types_match`error_count`hash_match`allow`pass!(cm;km;tm;`long$ec;hm;al;p)};

/ each engine's invented names, for n columns: pq x x1 x2...; duck column0 column1..., the index
/ zero-padded to the width of the last one (column00...column17 at 18 columns)
.csvDiffTest.i.gen_pq:{[n] `$"x",/:(enlist ""),string 1+til n-1};
.csvDiffTest.i.gen_dk:{[n] w:count string n-1; `$"column",/:{[w;s] ((w-count s)#"0"),s}[w] each string til n};

/ the rows a blank physical line produces on the DuckDB side: every cell null (a typed column) or empty
/ (a text one).  A text column arrives as a list of char vectors, where `null` would answer per CHARACTER
.csvDiffTest.i.blank_rows:{[t]
    where all each flip {$[0h=type x; 0=count each x; null x]} each value flip t};

/ compare two loaded tables; answers the count/cols/types/error/hash/pass tail of a result row.
/ A position where BOTH engines emit their OWN invented name (headerless file, or a blank header
/ cell) is engine convention, not parse output, so it compares positionally - keyed off the names
/ themselves, never off opts; a generated name against a real one stays a mismatch
.csvDiffTest.i.cmp_tables:{[fixture;pq_t;dk_t;allow_txt]
    / the blankline grant, applied before anything is measured: it must remove EXACTLY the file's blank
    / lines, so a duck row count that outruns them leaves the divergence in place and red
    if[(`blankline in fixture`allow) and (count dk_t)>count pq_t;
        b:.csvDiffTest.i.blank_rows dk_t;
        if[(count b)=sum 0=count each read0 hsym `$fixture`file; dk_t:dk_t (til count dk_t) except b]];
    rows_ok:(count pq_t)=count dk_t;
    pqn:cols pq_t; dkn:cols dk_t;
    cols_ok:$[(count pqn)=count dkn;
        all (pqn=dkn) or (pqn=.csvDiffTest.i.gen_pq count pqn) and dkn=.csvDiffTest.i.gen_dk count dkn;
        0b];
    pq_tc:.csvDiffTest.i.tc pq_t;
    dk_tc:$[0=count dk_t;@[.csvDiffTest.i.duck_tc;fixture;{[e] "?"}];.csvDiffTest.i.tc dk_t];
    shape_ok:(count pq_tc)=count dk_tc;
    verdicts:$[rows_ok and shape_ok;
        .csvDiffTest.i.cmp_idx[value flip pq_t;pq_tc;value flip dk_t;dk_tc] each til count pq_tc;
        (count pq_tc)#`red];
    bad:sum not verdicts in `ok,fixture`allow;
    .csvDiffTest.i.mk_row[rows_ok;cols_ok;shape_ok and pq_tc~dk_tc;bad+sum not rows_ok,cols_ok,shape_ok;
        rows_ok and shape_ok and 0=bad;allow_txt;cols_ok and rows_ok and shape_ok and 0=bad]};

/ every DuckDB-side error is the bare 'duckdb (house law: no message strings), so a file refusal and
/ a dead bridge look identical - a sentinel query tells them apart before a dual refusal may pass
.csvDiffTest.i.duck_alive:{@[{.duckdb.i.exec[.csvDiffTest.Duck;"SELECT 1"];1b};::;{[e] 0b}]};

/ run one fixture; both loads are trapped - both sides refusing the FILE is agreement (pq must say
/ 'csv or 'io and the oracle must still answer a sentinel); one side refusing is red unless the fixture
/ carries the grant for ITS direction - overtolerant when pq refuses, bytes when duck does
.csvDiffTest.i.diff_fixture:{[fixture]
    pq_r:@[{(1b;.csv.read[`$":",x`file;::;x`types;x`opts])};fixture;{[e] (0b;e)}];
    dk_r:@[{(1b;.duckdb.i.exec[.csvDiffTest.Duck;.csvDiffTest.i.sql x])};fixture;{[e] (0b;e)}];
    note:{[side;r] $[first r;"";" ",side,":'",$[48<count e:last r;48#e;e]]};
    dd:.csvDiffTest.i.dopts fixture`opts;
    args:(-3!fixture`opts),$[(::)~fixture`types;"";" types:",-3!fixture`types],$[count dd;" duck:",dd;""],
        note["pq";pq_r],note["duck";dk_r];
    allow_txt:" " sv string each fixture`allow;
    base:`name`file`args!(fixture`name;fixture`file;args);
    if[not[first pq_r] and not first dk_r;
        ok:((last pq_r) in ("csv";"io")) and .csvDiffTest.i.duck_alive[];
        :base,.csvDiffTest.i.mk_row[ok;ok;ok;not ok;ok;allow_txt;ok]];
    if[(first pq_r)<>first dk_r;
        / overtolerant covers EXACTLY pq-refuses-while-a-live-duck-loads, bytes exactly the mirror;
        / the live-oracle probe is load-bearing on both - a dead bridge must never satisfy either
        ot:(`overtolerant in fixture`allow) and not[first pq_r] and ((last pq_r) in ("csv";"io"))
            and .csvDiffTest.i.duck_alive[];
        by:(`bytes in fixture`allow) and first[pq_r] and not[first dk_r] and .csvDiffTest.i.duck_alive[];
        / emptyfile is overtolerant's shape with one more condition, and that condition is the grant
        ef:(`emptyfile in fixture`allow) and not[first pq_r] and ((last pq_r) in ("csv";"io"))
            and (0=count @[read1;hsym `$fixture`file;""]) and .csvDiffTest.i.duck_alive[];
        :base,.csvDiffTest.i.mk_row[0b;0b;0b;1;0b;allow_txt;ot or by or ef]];
    base,.csvDiffTest.i.cmp_tables[fixture;last pq_r;last dk_r;allow_txt]};

/ the per-fixture entry: one file, both loads, one results row, one assertion.  file is a file SYMBOL
/ (a hand fixture - 1:'s return) or a bare STRING name resolved to the vendored tree.  chk never takes
/ explicit types - that is the gating tests' door onto the wider internal.
.csvDiffTest.chk:{[file;opts;allow]
    path:$[-11h=type file;1_string file;.csvDiffTest.VDIR,file];
    fx:`name`file`opts`types`allow!(`$last "/" vs path;path;opts;::;(),allow);
    r:.csvDiffTest.i.diff_fixture fx;
    .csvDiffTest.Results,:enlist r;
    .qunit.assertTrue[r`pass;"differential agrees for ",path," (see the results table below)"]};

.csvDiffTest.i.as_table:{[rs]
    ks:key first rs;
    flip ks!{[rs;k] rs@\:k}[rs] each ks};

/ threads=1: the differential is one tiny file per query, so DuckDB's per-query thread fan-out is pure
/ overhead here (~3x on read_csv).  The canon dtype->char dict is derived ONCE per run - same
/ .duckdb.types selection, hoisted out of the per-column path.
.csvDiffTest.beforeNamespaceDiff:{
    .csvDiffTest.Duck::@[.duckdb.i.open;`$":default:";{[e] show "csvDiffTest: duckdb unavailable ('",e,")"; 0N}];
    @[{.duckdb.i.exec[.csvDiffTest.Duck;"SET threads=1"]};::;::];
    .csvDiffTest.DuckChars::@[{c:select from .duckdb.types[] where canon; (c`dtype)!c`ktype};::;{[e] (`$())!""}];
    .csvDiffTest.Results::()};

.csvDiffTest.afterNamespaceDiff:{
    if[count .csvDiffTest.Results; .csvDiffTest.Results::.csvDiffTest.i.as_table .csvDiffTest.Results];
    show .csvDiffTest.Results;
    fs:key `:.;
    {@[hdel;hsym x;::]} each fs where (string each fs) like "csvdiff_*";
    @[.duckdb.i.close;.csvDiffTest.Duck;::];
    .csvDiffTest.Duck::0N};

/ THE ONE NAMED DIAGNOSIS: a dead oracle reds every fixture identically, so this row alone states the
/ real cause - the open verdict, the probe error and the lib path the resolution ladder was given.
/ (qunit runs tests in name order, so it is not literally first to execute; it is the row to read.)
.csvDiffTest.test_duckdb_present:{
    lib:getenv `PEACHQ_DUCKDB_LIB;
    err:@[{.duckdb.i.exec[.csvDiffTest.Duck;"SELECT 1"];""};::;{[e] e}];
    .qunit.assertTrue[(not null .csvDiffTest.Duck) and ""~err;
        "DuckDB oracle is dead: open=",(string not null .csvDiffTest.Duck),
        $[""~err;"";" probe='",err,"'"],
        " PEACHQ_DUCKDB_LIB=",$[0=count lib;"(unset - is the lib beside the MAIN checkout's third_party/?)";lib]]};

/ ---- harness mechanics: the compare and the allow gate, proven on designed fixtures ------------

/ gating fixtures ride the wider internal directly - explicit types, no Results row
.csvDiffTest.i.gate:{[file;types;allow]
    .csvDiffTest.i.diff_fixture `name`file`opts`types`allow!(`gate;file;.csvDiffTest.NOOPT;types;(),allow)};

/ sniffed text is always a string column now, so the sym divergence occurs ONLY through the explicit-sym
/ workflow: the fixture passes types "s" for the text column and the DuckDB side stays VARCHAR naturally
.csvDiffTest.testGatingSymNeedsAllowance:{
    `:csvdiff_mech.csv 1: "a,b\n1,x\n2,y\n3,x\n";
    ty:(enlist `b)!enlist "s";
    r0:.csvDiffTest.i.gate["csvdiff_mech.csv";ty;0#`];
    r1:.csvDiffTest.i.gate["csvdiff_mech.csv";ty;`sym];
    .qunit.assertEquals[r0`pass;0b;"an uncovered sym difference is red without the allowance"];
    .qunit.assertEquals[r0`error_count;1;"and counts as exactly one uncovered difference"];
    .qunit.assertEquals[r1`pass;1b;"the same fixture passes once sym is allowed"];
    .qunit.assertEquals[r1`types_match;0b;"raw type divergence stays visible even when allowed"];
    .qunit.assertEquals[r1`hash_match;1b;"and the values agree under the sym bridge"]};

/ the both-sides-generated law: a headerless file passes positionally on the invented names, but real
/ header names still compare exactly - pq naming a REAL header x/x1 by chance cannot ride the law alone
.csvDiffTest.testGatingGeneratedNamesComparePositionally:{
    `:csvdiff_gen.csv 1: "1,2\n3,4\n";
    r:.csvDiffTest.i.gate["csvdiff_gen.csv";::;0#`];
    .qunit.assertEquals[r`cols_match;1b;"x/x1 vs column0/column1 is engine convention, compared positionally"];
    .qunit.assertEquals[r`pass;1b;"so a headerless file agrees on its values alone"];
    fx:`name`file`opts`types`allow!(`gate;"";.csvDiffTest.NOOPT;::;0#`);
    r2:.csvDiffTest.i.cmp_tables[fx;([]x:1 2;x1:3 4);([]a:1 2;b:3 4);""];
    .qunit.assertEquals[r2`cols_match;0b;"generated names on one side against real names on the other stays a mismatch"]};

/ the dateformat option is ONE spelling on both sides: pq parses with it and the SQL carries it, so a
/ day-first file - text by the never-guess law without it - agrees as dates under it
.csvDiffTest.testGatingDateformatBothSides:{
    `:csvdiff_dfmt.csv 1: "d\n13/10/2021\n04/10/2021\n";
    fx:`name`file`opts`types`allow!(`gate;"csvdiff_dfmt.csv";(enlist `dateformat)!enlist "%d/%m/%Y";::;0#`);
    r:.csvDiffTest.i.diff_fixture fx;
    .qunit.assertEquals[r`pass;1b;"both sides parse day-first dates under the explicit format"];
    .qunit.assertEquals[r`types_match;1b;"and as the same type"]};

/ blank_line.csv is a GENUINE structural divergence (DuckDB emits a NULL row where pq skips the blank
/ line), so even the full allowance list cannot cover it - opts are the single source for both sides,
/ so a dialect split cannot be hand-forced
.csvDiffTest.testGatingClassesCannotCoverStructure:{
    r:.csvDiffTest.i.gate[.csvDiffTest.VDIR,"blank_line.csv";::;`sym`narrow`nullnan];
    .qunit.assertEquals[r`count_match;0b;"the sides disagree structurally and it is seen"];
    .qunit.assertEquals[r`pass;0b;"and no allowance list can cover a structural difference"]};

.csvDiffTest.testGatingErrorAsymmetryIsRed:{
    `:csvdiff_empty.csv 1: "";
    r:.csvDiffTest.i.gate["csvdiff_empty.csv";::;`sym`narrow`nullnan];
    .qunit.assertEquals[r`pass;0b;"one side refusing the file is a difference no class covers"];
    .qunit.assertEquals[r`error_count;1;"scored as one difference"]};

.csvDiffTest.testGatingBothRefusalAgrees:{
    r:.csvDiffTest.i.gate["csvdiff_no_such_file.csv";::;0#`];
    .qunit.assertEquals[r`pass;1b;"both sides refusing the file is agreement"]};

.csvDiffTest.testGatingDeadBridgeNotAgreement:{
    live:.csvDiffTest.Duck;
    .csvDiffTest.Duck::0N;
    r:.csvDiffTest.i.gate["csvdiff_no_such_file.csv";::;0#`];
    .csvDiffTest.Duck::live;
    .qunit.assertEquals[r`pass;0b;"a dead oracle never turns a dual failure into agreement"]};

/ the overtolerant grant covers exactly one shape: pq refuses 'csv/'io while a LIVE duck loads (its
/ irreproducible truncation leniency).  Any other asymmetry or difference stays red under it.
.csvDiffTest.testGatingOvertolerantCoversPqRefusalOnly:{
    `:csvdiff_ot.csv 1: "a,b\n\"x\"y,1\n";
    r0:.csvDiffTest.i.gate["csvdiff_ot.csv";::;0#`];
    r1:.csvDiffTest.i.gate["csvdiff_ot.csv";::;`overtolerant];
    .qunit.assertEquals[r0`pass;0b;"pq refusing where duck loads is red without the grant"];
    .qunit.assertEquals[r1`pass;1b;"and passes under it"];
    .qunit.assertEquals[(r1`count_match;r1`error_count);(0b;1);"with the divergence still visible in the row"];
    `:csvdiff_otrev.csv 1: "a\nx\377z\n";
    r2:.csvDiffTest.i.gate["csvdiff_otrev.csv";::;`overtolerant];
    .qunit.assertEquals[r2`pass;0b;"the REVERSE asymmetry - duck refusing, pq loading - is never covered"];
    `:csvdiff_otval.csv 1: "a\n1\n";
    r3:.csvDiffTest.i.cmp_tables[`name`file`opts`types`allow!(`gate;"csvdiff_otval.csv";.csvDiffTest.NOOPT;::;enlist `overtolerant);
        ([]a:enlist 1);([]a:enlist 2);"overtolerant"];
    .qunit.assertEquals[r3`pass;0b;"a value mismatch between two successful loads is never covered"]};

.csvDiffTest.testGatingOvertolerantNeedsLiveOracle:{
    `:csvdiff_otd.csv 1: "a,b\n\"x\"y,1\n";
    live:.csvDiffTest.Duck;
    .csvDiffTest.Duck::0N;
    r:.csvDiffTest.i.gate["csvdiff_otd.csv";::;`overtolerant];
    .csvDiffTest.Duck::live;
    .qunit.assertEquals[r`pass;0b;"a dead oracle is a dual refusal, and the grant never blesses one"]};

/ 0xFF is never valid UTF-8 in any position, so duck rejects the FILE while pq carries the byte through
.csvDiffTest.testGatingBytesCoversDuckRefusalOnly:{
    `:csvdiff_by.csv 1: "a\nx\377z\n";
    r0:.csvDiffTest.i.gate["csvdiff_by.csv";::;0#`];
    r1:.csvDiffTest.i.gate["csvdiff_by.csv";::;`bytes];
    .qunit.assertEquals[r0`pass;0b;"duck refusing where pq loads is red without the grant"];
    .qunit.assertEquals[r1`pass;1b;"and passes under it"];
    .qunit.assertEquals[(r1`count_match;r1`error_count);(0b;1);"with the divergence still visible in the row"];
    `:csvdiff_byrev.csv 1: "a,b\n\"x\"y,1\n";
    r2:.csvDiffTest.i.gate["csvdiff_byrev.csv";::;`bytes];
    .qunit.assertEquals[r2`pass;0b;"the REVERSE asymmetry - pq refusing, duck loading - is never covered"];
    `:csvdiff_byval.csv 1: "a\n1\n";
    r3:.csvDiffTest.i.cmp_tables[`name`file`opts`types`allow!(`gate;"csvdiff_byval.csv";.csvDiffTest.NOOPT;::;enlist `bytes);
        ([]a:enlist 1);([]a:enlist 2);"bytes"];
    .qunit.assertEquals[r3`pass;0b;"a value mismatch between two successful loads is never covered"]};

.csvDiffTest.testGatingBytesNeedsLiveOracle:{
    `:csvdiff_byd.csv 1: "a\nx\377z\n";
    live:.csvDiffTest.Duck;
    .csvDiffTest.Duck::0N;
    r:.csvDiffTest.i.gate["csvdiff_byd.csv";::;`bytes];
    .csvDiffTest.Duck::live;
    .qunit.assertEquals[r`pass;0b;"a dead oracle is a dual refusal, and the grant never blesses one"]};

/ exact equality in the common unit is what bounds the clock grant to a CLASS difference
.csvDiffTest.testGatingClockCoversLatticeOnly:{
    `:csvdiff_ck.csv 1: "a\n05:40\n";
    r0:.csvDiffTest.i.gate["csvdiff_ck.csv";::;0#`];
    r1:.csvDiffTest.i.gate["csvdiff_ck.csv";::;`clock];
    .qunit.assertEquals[r0`pass;0b;"minute-against-TIME is red without the grant"];
    .qunit.assertEquals[r1`pass;1b;"and passes under it"];
    .qunit.assertEquals[r1`types_match;0b;"with the raw type divergence still visible"];
    fx:`name`file`opts`types`allow!(`gate;"csvdiff_ckval.csv";.csvDiffTest.NOOPT;::;enlist `clock);
    .qunit.assertEquals[.csvDiffTest.i.cmp_tables[fx;([]a:enlist 05:40);([]a:enlist 06:40:00.000);"clock"]`pass;0b;
        "an unequal instant is red even under clock"];
    .qunit.assertEquals[.csvDiffTest.i.cmp_tables[fx;([]a:enlist 0Nu);([]a:enlist 0Nt);"clock"]`pass;1b;
        "matching nulls agree across the lattice"];
    .qunit.assertEquals[.csvDiffTest.i.cmp_tables[fx;([]a:enlist 0Nu);([]a:enlist 00:00:00.000);"clock"]`pass;0b;
        "a null against a real instant does not"];
    / the vacuous case reaches cmp_col directly, and builds its empties with 0#enlist: `u$() signals
    / 'nyi in this build, so the old spelling measured that gap instead of the zero-count short circuit
    .qunit.assertEquals[.csvDiffTest.i.cmp_col[0#enlist 0Nu;"u";0#enlist 0Nt;"t"];`ok;
        "and two empty columns agree vacuously"]};

/ the zero-byte test is the whole grant: without it emptyfile would be a second name for overtolerant
.csvDiffTest.testGatingEmptyFileNeedsZeroBytes:{
    `:csvdiff_ef.csv 1: "";
    r0:.csvDiffTest.i.gate["csvdiff_ef.csv";::;0#`];
    r1:.csvDiffTest.i.gate["csvdiff_ef.csv";::;`emptyfile];
    .qunit.assertEquals[r0`pass;0b;"pq refusing a zero-byte file where duck invents column0 is red without the grant"];
    .qunit.assertEquals[r1`pass;1b;"and passes under it"];
    .qunit.assertEquals[(r1`count_match;r1`error_count);(0b;1);"with the divergence still visible in the row"];
    `:csvdiff_efot.csv 1: "a,b\n\"x\"y,1\n";
    r2:.csvDiffTest.i.gate["csvdiff_efot.csv";::;`emptyfile];
    .qunit.assertEquals[r2`pass;0b;"a NON-empty file pq refuses is duck's leniency, not this grant"];
    `:csvdiff_efrev.csv 1: "a\nx\377z\n";
    r3:.csvDiffTest.i.gate["csvdiff_efrev.csv";::;`emptyfile];
    .qunit.assertEquals[r3`pass;0b;"and the reverse asymmetry is never covered"]};

.csvDiffTest.testGatingEmptyFileNeedsLiveOracle:{
    `:csvdiff_efd.csv 1: "";
    live:.csvDiffTest.Duck;
    .csvDiffTest.Duck::0N;
    r:.csvDiffTest.i.gate["csvdiff_efd.csv";::;`emptyfile];
    .csvDiffTest.Duck::live;
    .qunit.assertEquals[r`pass;0b;"a dead oracle is a dual refusal, and the grant never blesses one"]};

/ the blank-line COUNT is what bounds this grant to blank lines: duck rows beyond them stay red
.csvDiffTest.testGatingBlankLineCoversTheSkipOnly:{
    `:csvdiff_bl.csv 1: "a\n1\n\n2\n";
    r0:.csvDiffTest.i.gate["csvdiff_bl.csv";::;0#`];
    r1:.csvDiffTest.i.gate["csvdiff_bl.csv";::;`blankline];
    .qunit.assertEquals[r0`pass;0b;"the extra all-null row is red without the grant"];
    .qunit.assertEquals[r1`pass;1b;"and passes under it once the blank-line row is removed"];
    f:`:csvdiff_blnul.csv 1: "a,b\n1,\n\n2,y\n";
    .qunit.assertEquals[.csvDiffTest.i.gate["csvdiff_blnul.csv";::;`blankline]`pass;1b;
        "an empty CELL is not a blank line - it stays in both tables and still compares"];
    fx:`name`file`opts`types`allow!(`gate;"csvdiff_bl.csv";.csvDiffTest.NOOPT;::;enlist `blankline);
    .qunit.assertEquals[.csvDiffTest.i.cmp_tables[fx;([]a:1 2);([]a:1 0N 0N 2);"blankline"]`pass;0b;
        "two null rows against one blank line is a row pq dropped for another reason - red"];
    .qunit.assertEquals[.csvDiffTest.i.cmp_tables[fx;([]a:1 2);([]a:1 3 2);"blankline"]`pass;0b;
        "and a real extra row is never blank, so it is never removed"]};

/ ---- hand fixtures: the bytes sit in the test, next to the assertion that reads them ---------------

.csvDiffTest.test_clean:{
    f:`:csvdiff_clean.csv 1: "id,qty,price,day,ok\n1,10,1.5,2024-01-15,true\n2,20,2.5,2024-02-20,false\n3,30,3.5,2024-03-25,true\n";
    .csvDiffTest.chk[f;()!();0#`]};

.csvDiffTest.test_quoted:{
    f:`:csvdiff_quoted.csv 1: "a,b\n\"x\ny\",1\n\"say \"\"hi\"\"\",2\n\"comma, inside\",3\n";
    .csvDiffTest.chk[f;()!();0#`]};

.csvDiffTest.test_crlf:{
    f:`:csvdiff_crlf.csv 1: "a,b\r\n1,2\r\n3,4\r\n";
    .csvDiffTest.chk[f;()!();0#`]};

.csvDiffTest.test_cronly:{
    f:`:csvdiff_cronly.csv 1: "a,b\r1,2\r3,4";
    .csvDiffTest.chk[f;()!();0#`]};

.csvDiffTest.test_noheader:{
    f:`:csvdiff_noheader.csv 1: "10,20\n30,40\n";
    .csvDiffTest.chk[f;(enlist `header)!enlist 0b;0#`]};

.csvDiffTest.test_empty:{
    f:`:csvdiff_empty.csv 1: "";
    .csvDiffTest.chk[f;()!();`emptyfile]};

.csvDiffTest.test_hdronly:{
    f:`:csvdiff_hdronly.csv 1: "a,b\n";
    .csvDiffTest.chk[f;()!();0#`]};

.csvDiffTest.test_openquote:{
    f:`:csvdiff_openq.csv 1: "a,b\n\"x,1\n";
    .csvDiffTest.chk[f;()!();enlist `overtolerant]};

/ ---- vendored fixtures: one line per upstream file, named after it ------------------------------

.csvDiffTest.test_bool:{.csvDiffTest.chk["bool.csv";()!();0#`]};
.csvDiffTest.test_big_number:{.csvDiffTest.chk["big_number.csv";()!();0#`]};
.csvDiffTest.test_header_only:{.csvDiffTest.chk["header_only.csv";()!();0#`]};
.csvDiffTest.test_blank_line:{.csvDiffTest.chk["blank_line.csv";()!();`blankline]};
.csvDiffTest.test_small_file:{.csvDiffTest.chk["small_file.csv";`delim`header!(";";0b);0#`]};
.csvDiffTest.test_mixed_decimal:{.csvDiffTest.chk["mixed_decimal.csv";(enlist `delim)!enlist ";";0#`]};
.csvDiffTest.test_null_string:{.csvDiffTest.chk["null_string.csv";()!();0#`]};
.csvDiffTest.test_header_bug:{.csvDiffTest.chk["header_bug.csv";()!();0#`]};
.csvDiffTest.test_special_date:{.csvDiffTest.chk["special_date.csv";()!();enlist `clock]};
.csvDiffTest.test_one_n_two:{.csvDiffTest.chk["one_n_two.csv";()!();0#`]};
.csvDiffTest.test_locations_row_trailing_comma:{.csvDiffTest.chk["locations_row_trailing_comma.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_nullpadding:{.csvDiffTest.chk["nullpadding.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_double_quoted_header:{.csvDiffTest.chk["double_quoted_header.csv";()!();0#`]};
.csvDiffTest.test_quoted_values_delimited:{.csvDiffTest.chk["quoted_values_delimited.csv";()!();0#`]};
.csvDiffTest.test_csv_quoted_newline_odd:{.csvDiffTest.chk["csv_quoted_newline_odd.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_sample_0:{.csvDiffTest.chk["sample-0.csv";()!();0#`]};
.csvDiffTest.test_autotypecandidates:{.csvDiffTest.chk["autotypecandidates.csv";`delim`header!("|";0b);0#`]};

/ ---- batch 2: the bulk corpus import (defaults only unless the row says otherwise) --------------

.csvDiffTest.test__hidden:{.csvDiffTest.chk[".hidden.csv";()!();0#`]};
.csvDiffTest.test__hidden_file:{.csvDiffTest.chk[".hidden_file.csv";()!();0#`]};
.csvDiffTest.test_0:{.csvDiffTest.chk["0.csv";()!();0#`]};
.csvDiffTest.test_1:{.csvDiffTest.chk["1.csv";()!();0#`]};
.csvDiffTest.test_10:{.csvDiffTest.chk["10.csv";()!();0#`]};
.csvDiffTest.test_11:{.csvDiffTest.chk["11.csv";()!();`emptyfile]};
.csvDiffTest.test_12:{.csvDiffTest.chk["12.csv";()!();0#`]};
.csvDiffTest.test_13:{.csvDiffTest.chk["13.csv";()!();0#`]};
.csvDiffTest.test_14:{.csvDiffTest.chk["14.csv";()!();0#`]};
.csvDiffTest.test_14177:{.csvDiffTest.chk["14177.csv";()!();0#`]};
.csvDiffTest.test_14512_og:{.csvDiffTest.chk["14512_og.csv";()!();0#`]};
.csvDiffTest.test_14635:{.csvDiffTest.chk["14635.csv";()!();0#`]};
.csvDiffTest.test_14648:{.csvDiffTest.chk["14648.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_15:{.csvDiffTest.chk["15.csv";()!();0#`]};
.csvDiffTest.test_15473:{.csvDiffTest.chk["15473.csv";()!();0#`]};
.csvDiffTest.test_15473_time:{.csvDiffTest.chk["15473_time.csv";()!();0#`]};
.csvDiffTest.test_15473_time_timestamp:{.csvDiffTest.chk["15473_time_timestamp.csv";()!();0#`]};
.csvDiffTest.test_15473_timestamp:{.csvDiffTest.chk["15473_timestamp.csv";()!();0#`]};
.csvDiffTest.test_16:{.csvDiffTest.chk["16.csv";()!();0#`]};
.csvDiffTest.test_17226:{.csvDiffTest.chk["17226.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_17705:{.csvDiffTest.chk["17705.csv";()!();0#`]};
.csvDiffTest.test_18:{.csvDiffTest.chk["18.csv";()!();0#`]};
.csvDiffTest.test_19:{.csvDiffTest.chk["19.csv";()!();0#`]};
.csvDiffTest.test_2:{.csvDiffTest.chk["2.csv";()!();0#`]};
.csvDiffTest.test_20:{.csvDiffTest.chk["20.csv";()!();0#`]};
.csvDiffTest.test_21:{.csvDiffTest.chk["21.csv";()!();0#`]};
.csvDiffTest.test_23:{.csvDiffTest.chk["23.csv";()!();0#`]};
.csvDiffTest.test_25:{.csvDiffTest.chk["25.csv";()!();`emptyfile]};
.csvDiffTest.test_26:{.csvDiffTest.chk["26.csv";()!();0#`]};
.csvDiffTest.test_27:{.csvDiffTest.chk["27.csv";()!();0#`]};
.csvDiffTest.test_28:{.csvDiffTest.chk["28.csv";()!();0#`]};
.csvDiffTest.test_29:{.csvDiffTest.chk["29.csv";()!();0#`]};
.csvDiffTest.test_3:{.csvDiffTest.chk["3.csv";()!();0#`]};
.csvDiffTest.test_30:{.csvDiffTest.chk["30.csv";()!();0#`]};
.csvDiffTest.test_31:{.csvDiffTest.chk["31.csv";()!();0#`]};
.csvDiffTest.test_32:{.csvDiffTest.chk["32.csv";`strict_mode`null_padding!(0b;1b);0#`]};
.csvDiffTest.test_34:{.csvDiffTest.chk["34.csv";()!();0#`]};
.csvDiffTest.test_35:{.csvDiffTest.chk["35.csv";()!();0#`]};
.csvDiffTest.test_36:{.csvDiffTest.chk["36.csv";()!();0#`]};
.csvDiffTest.test_37:{.csvDiffTest.chk["37.csv";()!();0#`]};
.csvDiffTest.test_38:{.csvDiffTest.chk["38.csv";()!();0#`]};
.csvDiffTest.test_39:{.csvDiffTest.chk["39.csv";()!();0#`]};
.csvDiffTest.test_4:{.csvDiffTest.chk["4.csv";()!();`emptyfile]};
.csvDiffTest.test_40:{.csvDiffTest.chk["40.csv";()!();0#`]};
.csvDiffTest.test_41:{.csvDiffTest.chk["41.csv";()!();0#`]};
.csvDiffTest.test_43:{.csvDiffTest.chk["43.csv";()!();0#`]};
.csvDiffTest.test_44:{.csvDiffTest.chk["44.csv";()!();0#`]};
.csvDiffTest.test_45:{.csvDiffTest.chk["45.csv";()!();0#`]};
.csvDiffTest.test_46:{.csvDiffTest.chk["46.csv";()!();0#`]};
.csvDiffTest.test_47:{.csvDiffTest.chk["47.csv";()!();0#`]};
.csvDiffTest.test_48:{.csvDiffTest.chk["48.csv";()!();0#`]};
.csvDiffTest.test_49:{.csvDiffTest.chk["49.csv";()!();0#`]};
.csvDiffTest.test_5:{.csvDiffTest.chk["5.csv";()!();0#`]};
.csvDiffTest.test_50:{.csvDiffTest.chk["50.csv";()!();0#`]};
.csvDiffTest.test_51:{.csvDiffTest.chk["51.csv";()!();0#`]};
.csvDiffTest.test_52:{.csvDiffTest.chk["52.csv";()!();0#`]};
.csvDiffTest.test_53:{.csvDiffTest.chk["53.csv";()!();0#`]};
.csvDiffTest.test_54:{.csvDiffTest.chk["54.csv";()!();0#`]};
.csvDiffTest.test_5438:{.csvDiffTest.chk["5438.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_55:{.csvDiffTest.chk["55.csv";()!();0#`]};
.csvDiffTest.test_6:{.csvDiffTest.chk["6.csv";()!();0#`]};
.csvDiffTest.test_7:{.csvDiffTest.chk["7.csv";()!();0#`]};
.csvDiffTest.test_8:{.csvDiffTest.chk["8.csv";()!();0#`]};
.csvDiffTest.test_9:{.csvDiffTest.chk["9.csv";()!();0#`]};
.csvDiffTest.test__avalon__daily_avg:{.csvDiffTest.chk["[avalon]_daily-avg.csv";()!();0#`]};
.csvDiffTest.test___2000:{.csvDiffTest.chk["__2000.csv";()!();0#`]};
.csvDiffTest.test___2001:{.csvDiffTest.chk["__2001.csv";()!();0#`]};
.csvDiffTest.test_a:{.csvDiffTest.chk["a.csv";()!();0#`]};
.csvDiffTest.test_a1:{.csvDiffTest.chk["a1.csv";()!();0#`]};
.csvDiffTest.test_a2:{.csvDiffTest.chk["a2.csv";()!();0#`]};
.csvDiffTest.test_aa_delim:{.csvDiffTest.chk["aa_delim.csv";()!();0#`]};
.csvDiffTest.test_aa_delim_quoted:{.csvDiffTest.chk["aa_delim_quoted.csv";(enlist `quote)!enlist "";0#`]};
.csvDiffTest.test_aa_delim_quoted_2:{.csvDiffTest.chk["aa_delim_quoted_2.csv";(enlist `quote)!enlist "";0#`]};
.csvDiffTest.test_aa_delim_small:{.csvDiffTest.chk["aa_delim_small.csv";()!();0#`]};
.csvDiffTest.test_aaa_delim:{.csvDiffTest.chk["aaa_delim.csv";()!();0#`]};
.csvDiffTest.test_aaaa_delim:{.csvDiffTest.chk["aaaa_delim.csv";()!();0#`]};
.csvDiffTest.test_aaaa_delim_rn:{.csvDiffTest.chk["aaaa_delim_rn.csv";()!();0#`]};
.csvDiffTest.test_aaab_delim:{.csvDiffTest.chk["aaab_delim.csv";()!();0#`]};
.csvDiffTest.test_aab_delim:{.csvDiffTest.chk["aab_delim.csv";()!();0#`]};
.csvDiffTest.test_ab_delim:{.csvDiffTest.chk["ab_delim.csv";()!();0#`]};
.csvDiffTest.test_abac:{.csvDiffTest.chk["abac.csv";()!();0#`]};
.csvDiffTest.test_abac_incomplete_quote:{.csvDiffTest.chk["abac_incomplete_quote.csv";()!();0#`]};
.csvDiffTest.test_abac_mix:{.csvDiffTest.chk["abac_mix.csv";()!();0#`]};
.csvDiffTest.test_abac_newline_in_quote:{.csvDiffTest.chk["abac_newline_in_quote.csv";()!();0#`]};
.csvDiffTest.test_add_escaped_rfc:{.csvDiffTest.chk["add_escaped_rfc.csv";()!();`emptyfile]};
.csvDiffTest.test_afile:{.csvDiffTest.chk["afile.csv";()!();0#`]};
.csvDiffTest.test_all_latin1:{.csvDiffTest.chk["all_latin1.csv";()!();enlist `bytes]};
.csvDiffTest.test_all_varchar:{.csvDiffTest.chk["all_varchar.csv";()!();0#`]};
.csvDiffTest.test_aws_locations:{.csvDiffTest.chk["aws_locations.csv";()!();0#`]};
.csvDiffTest.test_b:{.csvDiffTest.chk["b.csv";()!();0#`]};
.csvDiffTest.test_b1:{.csvDiffTest.chk["b1.csv";()!();0#`]};
.csvDiffTest.test_backslash_escape:{.csvDiffTest.chk["backslash_escape.csv";(enlist `escape)!enlist "\\";0#`]};
.csvDiffTest.test_bad:{.csvDiffTest.chk["bad.csv";()!();0#`]};
.csvDiffTest.test_bad2:{.csvDiffTest.chk["bad2.csv";()!();0#`]};
.csvDiffTest.test_bad_date:{.csvDiffTest.chk["bad_date.csv";()!();0#`]};
.csvDiffTest.test_bad_date_2:{.csvDiffTest.chk["bad_date_2.csv";()!();0#`]};
.csvDiffTest.test_bad_date_timestamp_mix:{.csvDiffTest.chk["bad_date_timestamp_mix.csv";()!();0#`]};
.csvDiffTest.test_bad_escape:{.csvDiffTest.chk["bad_escape.csv";(enlist `escape)!enlist "\\";0#`]};
.csvDiffTest.test_basic:{.csvDiffTest.chk["basic.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_big_escape:{.csvDiffTest.chk["big_escape.csv";()!();0#`]};
.csvDiffTest.test_big_header:{.csvDiffTest.chk["big_header.csv";()!();0#`]};
.csvDiffTest.test_big_not_bool:{.csvDiffTest.chk["big_not_bool.csv";()!();0#`]};
.csvDiffTest.test_borked_type:{.csvDiffTest.chk["borked_type.csv";()!();0#`]};
.csvDiffTest.test_bug_10273:{.csvDiffTest.chk["bug_10273.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_c:{.csvDiffTest.chk["c.csv";()!();0#`]};
.csvDiffTest.test_c1:{.csvDiffTest.chk["c1.csv";()!();0#`]};
.csvDiffTest.test_c2:{.csvDiffTest.chk["c2.csv";()!();0#`]};
.csvDiffTest.test_c3:{.csvDiffTest.chk["c3.csv";()!();0#`]};
.csvDiffTest.test_carriage_feed_newline:{.csvDiffTest.chk["carriage_feed_newline.csv";()!();0#`]};
.csvDiffTest.test_case_1:{.csvDiffTest.chk["case_1.csv";`quote`ignore_errors!("";1b);0#`]};
.csvDiffTest.test_case_10:{.csvDiffTest.chk["case_10.csv";()!();0#`]};
.csvDiffTest.test_case_11:{.csvDiffTest.chk["case_11.csv";()!();0#`]};
.csvDiffTest.test_case_12:{.csvDiffTest.chk["case_12.csv";()!();0#`]};
.csvDiffTest.test_case_13:{.csvDiffTest.chk["case_13.csv";()!();0#`]};
.csvDiffTest.test_case_14:{.csvDiffTest.chk["case_14.csv";()!();0#`]};
.csvDiffTest.test_case_15:{.csvDiffTest.chk["case_15.csv";()!();0#`]};
.csvDiffTest.test_case_16:{.csvDiffTest.chk["case_16.csv";()!();0#`]};
.csvDiffTest.test_case_17:{.csvDiffTest.chk["case_17.csv";()!();0#`]};
.csvDiffTest.test_case_18:{.csvDiffTest.chk["case_18.csv";()!();0#`]};
.csvDiffTest.test_case_19:{.csvDiffTest.chk["case_19.csv";()!();0#`]};
.csvDiffTest.test_case_2:{.csvDiffTest.chk["case_2.csv";()!();0#`]};
.csvDiffTest.test_case_20:{.csvDiffTest.chk["case_20.csv";()!();0#`]};
.csvDiffTest.test_case_21:{.csvDiffTest.chk["case_21.csv";()!();0#`]};
.csvDiffTest.test_case_22:{.csvDiffTest.chk["case_22.csv";()!();0#`]};
.csvDiffTest.test_case_23:{.csvDiffTest.chk["case_23.csv";()!();0#`]};
.csvDiffTest.test_case_24:{.csvDiffTest.chk["case_24.csv";()!();0#`]};
.csvDiffTest.test_case_25:{.csvDiffTest.chk["case_25.csv";()!();0#`]};
.csvDiffTest.test_case_26:{.csvDiffTest.chk["case_26.csv";()!();0#`]};
.csvDiffTest.test_case_27:{.csvDiffTest.chk["case_27.csv";()!();0#`]};
.csvDiffTest.test_case_28:{.csvDiffTest.chk["case_28.csv";()!();0#`]};
.csvDiffTest.test_case_29:{.csvDiffTest.chk["case_29.csv";()!();0#`]};
.csvDiffTest.test_case_3:{.csvDiffTest.chk["case_3.csv";()!();0#`]};
.csvDiffTest.test_case_30:{.csvDiffTest.chk["case_30.csv";()!();0#`]};
.csvDiffTest.test_case_31:{.csvDiffTest.chk["case_31.csv";()!();0#`]};
.csvDiffTest.test_case_32:{.csvDiffTest.chk["case_32.csv";()!();0#`]};
.csvDiffTest.test_case_33:{.csvDiffTest.chk["case_33.csv";()!();0#`]};
.csvDiffTest.test_case_34:{.csvDiffTest.chk["case_34.csv";()!();0#`]};
.csvDiffTest.test_case_35:{.csvDiffTest.chk["case_35.csv";()!();0#`]};
.csvDiffTest.test_case_36:{.csvDiffTest.chk["case_36.csv";()!();0#`]};
.csvDiffTest.test_case_37:{.csvDiffTest.chk["case_37.csv";()!();0#`]};
.csvDiffTest.test_case_38:{.csvDiffTest.chk["case_38.csv";()!();0#`]};
.csvDiffTest.test_case_39:{.csvDiffTest.chk["case_39.csv";()!();0#`]};
.csvDiffTest.test_case_4:{.csvDiffTest.chk["case_4.csv";()!();`emptyfile]};
.csvDiffTest.test_case_40:{.csvDiffTest.chk["case_40.csv";()!();0#`]};
.csvDiffTest.test_case_41:{.csvDiffTest.chk["case_41.csv";()!();0#`]};
.csvDiffTest.test_case_42:{.csvDiffTest.chk["case_42.csv";()!();0#`]};
.csvDiffTest.test_case_43:{.csvDiffTest.chk["case_43.csv";()!();0#`]};
.csvDiffTest.test_case_44:{.csvDiffTest.chk["case_44.csv";()!();0#`]};
.csvDiffTest.test_case_45:{.csvDiffTest.chk["case_45.csv";()!();0#`]};
.csvDiffTest.test_case_46:{.csvDiffTest.chk["case_46.csv";()!();0#`]};
.csvDiffTest.test_case_47:{.csvDiffTest.chk["case_47.csv";()!();0#`]};
.csvDiffTest.test_case_48:{.csvDiffTest.chk["case_48.csv";()!();0#`]};
.csvDiffTest.test_case_49:{.csvDiffTest.chk["case_49.csv";()!();0#`]};
.csvDiffTest.test_case_50:{.csvDiffTest.chk["case_50.csv";()!();0#`]};
.csvDiffTest.test_case_51:{.csvDiffTest.chk["case_51.csv";()!();0#`]};
.csvDiffTest.test_case_52:{.csvDiffTest.chk["case_52.csv";()!();0#`]};
.csvDiffTest.test_case_54:{.csvDiffTest.chk["case_54.csv";()!();0#`]};
.csvDiffTest.test_case_55:{.csvDiffTest.chk["case_55.csv";()!();0#`]};
.csvDiffTest.test_case_56:{.csvDiffTest.chk["case_56.csv";()!();0#`]};
.csvDiffTest.test_case_57:{.csvDiffTest.chk["case_57.csv";()!();0#`]};
.csvDiffTest.test_case_58:{.csvDiffTest.chk["case_58.csv";()!();0#`]};
.csvDiffTest.test_case_59:{.csvDiffTest.chk["case_59.csv";()!();0#`]};
.csvDiffTest.test_case_6:{.csvDiffTest.chk["case_6.csv";()!();0#`]};
.csvDiffTest.test_case_60:{.csvDiffTest.chk["case_60.csv";()!();0#`]};
.csvDiffTest.test_case_61:{.csvDiffTest.chk["case_61.csv";()!();0#`]};
.csvDiffTest.test_case_62:{.csvDiffTest.chk["case_62.csv";()!();0#`]};
.csvDiffTest.test_case_63:{.csvDiffTest.chk["case_63.csv";()!();0#`]};
.csvDiffTest.test_case_64:{.csvDiffTest.chk["case_64.csv";()!();0#`]};
.csvDiffTest.test_case_65:{.csvDiffTest.chk["case_65.csv";()!();0#`]};
.csvDiffTest.test_case_66:{.csvDiffTest.chk["case_66.csv";()!();0#`]};
.csvDiffTest.test_case_67:{.csvDiffTest.chk["case_67.csv";()!();0#`]};
.csvDiffTest.test_case_68:{.csvDiffTest.chk["case_68.csv";()!();0#`]};
.csvDiffTest.test_case_69:{.csvDiffTest.chk["case_69.csv";()!();0#`]};
.csvDiffTest.test_case_7:{.csvDiffTest.chk["case_7.csv";()!();0#`]};
.csvDiffTest.test_case_70:{.csvDiffTest.chk["case_70.csv";()!();0#`]};
.csvDiffTest.test_case_71:{.csvDiffTest.chk["case_71.csv";()!();0#`]};
.csvDiffTest.test_case_72:{.csvDiffTest.chk["case_72.csv";()!();0#`]};
.csvDiffTest.test_case_73:{.csvDiffTest.chk["case_73.csv";()!();0#`]};
.csvDiffTest.test_case_74:{.csvDiffTest.chk["case_74.csv";()!();0#`]};
.csvDiffTest.test_case_75:{.csvDiffTest.chk["case_75.csv";()!();0#`]};
.csvDiffTest.test_case_76:{.csvDiffTest.chk["case_76.csv";()!();0#`]};
.csvDiffTest.test_case_77:{.csvDiffTest.chk["case_77.csv";()!();0#`]};
.csvDiffTest.test_case_78:{.csvDiffTest.chk["case_78.csv";()!();0#`]};
.csvDiffTest.test_case_79:{.csvDiffTest.chk["case_79.csv";()!();0#`]};
.csvDiffTest.test_case_8:{.csvDiffTest.chk["case_8.csv";()!();0#`]};
.csvDiffTest.test_case_80:{.csvDiffTest.chk["case_80.csv";()!();0#`]};
.csvDiffTest.test_case_81:{.csvDiffTest.chk["case_81.csv";()!();0#`]};
.csvDiffTest.test_case_82:{.csvDiffTest.chk["case_82.csv";()!();0#`]};
.csvDiffTest.test_case_83:{.csvDiffTest.chk["case_83.csv";()!();0#`]};
.csvDiffTest.test_case_84:{.csvDiffTest.chk["case_84.csv";()!();0#`]};
.csvDiffTest.test_case_85:{.csvDiffTest.chk["case_85.csv";()!();0#`]};
.csvDiffTest.test_case_86:{.csvDiffTest.chk["case_86.csv";()!();0#`]};
.csvDiffTest.test_case_87:{.csvDiffTest.chk["case_87.csv";()!();0#`]};
.csvDiffTest.test_case_88:{.csvDiffTest.chk["case_88.csv";()!();0#`]};
.csvDiffTest.test_case_9:{.csvDiffTest.chk["case_9.csv";()!();0#`]};
.csvDiffTest.test_cast_and_less_col:{.csvDiffTest.chk["cast_and_less_col.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_cast_and_maxline:{.csvDiffTest.chk["cast_and_maxline.csv";()!();0#`]};
.csvDiffTest.test_cast_and_more_col:{.csvDiffTest.chk["cast_and_more_col.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_comma_decimal_null:{.csvDiffTest.chk["comma_decimal_null.csv";()!();0#`]};
.csvDiffTest.test_comment_skip:{.csvDiffTest.chk["comment_skip.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_complex_unterminated_quote:{.csvDiffTest.chk["complex_unterminated_quote.csv";()!();0#`]};
.csvDiffTest.test_conflict_timestamp:{.csvDiffTest.chk["conflict_timestamp.csv";()!();0#`]};
.csvDiffTest.test_custom_date:{.csvDiffTest.chk["custom_date.csv";()!();0#`]};
.csvDiffTest.test_customer_4:{.csvDiffTest.chk["customer.4.csv";()!();0#`]};
.csvDiffTest.test_customer:{.csvDiffTest.chk["customer.csv";()!();0#`]};
.csvDiffTest.test_d:{.csvDiffTest.chk["d.csv";()!();0#`]};
.csvDiffTest.test_date:{.csvDiffTest.chk["date.csv";()!();0#`]};
.csvDiffTest.test_date_example_1:{.csvDiffTest.chk["date_example_1.csv";()!();0#`]};
.csvDiffTest.test_date_format_bug_linux:{.csvDiffTest.chk["date_format_bug_linux.csv";()!();0#`]};
.csvDiffTest.test_date_format_percentage:{.csvDiffTest.chk["date_format_percentage.csv";()!();0#`]};
.csvDiffTest.test_date_specificity:{.csvDiffTest.chk["date_specificity.csv";()!();0#`]};
.csvDiffTest.test_dateformat:{.csvDiffTest.chk["dateformat.csv";(enlist `dateformat)!enlist "%d/%m/%Y";0#`]};
.csvDiffTest.test_dateformat_2:{.csvDiffTest.chk["dateformat_2.csv";(enlist `dateformat)!enlist "%d/%m/%Y";0#`]};
.csvDiffTest.test_dates_special_format:{.csvDiffTest.chk["dates_special_format.csv";()!();0#`]};
.csvDiffTest.test_decimal:{.csvDiffTest.chk["decimal.csv";()!();0#`]};
.csvDiffTest.test_decimal_separators_csv:{.csvDiffTest.chk["decimal_separators_csv.csv";()!();0#`]};
.csvDiffTest.test_device_metadata_1:{.csvDiffTest.chk["device_metadata_1.csv";()!();0#`]};
.csvDiffTest.test_dim0:{.csvDiffTest.chk["dim0.csv";()!();0#`]};
.csvDiffTest.test_dirty_line:{.csvDiffTest.chk["dirty_line.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_dr_who:{.csvDiffTest.chk["dr_who.csv";()!();0#`]};
.csvDiffTest.test_duplicate_header_col:{.csvDiffTest.chk["duplicate_header_col.csv";()!();0#`]};
.csvDiffTest.test_duplicate_header_collision:{.csvDiffTest.chk["duplicate_header_collision.csv";()!();0#`]};
.csvDiffTest.test_duplicate_header_columns:{.csvDiffTest.chk["duplicate_header_columns.csv";()!();0#`]};
.csvDiffTest.test_early_out_error:{.csvDiffTest.chk["early_out_error.csv";()!();0#`]};
.csvDiffTest.test_empty_1:{.csvDiffTest.chk["empty_1.csv";()!();0#`]};
.csvDiffTest.test_empty_2:{.csvDiffTest.chk["empty_2.csv";()!();0#`]};
.csvDiffTest.test_empty_3:{.csvDiffTest.chk["empty_3.csv";()!();0#`]};
.csvDiffTest.test_empty_4:{.csvDiffTest.chk["empty_4.csv";()!();0#`]};
.csvDiffTest.test_empty_first_line:{.csvDiffTest.chk["empty_first_line.csv";()!();0#`]};
.csvDiffTest.test_empty_header:{.csvDiffTest.chk["empty_header.csv";()!();0#`]};
.csvDiffTest.test_end_quote:{.csvDiffTest.chk["end_quote.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_end_quote_2:{.csvDiffTest.chk["end_quote_2.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_end_quote_3:{.csvDiffTest.chk["end_quote_3.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_end_quote_mixed:{.csvDiffTest.chk["end_quote_mixed.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_enum_type:{.csvDiffTest.chk["enum_type.csv";()!();0#`]};
.csvDiffTest.test_error:{.csvDiffTest.chk["error.csv";`comment`ignore_errors!("#";1b);0#`]};
.csvDiffTest.test_error_invalid_type:{.csvDiffTest.chk["error_invalid_type.csv";()!();0#`]};
.csvDiffTest.test_error_too_little:{.csvDiffTest.chk["error_too_little.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_error_too_little_end_of_filled_chunk:{.csvDiffTest.chk["error_too_little_end_of_filled_chunk.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_error_too_little_single:{.csvDiffTest.chk["error_too_little_single.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_error_too_many:{.csvDiffTest.chk["error_too_many.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_escape:{.csvDiffTest.chk["escape.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_escape_non_quote_escape:{.csvDiffTest.chk["escape_non_quote_escape.csv";()!();0#`]};
.csvDiffTest.test_escape_non_quote_escape_complex:{.csvDiffTest.chk["escape_non_quote_escape_complex.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_escaped_quote:{.csvDiffTest.chk["escaped_quote.csv";()!();0#`]};
.csvDiffTest.test_evil_nullpadding:{.csvDiffTest.chk["evil_nullpadding.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_evil_nullpadding_2:{.csvDiffTest.chk["evil_nullpadding_2.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_example:{.csvDiffTest.chk["example.tsv";()!();0#`]};
.csvDiffTest.test_extra_delimiters:{.csvDiffTest.chk["extra_delimiters.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_f:{.csvDiffTest.chk["f.csv";()!();0#`]};
.csvDiffTest.test_f_1:{.csvDiffTest.chk["f_1.csv";()!();0#`]};
.csvDiffTest.test_f_2:{.csvDiffTest.chk["f_2.csv";()!();0#`]};
.csvDiffTest.test_f_3:{.csvDiffTest.chk["f_3.csv";()!();0#`]};
.csvDiffTest.test_file:{.csvDiffTest.chk["file.csv";()!();0#`]};
.csvDiffTest.test_file_1:{.csvDiffTest.chk["file_1.csv";()!();0#`]};
.csvDiffTest.test_file_2:{.csvDiffTest.chk["file_2.csv";()!();0#`]};
.csvDiffTest.test_file_3:{.csvDiffTest.chk["file_3.csv";()!();0#`]};
.csvDiffTest.test_file_4:{.csvDiffTest.chk["file_4.csv";()!();0#`]};
.csvDiffTest.test_file_5:{.csvDiffTest.chk["file_5.csv";()!();0#`]};
.csvDiffTest.test_file_6:{.csvDiffTest.chk["file_6.csv";()!();0#`]};
.csvDiffTest.test_file_ends_in_quoted_value:{.csvDiffTest.chk["file_ends_in_quoted_value.csv";()!();0#`]};
.csvDiffTest.test_file_error:{.csvDiffTest.chk["file_error.csv";()!();0#`]};
.csvDiffTest.test_file_no_header:{.csvDiffTest.chk["file_no_header.csv";()!();0#`]};
.csvDiffTest.test_flights:{.csvDiffTest.chk["flights.csv";()!();0#`]};
.csvDiffTest.test_flush:{.csvDiffTest.chk["flush.csv";()!();0#`]};
.csvDiffTest.test_force_not_null:{.csvDiffTest.chk["force_not_null.csv";()!();0#`]};
.csvDiffTest.test_force_not_null_inull:{.csvDiffTest.chk["force_not_null_inull.csv";()!();0#`]};
.csvDiffTest.test_force_not_null_reordered:{.csvDiffTest.chk["force_not_null_reordered.csv";()!();0#`]};
.csvDiffTest.test_force_quote:{.csvDiffTest.chk["force_quote.csv";()!();0#`]};
.csvDiffTest.test_from_df:{.csvDiffTest.chk["from_df.csv";(enlist `quote)!enlist "'";0#`]};
.csvDiffTest.test_greek_utf8:{.csvDiffTest.chk["greek_utf8.csv";()!();0#`]};
.csvDiffTest.test_header:{.csvDiffTest.chk["header.csv";()!();0#`]};
.csvDiffTest.test_header_2:{.csvDiffTest.chk["header_2.csv";()!();0#`]};
.csvDiffTest.test_header_left_space:{.csvDiffTest.chk["header_left_space.csv";()!();0#`]};
.csvDiffTest.test_header_normalize:{.csvDiffTest.chk["header_normalize.csv";()!();0#`]};
.csvDiffTest.test_header_only_2:{.csvDiffTest.chk["header_only_2.csv";()!();0#`]};
.csvDiffTest.test_hits_problematic:{.csvDiffTest.chk["hits_problematic.csv";()!();0#`]};
.csvDiffTest.test_hive_partition_null_overwrite:{.csvDiffTest.chk["hive_partition_null_overwrite.csv";()!();0#`]};
.csvDiffTest.test_identical:{.csvDiffTest.chk["identical.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_ignore_errors:{.csvDiffTest.chk["ignore_errors.csv";()!();0#`]};
.csvDiffTest.test_incomplete_multibyte_delimiter:{.csvDiffTest.chk["incomplete_multibyte_delimiter.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_inconsistent_cells:{.csvDiffTest.chk["inconsistent_cells.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_inconsistent_columns_3:{.csvDiffTest.chk["inconsistent_columns_3.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_inconsistent_columns_6:{.csvDiffTest.chk["inconsistent_columns_6.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_int_bol:{.csvDiffTest.chk["int_bol.csv";()!();0#`]};
.csvDiffTest.test_integer:{.csvDiffTest.chk["integer.csv";()!();0#`]};
.csvDiffTest.test_integer_exponent:{.csvDiffTest.chk["integer_exponent.csv";()!();0#`]};
.csvDiffTest.test_integers:{.csvDiffTest.chk["integers.csv";()!();0#`]};
.csvDiffTest.test_invalid_char:{.csvDiffTest.chk["invalid_char.csv";()!();0#`]};
.csvDiffTest.test_invalid_time:{.csvDiffTest.chk["invalid_time.csv";()!();0#`]};
.csvDiffTest.test_invalid_utf:{.csvDiffTest.chk["invalid_utf.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf8:{.csvDiffTest.chk["invalid_utf8.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_cast:{.csvDiffTest.chk["invalid_utf_cast.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_complex:{.csvDiffTest.chk["invalid_utf_complex.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_header:{.csvDiffTest.chk["invalid_utf_header.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_less:{.csvDiffTest.chk["invalid_utf_less.csv";()!();0#`]};
.csvDiffTest.test_invalid_utf_list:{.csvDiffTest.chk["invalid_utf_list.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_max_line:{.csvDiffTest.chk["invalid_utf_max_line.csv";()!();0#`]};
.csvDiffTest.test_invalid_utf_more:{.csvDiffTest.chk["invalid_utf_more.csv";()!();0#`]};
.csvDiffTest.test_invalid_utf_quoted:{.csvDiffTest.chk["invalid_utf_quoted.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_quoted_nl:{.csvDiffTest.chk["invalid_utf_quoted_nl.csv";()!();enlist `bytes]};
.csvDiffTest.test_invalid_utf_unquoted:{.csvDiffTest.chk["invalid_utf_unquoted.csv";()!();0#`]};
.csvDiffTest.test_issue2518:{.csvDiffTest.chk["issue2518.csv";()!();0#`]};
.csvDiffTest.test_issue5077:{.csvDiffTest.chk["issue5077.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_issue5077_aligned:{.csvDiffTest.chk["issue5077_aligned.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_issue6764:{.csvDiffTest.chk["issue6764.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_issue_1254:{.csvDiffTest.chk["issue_1254.csv";()!();0#`]};
.csvDiffTest.test_issue_1254_rn:{.csvDiffTest.chk["issue_1254_rn.csv";()!();0#`]};
.csvDiffTest.test_json:{.csvDiffTest.chk["json.csv";`quote`ignore_errors!("";1b);0#`]};
.csvDiffTest.test_later_quotes:{.csvDiffTest.chk["later_quotes.csv";()!();0#`]};
.csvDiffTest.test_latin1:{.csvDiffTest.chk["latin1.csv";()!();enlist `bytes]};
.csvDiffTest.test_leading_space_numerics:{.csvDiffTest.chk["leading_space_numerics.csv";()!();0#`]};
.csvDiffTest.test_leading_zeros:{.csvDiffTest.chk["leading_zeros.csv";()!();0#`]};
.csvDiffTest.test_less_col_and_max_line:{.csvDiffTest.chk["less_col_and_max_line.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_line_with_spaces:{.csvDiffTest.chk["line_with_spaces.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_lineitem_carriage:{.csvDiffTest.chk["lineitem-carriage.csv";()!();0#`]};
.csvDiffTest.test_lineitem_sample:{.csvDiffTest.chk["lineitem_sample.csv";()!();0#`]};
.csvDiffTest.test_locations_header_trailing_comma:{.csvDiffTest.chk["locations_header_trailing_comma.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_manycolumns:{.csvDiffTest.chk["manycolumns.csv";()!();0#`]};
.csvDiffTest.test_matching_types:{.csvDiffTest.chk["matching_types.csv";()!();0#`]};
.csvDiffTest.test_max_10:{.csvDiffTest.chk["max_10.csv";()!();0#`]};
.csvDiffTest.test_mid_line:{.csvDiffTest.chk["mid_line.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_mid_line_header:{.csvDiffTest.chk["mid_line_header.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_mid_line_invalid:{.csvDiffTest.chk["mid_line_invalid.csv";`comment`ignore_errors!("#";1b);0#`]};
.csvDiffTest.test_mid_line_null:{.csvDiffTest.chk["mid_line_null.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_mid_line_quote:{.csvDiffTest.chk["mid_line_quote.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_mid_null:{.csvDiffTest.chk["mid_null.csv";()!();0#`]};
.csvDiffTest.test_midline_empty_space:{.csvDiffTest.chk["midline_empty_space.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_missing_column:{.csvDiffTest.chk["missing_column.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_missing_header_col:{.csvDiffTest.chk["missing_header_col.csv";()!();0#`]};
.csvDiffTest.test_missing_many_col:{.csvDiffTest.chk["missing_many_col.csv";()!();0#`]};
.csvDiffTest.test_mixed_dates:{.csvDiffTest.chk["mixed_dates.csv";()!();0#`]};
.csvDiffTest.test_mixed_decimal_delimiter:{.csvDiffTest.chk["mixed_decimal_delimiter.csv";()!();0#`]};
.csvDiffTest.test_mixed_double:{.csvDiffTest.chk["mixed_double.csv";()!();0#`]};
.csvDiffTest.test_mixed_format_fail:{.csvDiffTest.chk["mixed_format_fail.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_mixed_new_line_2:{.csvDiffTest.chk["mixed_new_line_2.csv";()!();0#`]};
.csvDiffTest.test_mixed_single_line:{.csvDiffTest.chk["mixed_single_line.csv";()!();0#`]};
.csvDiffTest.test_mock_duckdb_test_data:{.csvDiffTest.chk["mock_duckdb_test_data.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_more_col_and_max_line:{.csvDiffTest.chk["more_col_and_max_line.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_multi_column:{.csvDiffTest.chk["multi_column.csv";()!();0#`]};
.csvDiffTest.test_multi_column_integer:{.csvDiffTest.chk["multi_column_integer.csv";()!();0#`]};
.csvDiffTest.test_multi_column_integer_rn:{.csvDiffTest.chk["multi_column_integer_rn.csv";()!();0#`]};
.csvDiffTest.test_multi_column_quote:{.csvDiffTest.chk["multi_column_quote.csv";()!();0#`]};
.csvDiffTest.test_multi_column_string:{.csvDiffTest.chk["multi_column_string.csv";()!();0#`]};
.csvDiffTest.test_multi_column_string_r:{.csvDiffTest.chk["multi_column_string_r.csv";()!();0#`]};
.csvDiffTest.test_multi_column_string_r_n:{.csvDiffTest.chk["multi_column_string_r_n.csv";()!();0#`]};
.csvDiffTest.test_multi_column_string_rn:{.csvDiffTest.chk["multi_column_string_rn.csv";()!();0#`]};
.csvDiffTest.test_multiple_cast_implicit:{.csvDiffTest.chk["multiple_cast_implicit.csv";()!();0#`]};
.csvDiffTest.test_multiple_casts_flush:{.csvDiffTest.chk["multiple_casts_flush.csv";()!();0#`]};
.csvDiffTest.test_multiple_casts_mixed:{.csvDiffTest.chk["multiple_casts_mixed.csv";()!();0#`]};
.csvDiffTest.test_multiple_errors:{.csvDiffTest.chk["multiple_errors.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_multiple_nulls:{.csvDiffTest.chk["multiple_nulls.csv";()!();0#`]};
.csvDiffTest.test_multiple_quoted_nulls:{.csvDiffTest.chk["multiple_quoted_nulls.csv";()!();0#`]};
.csvDiffTest.test_multiple_skip_row:{.csvDiffTest.chk["multiple_skip_row.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_new_line_string:{.csvDiffTest.chk["new_line_string.csv";()!();0#`]};
.csvDiffTest.test_new_line_string_rn:{.csvDiffTest.chk["new_line_string_rn.csv";()!();0#`]};
.csvDiffTest.test_new_line_string_rn_exc:{.csvDiffTest.chk["new_line_string_rn_exc.csv";()!();0#`]};
.csvDiffTest.test_nfc:{.csvDiffTest.chk["nfc.csv";()!();0#`]};
.csvDiffTest.test_nightmare:{.csvDiffTest.chk["nightmare.csv";()!();0#`]};
.csvDiffTest.test_no_header:{.csvDiffTest.chk["no_header.csv";()!();0#`]};
.csvDiffTest.test_no_newline:{.csvDiffTest.chk["no_newline.csv";()!();0#`]};
.csvDiffTest.test_no_opt:{.csvDiffTest.chk["no_opt.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_no_quote:{.csvDiffTest.chk["no_quote.csv";(enlist `quote)!enlist "";0#`]};
.csvDiffTest.test_normalize:{.csvDiffTest.chk["normalize.csv";()!();0#`]};
.csvDiffTest.test_normalize_names_1:{.csvDiffTest.chk["normalize_names_1.csv";()!();0#`]};
.csvDiffTest.test_normalize_names_2:{.csvDiffTest.chk["normalize_names_2.csv";()!();0#`]};
.csvDiffTest.test_normalize_names_3:{.csvDiffTest.chk["normalize_names_3.csv";()!();0#`]};
.csvDiffTest.test_normalize_names_4:{.csvDiffTest.chk["normalize_names_4.csv";()!();0#`]};
.csvDiffTest.test_normalize_names_5:{.csvDiffTest.chk["normalize_names_5.csv";()!();0#`]};
.csvDiffTest.test_normalize_names_6:{.csvDiffTest.chk["normalize_names_6.csv";()!();0#`]};
.csvDiffTest.test_not_hidden:{.csvDiffTest.chk["not_hidden.csv";()!();0#`]};
.csvDiffTest.test_not_working:{.csvDiffTest.chk["not_working.csv";()!();0#`]};
.csvDiffTest.test_null_comparison:{.csvDiffTest.chk["null_comparison.csv";()!();0#`]};
.csvDiffTest.test_nullbyte:{.csvDiffTest.chk["nullbyte.csv";()!();0#`]};
.csvDiffTest.test_nullpadding_commas:{.csvDiffTest.chk["nullpadding_commas.csv";(enlist `null_padding)!enlist 1b;0#`]};
.csvDiffTest.test_nullpadding_header:{.csvDiffTest.chk["nullpadding_header.csv";`comment`null_padding!("#";1b);0#`]};
.csvDiffTest.test_nullterm:{.csvDiffTest.chk["nullterm.csv";()!();0#`]};
.csvDiffTest.test_one_r_two:{.csvDiffTest.chk["one_r_two.csv";()!();0#`]};
.csvDiffTest.test_only_latin1:{.csvDiffTest.chk["only_latin1.csv";()!();enlist `bytes]};
.csvDiffTest.test_only_midline:{.csvDiffTest.chk["only_midline.csv";()!();0#`]};
.csvDiffTest.test_only_utf16:{.csvDiffTest.chk["only_utf16.csv";()!();0#`]};
.csvDiffTest.test_ontime_sample:{.csvDiffTest.chk["ontime_sample.csv";()!();0#`]};
.csvDiffTest.test_part_1:{.csvDiffTest.chk["part-1.csv";()!();0#`]};
.csvDiffTest.test_part_0_0001:{.csvDiffTest.chk["part_0_0001.csv";()!();0#`]};
.csvDiffTest.test_part_1_0001:{.csvDiffTest.chk["part_1_0001.csv";()!();0#`]};
.csvDiffTest.test_people:{.csvDiffTest.chk["people.csv";()!();0#`]};
.csvDiffTest.test_pipe_delim:{.csvDiffTest.chk["pipe_delim.csv";()!();0#`]};
.csvDiffTest.test_pipe_delim_quote:{.csvDiffTest.chk["pipe_delim_quote.csv";()!();0#`]};
.csvDiffTest.test_projection_buffer:{.csvDiffTest.chk["projection_buffer.csv";()!();0#`]};
.csvDiffTest.test_quote_escape:{.csvDiffTest.chk["quote_escape.csv";()!();0#`]};
.csvDiffTest.test_quoted_comment_buffer_boundary:{.csvDiffTest.chk["quoted_comment_buffer_boundary.csv";()!();0#`]};
.csvDiffTest.test_quoted_newline:{.csvDiffTest.chk["quoted_newline.csv";()!();0#`]};
.csvDiffTest.test_rejects_sniffer:{.csvDiffTest.chk["rejects_sniffer.csv";()!();0#`]};
.csvDiffTest.test_response:{.csvDiffTest.chk["response.csv";()!();0#`]};
.csvDiffTest.test_rfc_conform:{.csvDiffTest.chk["rfc_conform.csv";()!();0#`]};
.csvDiffTest.test_rfc_conform_quote:{.csvDiffTest.chk["rfc_conform_quote.csv";()!();0#`]};
.csvDiffTest.test_sales_snippet:{.csvDiffTest.chk["sales_snippet.csv";()!();0#`]};
.csvDiffTest.test_sample:{.csvDiffTest.chk["sample.csv";()!();0#`]};
.csvDiffTest.test_segfault:{.csvDiffTest.chk["segfault.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_semicolon_delim:{.csvDiffTest.chk["semicolon_delim.csv";()!();0#`]};
.csvDiffTest.test_semicolon_escape:{.csvDiffTest.chk["semicolon_escape.csv";()!();0#`]};
.csvDiffTest.test_semicolon_quote:{.csvDiffTest.chk["semicolon_quote.csv";()!();0#`]};
.csvDiffTest.test_shift_jis:{.csvDiffTest.chk["shift_jis.csv";()!();`bytes]};
.csvDiffTest.test_simple:{.csvDiffTest.chk["simple.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_simple_comma:{.csvDiffTest.chk["simple_comma.csv";(enlist `comment)!enlist ",";0#`]};
.csvDiffTest.test_simple_mid_line:{.csvDiffTest.chk["simple_mid_line.csv";(enlist `comment)!enlist "#";0#`]};
.csvDiffTest.test_simple_quoted:{.csvDiffTest.chk["simple_quoted.csv";()!();0#`]};
.csvDiffTest.test_simple_unterminated_quote:{.csvDiffTest.chk["simple_unterminated_quote.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_single_column_notquoted_newline:{.csvDiffTest.chk["single_column_notquoted_newline.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_single_column_quoted_newline:{.csvDiffTest.chk["single_column_quoted_newline.csv";()!();0#`]};
.csvDiffTest.test_single_header:{.csvDiffTest.chk["single_header.csv";()!();0#`]};
.csvDiffTest.test_single_line:{.csvDiffTest.chk["single_line.csv";()!();0#`]};
.csvDiffTest.test_single_numeric:{.csvDiffTest.chk["single_numeric.csv";()!();0#`]};
.csvDiffTest.test_single_quote:{.csvDiffTest.chk["single_quote.csv";(enlist `quote)!enlist "'";0#`]};
.csvDiffTest.test_single_quote_backslash:{.csvDiffTest.chk["single_quote_backslash.csv";`quote`escape!("'";"\\");0#`]};
.csvDiffTest.test_single_value:{.csvDiffTest.chk["single_value.csv";()!();0#`]};
.csvDiffTest.test_skip_header:{.csvDiffTest.chk["skip_header.csv";()!();0#`]};
.csvDiffTest.test_skip_row:{.csvDiffTest.chk["skip_row.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_small1:{.csvDiffTest.chk["small1.csv";()!();0#`]};
.csvDiffTest.test_small2:{.csvDiffTest.chk["small2.csv";()!();0#`]};
.csvDiffTest.test_small_lineitem_strings:{.csvDiffTest.chk["small_lineitem_strings.csv";()!();0#`]};
.csvDiffTest.test_small_mix:{.csvDiffTest.chk["small_mix.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_some_escaped_some_not:{.csvDiffTest.chk["some_escaped_some_not.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_split_part:{.csvDiffTest.chk["split_part.csv";()!();0#`]};
.csvDiffTest.test_stat_stat_chars:{.csvDiffTest.chk["stat_stat_chars.csv";()!();0#`]};
.csvDiffTest.test_struct:{.csvDiffTest.chk["struct.csv";()!();0#`]};
.csvDiffTest.test_struct_padding:{.csvDiffTest.chk["struct_padding.csv";()!();0#`]};
.csvDiffTest.test_tab:{.csvDiffTest.chk["tab.csv";()!();0#`]};
.csvDiffTest.test_tab_single_quote:{.csvDiffTest.chk["tab_single_quote.csv";(enlist `quote)!enlist "'";0#`]};
.csvDiffTest.test_tab_single_quote_varchar:{.csvDiffTest.chk["tab_single_quote_varchar.csv";(enlist `quote)!enlist "'";0#`]};
.csvDiffTest.test_test:{.csvDiffTest.chk["test.csv";()!();0#`]};
.csvDiffTest.test_test_commit_rollback:{.csvDiffTest.chk["test_commit_rollback.csv";()!();0#`]};
.csvDiffTest.test_test_fuzz_4234:{.csvDiffTest.chk["test_fuzz_4234.csv";()!();0#`]};
.csvDiffTest.test_test_ignore_errors:{.csvDiffTest.chk["test_ignore_errors.csv";()!();enlist `clock]};
.csvDiffTest.test_test_incompatible_type_with_nullable:{.csvDiffTest.chk["test_incompatible_type_with_nullable.csv";()!();0#`]};
.csvDiffTest.test_test_multiple_columns:{.csvDiffTest.chk["test_multiple_columns.csv";()!();0#`]};
.csvDiffTest.test_test_multiple_columns_rn:{.csvDiffTest.chk["test_multiple_columns_rn.csv";()!();0#`]};
.csvDiffTest.test_test_null_csv:{.csvDiffTest.chk["test_null_csv.csv";()!();0#`]};
.csvDiffTest.test_test_null_option:{.csvDiffTest.chk["test_null_option.csv";()!();0#`]};
.csvDiffTest.test_test_pipe:{.csvDiffTest.chk["test_pipe.csv";()!();0#`]};
.csvDiffTest.test_test_single_column:{.csvDiffTest.chk["test_single_column.csv";()!();0#`]};
.csvDiffTest.test_test_single_column_rn:{.csvDiffTest.chk["test_single_column_rn.csv";()!();0#`]};
.csvDiffTest.test_teste1:{.csvDiffTest.chk["teste1.csv";()!();0#`]};
.csvDiffTest.test_teste2:{.csvDiffTest.chk["teste2.csv";()!();0#`]};
.csvDiffTest.test_thijs_unquoted:{.csvDiffTest.chk["thijs_unquoted.csv";(enlist `quote)!enlist "";0#`]};
.csvDiffTest.test_thousands_broken:{.csvDiffTest.chk["thousands_broken.csv";()!();0#`]};
.csvDiffTest.test_time:{.csvDiffTest.chk["time.csv";()!();enlist `clock]};
.csvDiffTest.test_time_date_timestamp:{.csvDiffTest.chk["time_date_timestamp.csv";()!();enlist `clock]};
.csvDiffTest.test_time_date_timestamp_dd_mm_yyyy:{.csvDiffTest.chk["time_date_timestamp_dd-mm-yyyy.csv";
    `dateformat`timestampformat!("%d-%m-%Y";"%d-%m-%Y %H:%M:%S");enlist `clock]};
.csvDiffTest.test_time_date_timestamp_trailing:{.csvDiffTest.chk["time_date_timestamp_trailing.csv";()!();enlist `clock]};
.csvDiffTest.test_time_date_timestamp_yy_mm_dd:{.csvDiffTest.chk["time_date_timestamp_yy.mm.dd.csv";
    `dateformat`timestampformat!("%y.%m.%d";"%y.%m.%d %H:%M:%S");enlist `clock]};
.csvDiffTest.test_time_date_timestamp_yyyy_mm_dd:{.csvDiffTest.chk["time_date_timestamp_yyyy.mm.dd.csv";()!();enlist `clock]};
.csvDiffTest.test_timestamp:{.csvDiffTest.chk["timestamp.csv";()!();0#`]};
.csvDiffTest.test_timestampformat:{.csvDiffTest.chk["timestampformat.csv";()!();0#`]};
.csvDiffTest.test_too_many_values:{.csvDiffTest.chk["too_many_values.csv";()!();0#`]};
.csvDiffTest.test_trailing_delimiter_complex:{.csvDiffTest.chk["trailing_delimiter_complex.csv";()!();0#`]};
.csvDiffTest.test_ubn1:{.csvDiffTest.chk["ubn1.csv";()!();0#`]};
.csvDiffTest.test_ubn3:{.csvDiffTest.chk["ubn3.csv";()!();0#`]};
.csvDiffTest.test_ubn4:{.csvDiffTest.chk["ubn4.csv";()!();0#`]};
.csvDiffTest.test_undetected_type:{.csvDiffTest.chk["undetected_type.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_unescaped_quote:{.csvDiffTest.chk["unescaped_quote.csv";()!();0#`]};
.csvDiffTest.test_unescaped_quote_new_line:{.csvDiffTest.chk["unescaped_quote_new_line.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unescaped_quote_new_line_rn:{.csvDiffTest.chk["unescaped_quote_new_line_rn.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unionbyname_21248_2:{.csvDiffTest.chk["unionbyname_21248_2.csv";()!();`blankline]};
.csvDiffTest.test_unnamed_columns:{.csvDiffTest.chk["unnamed_columns.csv";()!();0#`]};
.csvDiffTest.test_unquote_without_delimiter:{.csvDiffTest.chk["unquote_without_delimiter.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unquoted_cast:{.csvDiffTest.chk["unquoted_cast.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unquoted_last_value:{.csvDiffTest.chk["unquoted_last_value.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unquoted_less:{.csvDiffTest.chk["unquoted_less.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_unquoted_maxline:{.csvDiffTest.chk["unquoted_maxline.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unquoted_more:{.csvDiffTest.chk["unquoted_more.csv";(enlist `ignore_errors)!enlist 1b;0#`]};
.csvDiffTest.test_unquoted_new_line:{.csvDiffTest.chk["unquoted_new_line.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unterminated:{.csvDiffTest.chk["unterminated.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unterminated_escape:{.csvDiffTest.chk["unterminated_escape.csv";()!();enlist `overtolerant]};
.csvDiffTest.test_unterminated_escape_complex:{.csvDiffTest.chk["unterminated_escape_complex.csv";()!();0#`]};
.csvDiffTest.test_unterminated_quote_escape:{.csvDiffTest.chk["unterminated_quote_escape.csv";()!();0#`]};
.csvDiffTest.test_unterminated_quote_escape_complex:{.csvDiffTest.chk["unterminated_quote_escape_complex.csv";()!();0#`]};
.csvDiffTest.test_unterminated_quote_multi_line:{.csvDiffTest.chk["unterminated_quote_multi_line.csv";(enlist `strict_mode)!enlist 0b;0#`]};
.csvDiffTest.test_unterminated_quote_with_escape:{.csvDiffTest.chk["unterminated_quote_with_escape.csv";()!();0#`]};
.csvDiffTest.test_unterminated_quote_with_escape_complex:{.csvDiffTest.chk["unterminated_quote_with_escape_complex.csv";()!();0#`]};
.csvDiffTest.test_utf16:{.csvDiffTest.chk["utf16.csv";()!();0#`]};
.csvDiffTest.test_utf8bom:{.csvDiffTest.chk["utf8bom.csv";()!();0#`]};
.csvDiffTest.test_varchar_multi_line:{.csvDiffTest.chk["varchar_multi_line.csv";()!();0#`]};
.csvDiffTest.test_varchar_single_line:{.csvDiffTest.chk["varchar_single_line.csv";()!();0#`]};
.csvDiffTest.test_various_time_formats:{.csvDiffTest.chk["various_time_formats.csv";()!();enlist `clock]};
.csvDiffTest.test_venue_pipe:{.csvDiffTest.chk["venue_pipe.csv";()!();0#`]};
.csvDiffTest.test_vsize:{.csvDiffTest.chk["vsize.csv";()!();0#`]};
.csvDiffTest.test_web_page:{.csvDiffTest.chk["web_page.csv";()!();0#`]};
.csvDiffTest.test_windows_newline:{.csvDiffTest.chk["windows_newline.csv";()!();0#`]};
.csvDiffTest.test_working:{.csvDiffTest.chk["working.csv";()!();0#`]};
.csvDiffTest.test_wrongtype:{.csvDiffTest.chk["wrongtype.csv";()!();0#`]};

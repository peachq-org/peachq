/ The two assertions qunit gained on 2026-08-12, tested through qunit itself.
/ checkTable is the table comparison assertEquals always did (columns, row
/ count) plus meta and keys; assertAlmostEquals is exact match with a tolerance
/ on the float lanes, recursing through tables, dicts and lists.
system "d .qunitTest";

/ doCheck THROWS on a failed check AND leaves failFlag set, so an assertion that
/ must FAIL can only be observed by switching the throw off — and both globals
/ have to be put back, or this test's own verdict is the one that changes.
didFail:{[assertion]
    ie:.qunit.ignoreAllExceptions; ff:.qunit.failFlag;
    .qunit.ignoreAllExceptions:1b; .qunit.failFlag:0b;
    @[assertion; ::; ::];
    r:.qunit.failFlag;
    .qunit.failFlag:ff; .qunit.ignoreAllExceptions:ie;
    r};

testCheckTableAcceptsMatchingTables:{
    .qunit.assertFalse[didFail {.qunit.checkTable[([] a:1 2; b:`x`y); ([] a:1 2; b:`x`y)]}; "a matching pair checks clean"];
    .qunit.assertEquals[([] a:1 2; b:`x`y); ([] a:1 2; b:`x`y); "assertEquals still passes an equal table through checkTable"]};

testCheckTableRejectsShapeDifferences:{
    .qunit.assertTrue[didFail {.qunit.checkTable[([] a:1 2); ([] b:1 2)]}; "different column names fail"];
    .qunit.assertTrue[didFail {.qunit.checkTable[([] a:1 2); ([] a:1 2 3)]}; "different row counts fail"]};

/ meta and keys are what this checkTable ADDS: two tables can carry the same
/ column names and the same row count and still not be the same table.
testCheckTableRejectsMetaDifference:{
    .qunit.assertTrue[didFail {.qunit.checkTable[([] a:1 2); ([] a:1 2.0)]}; "one column name, two column types is a meta difference"]};

testCheckTableRejectsKeyDifference:{
    .qunit.assertTrue[didFail {.qunit.checkTable[([] a:1 2; b:3 4); ([a:1 2] b:3 4)]}; "a keyed expected does not match an unkeyed actual"]};

testCheckTableRejectsNonTables:{
    .qunit.assertTrue[didFail {.qunit.checkTable[7; ([] a:1 2)]}; "a non-table actual fails the check"];
    .qunit.assertThrows[.qunit.checkTable[([] a:1 2);]; 7; "Expected must be table"; "a non-table expected is a usage error, not a failed check"]};

testAlmostEqualsIdenticalShortCircuits:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[`a`b; `a`b; 0.5]; `a`b; "a match returns actual without reaching the tolerance"]};

testAlmostEqualsFloatTolerance:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[1.0; 1.4; 0.5]; 1.0; "a difference inside the tolerance passes, returning actual"];
    .qunit.assertEquals[.qunit.assertAlmostEquals[1.0; 1.5; 0.5]; 1.0; "the tolerance itself is inclusive"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[1.0; 1.6; 0.5]}; "a difference outside the tolerance fails"]};

/ a null is not a distance from anything, so it matches null and nothing else
testAlmostEqualsNulls:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[1 0n 3.0; 1.4 0n 3.0; 0.5]; 1 0n 3.0; "a null in both lanes matches"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[1 0n 3.0; 1 2 3.0; 0.5]}; "a null against a value fails"]};

testAlmostEqualsExactFallback:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[1 2 3; 1 2 3; 0.5]; 1 2 3; "equal ints pass"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[1 2 3; 1 2 4; 2.0]}; "ints inside the tolerance still fail: a non-float lane is exact match"]};

/ every branch past the table one broadcasts or throws on a shape mismatch, so
/ without a shape check a float atom compares clean against a float vector
testAlmostEqualsRejectsShapeMismatch:{
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[1.0; 1.1 1.2; 0.5]}; "a float atom is not a one-element-per-lane match for a vector"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[1 2.0; 1 2 3.0; 0.5]}; "float vectors of different length fail rather than raising length"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[(1.0;2.0); (1.0;2.0;3.0); 0.5]}; "general lists of different length fail rather than raising length"]};

/ every branch answers the actual object, as the other assertions do
testAlmostEqualsRecursesThroughDicts:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[`a`b!1 2.0; `a`b!1.4 2.0; 0.5]; `a`b!1 2.0; "a dict compares value by value and returns actual"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[`a`b!1 2.0; `a`c!1 2.0; 0.5]}; "different dict keys fail"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[`a`b!1 2.0; `a`b!1 9.0; 0.5]}; "a dict value outside the tolerance fails"]};

testAlmostEqualsRecursesThroughTables:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[([] a:1 2.0; b:`x`y); ([] a:1.4 2.0; b:`x`y); 0.5]; ([] a:1 2.0; b:`x`y); "a table compares column by column and returns actual"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[([] a:1 2.0); ([] a:1 9.0); 0.5]}; "a table column outside the tolerance fails"]};

testAlmostEqualsRecursesThroughLists:{
    .qunit.assertEquals[.qunit.assertAlmostEquals[(1.0; 2 3.0); (1.4; 2 3.0); 0.5]; (1.0; 2 3.0); "a general list compares element by element and returns actual"];
    .qunit.assertTrue[didFail {.qunit.assertAlmostEquals[(1.0; 2 3.0); (1.9; 2 3.0); 0.5]}; "one element outside the tolerance fails the list"]};

/ source: TimeStored qunit, imported 2026-08-06
/ QUnit testing mathematical functions
system "d .mathTest";

testAdd:{.qunit.assertEquals[.math.add[2;2]; 4; "2 plus 2 equals four"]};

testKnownValue:{ .qunit.assertKnown[0; `testKnownValue; "known value matches the golden file"] };

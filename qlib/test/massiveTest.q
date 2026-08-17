/ The one LIVE .massive gate: every assertion goes over the real Massive REST API, so a renamed
/ field or a changed type upstream fails the suite. Needs $MASSIVE_API_KEY exported; unset, the
/ suite fails and names it. Deliberately no fixture and no offline mode — a stored payload proves
/ only that we still parse the bytes captured last month. Endpoints, against the published Massive
/ REST reference: /v1/marketstatus/now, /v2/aggs/ticker/{t}/range, /v3/reference/tickers. The
/ offline laws of .massive are pinned by test/q/massive/*.qcmd against a stubbed transport.
system "d .massiveTest";

/ a length, never the key: assertThat prints `actual` into the report. It is also this namespace's
/ only non-boolean result, which peachq needs until table `,` widens a typed column (PLAN.md).
testApiKeyIsExported:{
    .qunit.assertThat[count .massive.apikey; >; 0;
        "MASSIVE_API_KEY is not exported — the live massive tier cannot run"]};

testMarketStatus:{
    s:.massive.status[];
    .qunit.assertEquals[type s; 99h; "/v1/marketstatus/now answers one dict, never a list of rows"];
    .qunit.assertTrue[`market in key s; "the status dict carries the overall market state"]};

/ a fixed PAST week, so the response set is stable and no price, volume or timestamp is ever asserted
testFetchAggregates:{
    r:.massive.fetch "/v2/aggs/ticker/AAPL/range/1/day/2026-07-13/2026-07-17?adjusted=true";
    .qunit.assertEquals[type r; 98h; "a results list of uniform dicts converts to one table"];
    .qunit.assertThat[count r; >; 0; "a full trading week of daily bars comes back non-empty"];
    .qunit.assertTrue[all `o`h`l`c`v`t in cols r; "the documented OHLCV aggregate fields all arrive"];
    .qunit.assertEquals[type r`t; 12h; "the epoch-millisecond t column coerces to timestamp"];
    .qunit.assertTrue[`status in key .massive.envelope; "the raw envelope survives the convert"]};

/ .massive.envelope is a process global that every call overwrites and qunit fixes no test order,
/ so a test reads it in the same body as the call it belongs to
testPagedTickers:{
    t:.massive.tickers ``limit`maxpages`max!(`;5;2;1000);
    .qunit.assertEquals[type t; 98h; "a walk answers one table, never a list of pages"];
    .qunit.assertEquals[count t; 10; "two pages of five accumulate into one ten-row table"];
    .qunit.assertTrue[`ticker in cols t; "every reference row carries its ticker"];
    .qunit.assertTrue[.massive.envelope`truncated; "a walk stopped by maxpages says so"];
    .qunit.assertTrue[`next_url in key .massive.envelope; "and leaves the outstanding cursor readable"]};

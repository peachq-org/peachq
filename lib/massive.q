/ Market data from the Massive REST API (massive.com) as q tables: bars, daily aggregates, snapshots, trades,
/ quotes, tickers, splits, dividends, IPOs and news. Export MASSIVE_API_KEY before starting q, or call
/ .massive.setKey. Every call that takes options takes them last, as a dict (()!() or :: for none): `max and
/ `maxpages bound the paging (10000 rows, 10 pages), any other key goes to the API as a query parameter, so `limit
/ is the API's page size and `max the rows you want; a walk that stopped with pages outstanding says so on stderr
/ and sets .massive.envelope`truncated.
/ .
/ Timestamps: `t is the start of the bar in UTC (a daily bar stamps midnight Eastern, 04:00 or 05:00 UTC); the
/ snapshot's updated and the ticks' sip/participant/trf stamps are UTC too; *_utc text becomes a timestamp and, in
/ a table, an ISO date a date. Numbers come as the API writes them (a long for 12, a float for 5.3e+07); tickers
/ are strings (`$ for symbols). Top-level columns only: a snapshot's nested day/prevDay dicts arrive as written.
/ No rows (a weekend, an unknown ticker) is (); a refusal signals "STATUS: message" (bad key, bad date, rate limit)
/ and the whole response, status included, is in .massive.envelope.
/ .
/ @eg
/ \l pq
/ .massive.setKey "your-api-key"
/ r:.massive.bars[`AAPL;2026.08.01;2026.09.04;()!()]
/ 1-min r[`c]%maxs r`c                                          / the max drawdown over the range
/ .massive.bars[`AAPL;2026.09.04;2026.09.04;`mult`span!(5;`minute)]
/ n:.massive.news[`ticker`max!(`AAPL;20)]; .massive.envelope`truncated
/ `$(.massive.tickers[(enlist `search)!enlist "apple"])`ticker

/ The API base URL.
.massive.url:"https://api.massive.com";
/ The API key, read from MASSIVE_API_KEY at load.
.massive.apikey:getenv `MASSIVE_API_KEY;
/ Convert the epoch (.massive.tcols) and ISO (*_utc) columns to q timestamps on the way in; 0b keeps them raw.
.massive.times:1b;
/ @ignore
.massive.i.nul:(0#`)!();
/ The last response's envelope: status, count, next_url and, after a paged call, truncated.
.massive.envelope:.massive.i.nul;

/ Set the API key for this session.
.massive.setKey:{[apikey] .massive.apikey::apikey;};

/ @ignore
.massive.i.fmt:{[v] t:type v;
  $[10h=t;  v;
    -11h=t; string v;
    -1h=t;  $[v;"true";"false"];
    -14h=t; ssr[string v;".";"-"];
    11h=t;  "," sv string v;
    0h=t;   "," sv .massive.i.fmt each v;
    string v]};

/ @ignore
.massive.qs:{[d] "&" sv {[k;v] (.h.hu string k),"=",.h.hu .massive.i.fmt v}'[key d;value d]};
/ @ignore
.massive.i.sep:{[u] u,$[any "?"=u;"&";"?"]};
/ @ignore
.massive.i.absurl:{[p] $[p like "http*";p;.massive.url,p]};
/ @ignore
.massive.i.withkey:{[u] (.massive.i.sep u),"apiKey=",.h.hu .massive.apikey};

/ The response statuses accepted as success; any other is signalled as "STATUS: message" (the API's error or message).
.massive.ok:`OK`DELAYED;
/ @ignore
.massive.check:{[r]
  if[99h<>type r; :r];
  if[not `status in key r; :r];
  s:r`status;
  if[not 10h=type s; :r];
  if[(`$s) in .massive.ok; :r];
  m:r (`error`message) inter key r;
  m:m where (10h=type each m) and 0<count each m;
  's,$[count m;": ",(200&count first m)#first m;""]};

/ The response keys whose value is the payload, in the order tried.
.massive.pay:`results`tickers;

/ @ignore
.massive.i.paykey:{[r] .massive.pay where .massive.pay in key r};

/ @ignore
.massive.i.pay:{[resp;k]
  v:resp[1] k;
  $[(98h=type v) or (0h=type v) and count[v] and all 99h=type each v;
    .j.read[resp 0;::;::;(enlist `path)!enlist k]; v]};

/ @ignore
.massive.i.merge:{[ts]
  ts:ts where 0<count each ts;
  $[0=count ts;();all 98h=type each ts;(uj/) ts;raze ts]};

/ @ignore
.massive.i.norows:{[r] any 0=r (`resultsCount`count) inter key r};

/ @ignore
.massive.convert:{[resp]
  k:.massive.i.paykey resp 1;
  $[count k; .massive.coerce .massive.i.pay[resp;first k]; .massive.i.norows resp 1; (); .massive.coerce resp 1]};

/ The response columns holding epoch timestamps (ms or ns, told apart by magnitude): bars, snapshots and ticks.
.massive.tcols:`t`updated`sip_timestamp`participant_timestamp`trf_timestamp;
/ @ignore
.massive.i.ns:1000000000000000;
/ @ignore
.massive.i.ep:"j"$1970.01.01D00:00:00.000000000;
/ @ignore
.massive.i.cnum:{[v] v:"j"$v; "p"$.massive.i.ep+?[.massive.i.ns<abs v;v;1000000*v]};
/ @ignore
.massive.i.ciso:{[v] $[10h=type v;"P"$ssr[ssr[ssr[v;"-";"."];"T";"D"];"Z";""];null v;0Np;'type]};

/ @ignore
.massive.i.ccol:{[c;v]
  if[(c in .massive.tcols) and type[v] in 6 7 9h; :.massive.i.cnum v];
  if[c like "*_utc";
    if[type[v] in -9 10h; :.massive.i.ciso v];
    if[type[v] in 0 9h; :.massive.i.ciso each v]];
  v};

/ @ignore
.massive.coerce:{[x]
  if[not .massive.times; :x];
  if[98h=type x; :flip (cols x)!.massive.i.ccol'[cols x;value flip x]];
  if[99h=type x; :(key x)!.massive.i.ccol'[key x;value x]];
  x};

/ @ignore
.massive.i.raw:{[x]
  a:$[10h=type x;(x;.massive.i.nul);x];
  u:.massive.i.absurl a 0;
  q:.massive.qs a 1;
  u:$[count q;(.massive.i.sep u),q;u];
  t:.Q.hg .massive.i.withkey u;
  .massive.envelope::@[.j.k;t;{[body;e] `status`error!("ERROR";body)}[t]];
  .massive.check .massive.envelope;
  (t;.massive.envelope)};

/ GET any API path and answer its payload: a table for a list of records, a dict for a single record (one page, no
/ paging). The key is appended, so hand it a path under .massive.url or a next_url read from .massive.envelope.
/ @param request a path string, or (path;params dict)
/ @eg .massive.fetch "/v3/reference/tickers/AAPL"                                    / ticker details
/ @eg .massive.fetch ("/v3/reference/tickers";(enlist `limit)!enlist 5)
/ @eg .massive.fetch .massive.envelope`next_url                                    / the page after a truncated walk
.massive.fetch:{[request] .massive.convert .massive.i.raw request};

/ The options every call starts from: the walk stops after `max rows or `maxpages pages, checked between pages, so
/ the first page always arrives and a page that arrived is kept whole (the result may exceed `max).
.massive.defaults:`max`maxpages!(10000;10);
/ @ignore
.massive.i.ctl:`max`maxpages;

/ @ignore
.massive.opts:{[d;o]
  if[not 99h=type o; :d];
  k:key[o] except `;
  $[0=count k;d;d,k#o]};
/ @ignore
.massive.i.params:{[o] (key[o] except .massive.i.ctl)#o};
/ @ignore
.massive.i.nexturl:{[] $[`next_url in key .massive.envelope;.massive.envelope`next_url;""]};

/ @ignore
.massive.i.done:{[tr;n]
  .massive.envelope::.massive.envelope,(enlist `truncated)!enlist tr;
  if[tr; -2 "massive: stopped after ",(string n)," page(s) with a next_url outstanding - it is in .massive.envelope"];};

/ @ignore
.massive.i.walk:{[x;o]
  r:.massive.i.raw x;
  k:.massive.i.paykey r 1;
  u:.massive.i.nexturl[];
  if[(0=count u) or 0=count k; .massive.i.done[0b;1]; :.massive.convert r];
  acc:enlist .massive.i.pay[r;first k];
  n:1;
  while[(0<count u) and (n<o`maxpages) and (o`max)>sum count each acc;
    r:.massive.i.raw u;
    k:.massive.i.paykey r 1;
    acc:$[count k;acc,enlist .massive.i.pay[r;first k];acc];
    n+:1; u:.massive.i.nexturl[]];
  .massive.i.done[0<count u;n];
  .massive.coerce .massive.i.merge acc};

/ @ignore
.massive.i.paged:{[p;o;extra]
  o:.massive.opts[.massive.defaults;o];
  .massive.i.walk[(p;extra,.massive.i.params o);o]};

/ Every US stock's bar for one trading date (a weekend or holiday answers ()); `T is the ticker.
/ @param adjusted 1b for split-adjusted prices
/ @eg .massive.daily[2026.09.04;1b;()!()]
.massive.daily:{[date;adjusted;opts] .massive.i.paged["/v2/aggs/grouped/locale/us/market/stocks/",.massive.i.fmt date;opts;(enlist `adjusted)!enlist adjusted]};

/ The bar options: `mult and `span set the bar size; `adjusted and `limit (the base aggregates one page is built
/ from, max 50000) pass to the API.
.massive.bardefaults:`mult`span`adjusted`limit!(1;`day;1b;5000);
/ Bars for a ticker between two dates, both inclusive.
/ @param opts `span (`minute`hour`day`week`month`quarter`year) and `mult set the bar size: `mult`span!(5;`minute)
/ is 5-minute bars
/ @eg .massive.bars[`AAPL;2026.08.01;2026.09.04;()!()]
/ @eg .massive.bars[`AAPL;2026.09.04;2026.09.04;`mult`span!(5;`minute)]
.massive.bars:{[ticker;start;end;opts]
  opts:.massive.opts[.massive.defaults,.massive.bardefaults;opts];
  p:"/v2/aggs/ticker/",(.massive.i.fmt ticker),"/range/",(.massive.i.fmt opts`mult),"/",(.massive.i.fmt opts`span),"/",(.massive.i.fmt start),"/",.massive.i.fmt end;
  .massive.i.walk[(p;(key[opts] except .massive.i.ctl,`mult`span)#opts);opts]};

/ The previous day's open, high, low, close and volume for a ticker.
/ @eg .massive.prevclose[`AAPL;()!()]
.massive.prevclose:{[ticker;opts] .massive.i.paged["/v2/aggs/ticker/",(.massive.i.fmt ticker),"/prev";opts;.massive.i.nul]};
/ One ticker's open, high, low, close, pre-market and after-hours prices for one trading date, as a dict (a
/ non-trading date signals NOT_FOUND).
/ @eg .massive.ohlc[`AAPL;2026.09.04;()!()]
.massive.ohlc:{[ticker;date;opts] .massive.fetch ("/v1/open-close/",(.massive.i.fmt ticker),"/",.massive.i.fmt date;.massive.i.params .massive.opts[.massive.defaults;opts])};
/ The current snapshot (today's bar, the last minute, the previous day) for a list of tickers.
/ @eg .massive.snap[`AAPL`MSFT;()!()]
.massive.snap:{[tickers;opts] .massive.i.paged["/v2/snapshot/locale/us/markets/stocks/tickers";opts;(enlist `tickers)!enlist .massive.i.fmt tickers]};
/ The reference list of tickers.
/ @param opts API filters such as `market`exchange`search`active
/ @eg .massive.tickers[`search`active!("apple";1b)]
.massive.tickers:{[opts] .massive.i.paged["/v3/reference/tickers";opts;.massive.i.nul]};
/ Stock splits.
/ @param opts API filters such as `ticker`execution_date
/ @eg .massive.splits[(enlist `ticker)!enlist `AAPL]
.massive.splits:{[opts] .massive.i.paged["/v3/reference/splits";opts;.massive.i.nul]};
/ Dividends.
/ @param opts API filters such as `ticker`ex_dividend_date
/ @eg .massive.divs[(enlist `ticker)!enlist `AAPL]
.massive.divs:{[opts] .massive.i.paged["/v3/reference/dividends";opts;.massive.i.nul]};
/ IPOs, newest first.
/ @param opts API filters such as `ticker`listing_date`ipo_status
/ @eg .massive.ipos[`limit`max!(50;50)]
.massive.ipos:{[opts] .massive.i.paged["/vX/reference/ipos";opts;.massive.i.nul]};
/ News articles.
/ @param opts API filters such as `ticker`published_utc
/ @eg .massive.news[`ticker`max!(`AAPL;20)]
.massive.news:{[opts] .massive.i.paged["/v2/reference/news";opts;.massive.i.nul]};
/ Tick-level trades for a ticker; the stamps are sip_timestamp, participant_timestamp and trf_timestamp. Needs a
/ plan that includes tick data (NOT_AUTHORIZED otherwise).
/ @param opts `timestamp (a date) selects the day, `limit the page size (max 50000)
/ @eg .massive.trades[`AAPL;`timestamp`limit!(2026.09.04;100)]
.massive.trades:{[ticker;opts] .massive.i.paged["/v3/trades/",.massive.i.fmt ticker;opts;.massive.i.nul]};
/ Tick-level quotes for a ticker; the same stamps and plan requirement as .massive.trades.
/ @param opts `timestamp (a date) selects the day, `limit the page size (max 50000)
/ @eg .massive.quotes[`AAPL;`timestamp`limit!(2026.09.04;100)]
.massive.quotes:{[ticker;opts] .massive.i.paged["/v3/quotes/",.massive.i.fmt ticker;opts;.massive.i.nul]};
/ Whether the market is open now: the exchanges, their state and the server time, as a dict.
/ @eg .massive.status[]
.massive.status:{[] .massive.fetch "/v1/marketstatus/now"};

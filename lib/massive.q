/ Market data from the Massive REST API (massive.com) as q tables: bars, daily aggregates, snapshots, trades,
/ quotes, tickers, splits, dividends, IPOs and news.  Set MASSIVE_API_KEY in the environment or call .massive.setKey.
/ The last argument of every call is an options dict, ()!() for none: `max and `maxpages bound the paging, any
/ other key is passed to the API as a query parameter.
/ .
/ @eg
/ .massive.setKey "your-api-key"
/ .massive.bars[`AAPL;2026.08.01;2026.09.05;()!()]
/ .massive.tickers[(enlist `search)!enlist "apple"]

/ The API base URL.
.massive.url:"https://api.massive.com";
/ The API key, read from MASSIVE_API_KEY at load.
.massive.apikey:getenv `MASSIVE_API_KEY;
/ Convert epoch timestamp columns (t, updated, *_utc) to q timestamps on the way in.
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

/ The response statuses accepted as success; any other status is signalled verbatim.
.massive.ok:`OK`DELAYED;
/ @ignore
.massive.check:{[r]
  if[99h<>type r; :r];
  if[not `status in key r; :r];
  s:r`status;
  if[not 10h=type s; :r];
  if[not (`$s) in .massive.ok; '`$s];
  r};

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
.massive.convert:{[resp]
  k:.massive.i.paykey resp 1;
  .massive.coerce $[count k; .massive.i.pay[resp;first k]; resp 1]};

/ The response columns holding epoch timestamps (ms or ns, told apart by magnitude).
.massive.tcols:`t`updated;
/ @ignore
.massive.i.ns:1000000000000000;
/ @ignore
.massive.i.ep:"j"$1970.01.01D00:00:00.000000000;
/ @ignore
.massive.i.cnum:{[v] v:"j"$v; "p"$.massive.i.ep+?[.massive.i.ns<abs v;v;1000000*v]};
/ @ignore
.massive.i.ciso:{[v] "P"$ssr[ssr[ssr[v;"-";"."];"T";"D"];"Z";""]};

/ @ignore
.massive.i.ccol:{[c;v]
  if[(c in .massive.tcols) and type[v] in 6 7 9h; :.massive.i.cnum v];
  if[(c like "*_utc") and 0h=type v; :.massive.i.ciso each v];
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
  .massive.envelope::.j.k t;
  .massive.check .massive.envelope;
  (t;.massive.envelope)};

/ GET any API path and answer its payload as a table (one page, no paging).
/ @param request a path string, or (path;params dict)
/ @eg .massive.fetch ("/v3/reference/tickers";(enlist `limit)!enlist 5)
.massive.fetch:{[request] .massive.convert .massive.i.raw request};

/ The default options every call starts from: paging stops after `max rows or `maxpages pages (a page that
/ arrived is kept whole, so the result may exceed `max).
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

/ Every US stock's daily bar for one date.
/ @param adjusted 1b for split-adjusted prices
/ @eg .massive.daily[2026.09.05;1b;()!()]
.massive.daily:{[date;adjusted;opts] .massive.i.paged["/v2/aggs/grouped/locale/us/market/stocks/",.massive.i.fmt date;opts;(enlist `adjusted)!enlist adjusted]};

/ The default bar options: `mult and `span set the bar size, `adjusted and `limit pass to the API.
.massive.bardefaults:`mult`span`adjusted`limit!(1;`day;1b;5000);
/ Bars for a ticker between two dates.
/ @param opts `span (`day `hour `minute) and `mult set the bar size: `mult`span!(5;`minute) is 5-minute bars
/ @eg .massive.bars[`AAPL;2026.08.01;2026.09.05;()!()]
/ @eg .massive.bars[`AAPL;2026.09.05;2026.09.05;`mult`span!(5;`minute)]
.massive.bars:{[ticker;start;end;opts]
  opts:.massive.opts[.massive.defaults,.massive.bardefaults;opts];
  p:"/v2/aggs/ticker/",(.massive.i.fmt ticker),"/range/",(.massive.i.fmt opts`mult),"/",(.massive.i.fmt opts`span),"/",(.massive.i.fmt start),"/",.massive.i.fmt end;
  .massive.i.walk[(p;(key[opts] except .massive.i.ctl,`mult`span)#opts);opts]};

/ The previous day's open, high, low, close and volume for a ticker.
/ @eg .massive.prevclose[`AAPL;()!()]
.massive.prevclose:{[ticker;opts] .massive.i.paged["/v2/aggs/ticker/",(.massive.i.fmt ticker),"/prev";opts;.massive.i.nul]};
/ One ticker's open, high, low and close for one date.
/ @eg .massive.ohlc[`AAPL;2026.09.05;()!()]
.massive.ohlc:{[ticker;date;opts] .massive.fetch ("/v1/open-close/",(.massive.i.fmt ticker),"/",.massive.i.fmt date;.massive.i.params .massive.opts[.massive.defaults;opts])};
/ The current snapshot (last trade, last quote, today's bar) for a list of tickers.
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
/ IPOs.
/ @param opts API filters such as `ticker`listing_date
.massive.ipos:{[opts] .massive.i.paged["/vX/reference/ipos";opts;.massive.i.nul]};
/ News articles.
/ @param opts API filters such as `ticker`published_utc
/ @eg .massive.news[`ticker`max!(`AAPL;20)]
.massive.news:{[opts] .massive.i.paged["/v2/reference/news";opts;.massive.i.nul]};
/ Tick-level trades for a ticker.
/ @param opts API filters such as `timestamp`limit bound the range
/ @eg .massive.trades[`AAPL;`timestamp`limit!(2026.09.05;100)]
.massive.trades:{[ticker;opts] .massive.i.paged["/v3/trades/",.massive.i.fmt ticker;opts;.massive.i.nul]};
/ Tick-level quotes for a ticker.
/ @param opts API filters such as `timestamp`limit bound the range
/ @eg .massive.quotes[`AAPL;`timestamp`limit!(2026.09.05;100)]
.massive.quotes:{[ticker;opts] .massive.i.paged["/v3/quotes/",.massive.i.fmt ticker;opts;.massive.i.nul]};
/ Whether the market is open now: the exchanges, their state and the server time.
/ @eg .massive.status[]
.massive.status:{[] .massive.fetch "/v1/marketstatus/now"};

/ massive.q — q wrapper over the Massive market-data REST API.  \l lib/massive.q
/ Paths are passed WHOLE by the caller (the live surface mixes /v1, /v2 and /v3),
/ so no version prefix is ever baked into the transport.

.massive.url:"https://api.massive.com";
.massive.apikey:getenv `MASSIVE_API_KEY;
.massive.times:1b;
.massive.i.nul:(0#`)!();
.massive.envelope:.massive.i.nul;

.massive.setKey:{.massive.apikey::x;};

/ .h.hu's own map, indexed by CODE: the char-keyed lookup never matches " "
/ (PLAN.md defect), silently leaving spaces unescaped.  Delete once find is fixed.
.massive.i.esc:value .h.hug .h.sc;
.massive.enc:{[s] i:"i"$s; raze .massive.i.esc i+256*i<0};

.massive.i.fmt:{[v] t:type v;
  $[10h=t;  v;
    -11h=t; string v;
    -1h=t;  $[v;"true";"false"];
    -14h=t; ssr[string v;".";"-"];
    11h=t;  "," sv string v;
    0h=t;   "," sv .massive.i.fmt each v;
    string v]};

.massive.qs:{[d] "&" sv {[k;v] (.massive.enc string k),"=",.massive.enc .massive.i.fmt v}'[key d;value d]};
.massive.i.sep:{[u] u,$[any "?"=u;"&";"?"]};
.massive.i.absurl:{[p] $[p like "http*";p;.massive.url,p]};
.massive.i.withkey:{[u] (.massive.i.sep u),"apiKey=",.massive.enc .massive.apikey};

/ Statuses the API returns on a good answer; anything else is signalled verbatim.
.massive.ok:`OK`DELAYED;
.massive.check:{[r]
  if[99h<>type r; :r];
  if[not `status in key r; :r];
  s:r`status;
  if[not 10h=type s; :r];
  if[not (`$s) in .massive.ok; '`$s];
  r};

/ The convert law: a payload LIST OF DICTS becomes a table (keys unioned, ragged
/ rows null-filled); one dict stays that dict; non-dicts stay a list; no payload
/ key stays the envelope dict.  Nested dicts are left NESTED.
.massive.pay:`results`tickers;

.massive.i.paykey:{[r] .massive.pay where .massive.pay in key r};
.massive.i.flat:{[rs] flip (key first rs)!flip value each rs};

/ THE ragged law, and its only home: uniform tables become one table over the union
/ of their columns, missing cells filled with the column's own null.  Ragged rows
/ within a page and ragged pages within a walk are the same irregularity at two
/ granularities, so both come here — it cannot be right at one and wrong at the other.
.massive.i.mcol:{[ts;k]
  tpl:first 0#(first ts where {[k;t] k in cols t}[k] each ts)k;
  raze {[k;tpl;t] $[k in cols t;t k;count[t]#tpl]}[k;tpl] each ts};

.massive.i.merge:{[ts]
  ts:ts where 0<count each ts;
  if[0=count ts; :()];
  if[1=count ts; :first ts];
  if[not all 98h=type each ts; :raze ts];
  ks:distinct raze cols each ts;
  flip ks!.massive.i.mcol[ts] each ks};

.massive.i.page:{[v]
  if[98h=type v; :v];
  if[not 0h=type v; :v];
  if[0=count v; :v];
  if[not all 99h=type each v; :v];
  g:value group key each v;
  (.massive.i.merge .massive.i.flat each v@/:g) iasc raze g};

.massive.convert:{[r]
  if[99h<>type r; :r];
  k:.massive.i.paykey r;
  if[0=count k; :.massive.coerce r];
  .massive.coerce .massive.i.page r first k};

/ Epoch coercion, one name list.  Aggregates carry ms, snapshots ns — told apart
/ by magnitude, not by endpoint.  ISO strings arrive under a *_utc name.
.massive.tcols:`t`updated;
.massive.i.ns:1000000000000000;
.massive.i.ep:"j"$1970.01.01D00:00:00.000000000;
.massive.i.cnum:{[v] v:"j"$v; "p"$.massive.i.ep+?[.massive.i.ns<abs v;v;1000000*v]};
.massive.i.ciso:{[v] "P"$ssr[ssr[ssr[v;"-";"."];"T";"D"];"Z";""]};

.massive.i.ccol:{[c;v]
  if[(c in .massive.tcols) and type[v] in 6 7 9h; :.massive.i.cnum v];
  if[(c like "*_utc") and 0h=type v; :.massive.i.ciso each v];
  v};

.massive.coerce:{[x]
  if[not .massive.times; :x];
  if[98h=type x; :flip (cols x)!.massive.i.ccol'[cols x;value flip x]];
  if[99h=type x; :(key x)!.massive.i.ccol'[key x;value x]];
  x};

.massive.i.raw:{[x]
  a:$[10h=type x;(x;.massive.i.nul);x];
  u:.massive.i.absurl a 0;
  q:.massive.qs a 1;
  u:$[count q;(.massive.i.sep u),q;u];
  r:.j.k .Q.hg .massive.i.withkey u;
  .massive.envelope::r;
  .massive.check r};

.massive.fetch:{[x] .massive.convert .massive.i.raw x};

/ next_url is followed internally into ONE table.  `max`/`maxpages` bound the pages
/ FETCHED, never the rows KEPT — a page that arrived whole is returned whole, so the
/ result may exceed `max` and an unpaginated response is never sliced.
.massive.defaults:`max`maxpages!(10000;10);
.massive.i.ctl:`max`maxpages;

.massive.opts:{[d;o]
  if[not 99h=type o; :d];
  k:key[o] except `;
  $[0=count k;d;d,k#o]};
.massive.i.params:{[o] (key[o] except .massive.i.ctl)#o};
.massive.i.nexturl:{[] $[`next_url in key .massive.envelope;.massive.envelope`next_url;""]};

/ Every walk exit reports through here, so `truncated` is ALWAYS readable after one.
.massive.i.done:{[tr;n]
  .massive.envelope::.massive.envelope,(enlist `truncated)!enlist tr;
  if[tr; -2 "massive: stopped after ",(string n)," page(s) with a next_url outstanding — it is in .massive.envelope"];};

.massive.i.walk:{[x;o]
  r:.massive.i.raw x;
  k:.massive.i.paykey r;
  u:.massive.i.nexturl[];
  if[(0=count u) or 0=count k; .massive.i.done[0b;1]; :.massive.convert r];
  acc:enlist .massive.i.page r first k;
  n:1;
  while[(0<count u) and (n<o`maxpages) and (o`max)>sum count each acc;
    r:.massive.i.raw u;
    k:.massive.i.paykey r;
    acc:$[count k;acc,enlist .massive.i.page r first k;acc];
    n+:1; u:.massive.i.nexturl[]];
  .massive.i.done[0<count u;n];
  .massive.coerce .massive.i.merge acc};

.massive.i.paged:{[p;o;extra]
  o:.massive.opts[.massive.defaults;o];
  .massive.i.walk[(p;extra,.massive.i.params o);o]};

.massive.daily:{[dt;adj;o] .massive.i.paged["/v2/aggs/grouped/locale/us/market/stocks/",.massive.i.fmt dt;o;(enlist `adjusted)!enlist adj]};

.massive.bardefaults:`mult`span`adjusted`limit!(1;`day;1b;5000);
.massive.bars:{[s;f;t;o]
  o:.massive.opts[.massive.defaults,.massive.bardefaults;o];
  p:"/v2/aggs/ticker/",(.massive.i.fmt s),"/range/",(.massive.i.fmt o`mult),"/",(.massive.i.fmt o`span),"/",(.massive.i.fmt f),"/",.massive.i.fmt t;
  .massive.i.walk[(p;(key[o] except .massive.i.ctl,`mult`span)#o);o]};

.massive.prevclose:{[s;o] .massive.i.paged["/v2/aggs/ticker/",(.massive.i.fmt s),"/prev";o;.massive.i.nul]};
.massive.ohlc:{[s;dt;o] .massive.fetch ("/v1/open-close/",(.massive.i.fmt s),"/",.massive.i.fmt dt;.massive.i.params .massive.opts[.massive.defaults;o])};
.massive.snap:{[syms;o] .massive.i.paged["/v2/snapshot/locale/us/markets/stocks/tickers";o;(enlist `tickers)!enlist .massive.i.fmt syms]};
.massive.tickers:{[o] .massive.i.paged["/v3/reference/tickers";o;.massive.i.nul]};
.massive.splits:{[o] .massive.i.paged["/v3/reference/splits";o;.massive.i.nul]};
.massive.divs:{[o] .massive.i.paged["/v3/reference/dividends";o;.massive.i.nul]};
.massive.ipos:{[o] .massive.i.paged["/vX/reference/ipos";o;.massive.i.nul]};
.massive.news:{[o] .massive.i.paged["/v2/reference/news";o;.massive.i.nul]};
.massive.trades:{[s;o] .massive.i.paged["/v3/trades/",.massive.i.fmt s;o;.massive.i.nul]};
.massive.quotes:{[s;o] .massive.i.paged["/v3/quotes/",.massive.i.fmt s;o;.massive.i.nul]};
.massive.status:{[] .massive.fetch "/v1/marketstatus/now"};

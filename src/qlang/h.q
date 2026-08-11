/ h.q — peachq's `.h` namespace, authored from the PUBLISHED qdocs (ref/doth.md, CC BY 4.0).
/ ALWAYS-ON: baked in by tools/gen-bootstrap.sh -> h_gen.h, loaded at q_runtime_create after
/ q.q+dotq.q. One definition per line (no LITERAL newline in one). Absent: .h.ht (doth-status.md).
/ NAMING RULE: `.h.` carries ONLY names ref/doth.md documents; everything peachq invented is `.h.i.`.

.h.br:"<br>";
.h.c0:`024C7E;
.h.c1:`958600;
.h.d:" ";
/ .h.logo: kdb ships the KX logo here; peachq carries NO KX branding — peachq's instead (divergence).
.h.logo:"<a href=\"https://peachq.org\"><img src=\"https://peachq.org/img/peachq-logo.svg\" alt=\"peachq\" height=\"28\"></a>";
/ .h.HOME: doc'd as the root path with no fixed default; pinned to what #217's static server serves.
.h.HOME:"html";
/ .h.ty: the 7 doc-listed keys carry DOC values (csv/xml/xls thus differ from peachq's C fallback — doc fidelity wins); the rest is #217's superset.
.h.ty:`htm`html`csv`txt`xml`xls`gif`css`js`mjs`png`jpg`jpeg`svg`ico`webp`json`pdf`wasm`woff`woff2!("text/html";"text/html";"text/comma-separated-values";"text/plain";"text/plain";"application/msexcel";"image/gif";"text/css";"application/javascript";"application/javascript";"image/png";"image/jpeg";"image/jpeg";"image/svg+xml";"image/x-icon";"image/webp";"application/json";"application/pdf";"application/wasm";"font/woff";"font/woff2");
/ .h.sa's own doc entry truncates its value; .h.html's HTML block prints it whole.
.h.sa:"a{text-decoration:none}a:link{color:024C7E}a:visited{color:024C7E}a:active{color:958600}";
.h.sb:"body{font:10pt verdana;text-align:justify}";
.h.sc:"$-.+!*'(),abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789";

.h.htc:{[x;y] "<",(string x),">",y,"</",(string x),">"};
.h.hta:{[x;y] "<",(string x)," ",(" " sv {[k;v] (string k),"=\"",v,"\""}'[key y;value y]),">"};
.h.htac:{[x;y;z] .h.hta[x;y],z,"</",(string x),">"};
.h.hr:{count[x]#"-"};
.h.nbr:{.h.htc[`nobr;x]};
.h.hc:{ssr[x;"<";"&lt;"]};
.h.xs:{ssr[ssr[x;"&";"&amp;"];"<";"&lt;"]};
/ .h.ha/.h.hb: a SYMBOL href is emitted bare, a STRING href quoted — both spellings doc-pinned.
.h.ha:{[x;y] "<a href=",$[-11h=type x;string x;"\"",x,"\""],">",y,"</a>"};
.h.hb:{[x;y] "<a target=v href=",$[-11h=type x;string x;"\"",x,"\""],">",y,"</a>"};
.h.html:{"<html><head><style>",.h.sa,.h.sb,"</style></head><body>",x,"</body></html>"};
.h.fram:{[x;y;z] "<html><head><title>",x,"</title><frameset cols=\"",(string 26+10*max count each y),",*\"><frame src=\"",(z 0),"\"><frame name=v src=\"",(z 1),"\"></frameset></head></html>"};
.h.pre:{"<pre>",(raze x,\:"\n"),"</pre>"};
.h.xmp:{"<xmp>",(raze x,\:"\n"),"</xmp>"};
.h.text:{raze {"<p>",x,"</p>\n"} each x};
.h.code:{s:"\t" vs x; " " sv {$[x;.h.htc[`code;.h.nbr y];y]}'[1=(til count s) mod 2;s]};
/ .h.http: whitespace-delimited scan (the doc's only example); trailing punctuation is not trimmed.
.h.http:{" " sv {$[x like "http://*";.h.ha[x;x];x]} each " " vs x};

/ .h.hn: an unknown MIME key falls back to application/octet-stream.
.h.hn:{[x;y;z] "HTTP/1.1 ",x,"\r\nContent-Type: ",$[y in key .h.ty;.h.ty y;"application/octet-stream"],"\r\nConnection: close\r\nContent-Length: ",(string count z),"\r\n\r\n",z};
.h.hy:{[x;y] .h.hn["200 OK";x;y]};
.h.he:{.h.hn["400 Bad Request";`txt;"'",.h.hc x]};
/ .h.hp: the doc's response carries .h.sb ONLY — it is not .h.html (which also carries .h.sa).
.h.hp:{.h.hy[`html;"<html><head><style>",.h.sb,"</style></head><body>",.h.pre[x],"</body></html>"]};
.h.ka:{$[0=x;"close";"keep-alive"]};
.h.val:value;

/ .h.cd joins NESTED columns' subitems with .h.d before Prepare Text (vectors / lists of strings only, ref/file-text.md); that pre-pass is .h.cd's OWN — .h.td, a bare `0:` call, has none.
.h.cd:{"," 0: $[.Q.qt x;{flip (cols x)!{$[0h<>type x;x;10h=type first x;x;.h.d sv/: string each x]} each value flip x} 0!x;x]};
.h.td:{"\t" 0: x};
/ .h.xd escapes cell text with .h.xs, as the printed source of `.h.tx[`xml]` does with `xs'`.
.h.xd:{c:cols x; (enlist "<R>"),({[c;r] "<r>",(raze {[k;v] "<",(string k),">",(.h.xs $[10h=type v;v;string v]),"</",(string k),">"}'[c;r]),"</r>"}[c;] each value each 0!x),enlist "</R>"};
.h.xt:{[x;y] .j.k each y};

/ Excel = SpreadsheetML 2003 TEXT, not xlsx. ref/doth.md pins DIFFERENT second attributes per entry point (.h.ed xmlns:o, .h.edsn xmlns:ss), so .h.ed is NOT .h.edsn on a one-key dict. Null=empty cell; a DateTime cell with NO ss:StyleID renders in Excel as a bare serial number, so every one carries one.
/ SEQUENCE SCHEMA: <Column>s before <Row>s inside <Table>, then <WorksheetOptions>, then <AutoFilter> as Table's SIBLINGS. Misorder it and Excel refuses the whole file — which nothing here can detect, since an invalid workbook is still well-formed XML. .h.i.exp/.h.i.exf re-declare the excel namespace on themselves, so both entry points work without touching either pinned namespace list.
/ .h.i.exm sanitises rather than trusts sheet names (Excel refuses one holding : \ / ? * [ ] , past 31 chars, empty, apostrophe-ended, or repeated — and a q table named with a slash is ordinary); .h.i.exn's ss:Width is POINTS, not characters (Excel's 8.43-char default is ~48pt), so a 36-char guid needs ~228 and emitting 36 would make it NARROWER.
.h.i.exq:{[x;y;z] "<Cell",x,"><Data ss:Type=\"",y,"\">",(.h.xs z),"</Data></Cell>"};
.h.i.ext:{$[x=1h;"Boolean";x in 5 6 7 8 9h;"Number";x in 12 13 14 15h;"DateTime";"String"]};
.h.i.exi:{$[x in 13 14h;" ss:StyleID=\"sD\"";x in 12 15h;" ss:StyleID=\"sDT\"";""]};
.h.i.exv:{[x;y] $[x=1h;$[y;"1";"0"];x=10h;enlist y;x in 12 13 14 15h;23#.h.iso8601 y;string y]};
.h.i.exc:{t:type x; n:neg t; $[0h<=t;.h.i.exq["";"String";$[10h=t;x;.h.d sv string x]];null x;"<Cell/>";n in 8 9h;$[0w<=abs x;.h.i.exq["";"Error";"#NUM!"];.h.i.exq["";"Number";string x]];.h.i.exq[.h.i.exi n;.h.i.ext n;.h.i.exv[n;x]]]};
.h.i.exl:{t:type x; $[0h<=t;$[10h=t;count x;count .h.d sv string x];null x;0;count .h.i.exv[neg t;x]]};
.h.i.exr:{"<Row>",(raze .h.i.exc each x),"</Row>"};
.h.i.exh:{"<Row>",(raze {.h.i.exq[" ss:StyleID=\"sH\"";"String";string x]} each x),"</Row>"};
.h.i.exn:{raze {"<Column ss:AutoFitWidth=\"0\" ss:Width=\"",(string x),"\"/>"} each {400&48|12+6*(count string x)|max 0,.h.i.exl each y}'[cols x;value flip x]};
.h.i.exf:{[x;y] $[0=y;"";"<AutoFilter x:Range=\"R1C1:R",(string x),"C",(string y),"\" xmlns=\"urn:schemas-microsoft-com:office:excel\" xmlns:x=\"urn:schemas-microsoft-com:office:excel\"/>"]};
.h.i.exp:"<WorksheetOptions xmlns=\"urn:schemas-microsoft-com:office:excel\"><FreezePanes/><FrozenNoSplit/><SplitHorizontal>1</SplitHorizontal><TopRowBottomPane>1</TopRowBottomPane><ActivePane>2</ActivePane></WorksheetOptions>";
.h.i.exm:{s:31 sublist {$[x in ":\\/?*[]";"_";x]} each string x; s:$[0=count s;"Sheet";s]; s:$["'"=first s;"_",1_s;s]; `$$["'"=last s;(-1_s),"_";s]};
.h.i.exk:{n:.h.i.exm each x; {$[y=x?(x y);x y;`$(27 sublist string x y),"_",string y]}[n;] each til count n};
.h.i.exs:{[x;y] u:0!y; "<Worksheet ss:Name=\"",(.h.xs string x),"\"><Table>",(.h.i.exn u),.h.i.exh[cols u],(raze .h.i.exr each value each u),"</Table>",.h.i.exp,.h.i.exf[1+count u;count cols u],"</Worksheet>"};
.h.i.exy:"<Styles><Style ss:ID=\"sH\"><Font ss:Bold=\"1\"/><Interior ss:Color=\"#D9D9D9\" ss:Pattern=\"Solid\"/></Style><Style ss:ID=\"sD\"><NumberFormat ss:Format=\"yyyy\\-mm\\-dd\"/></Style><Style ss:ID=\"sDT\"><NumberFormat ss:Format=\"yyyy\\-mm\\-dd hh:mm:ss.000\"/></Style></Styles>";
.h.i.exw:{[x;y] (enlist "<?xml version=\"1.0\"?><?mso-application progid=\"Excel.Sheet\"?>"),enlist "<Workbook ",x,">",.h.i.exy,(raze y),"</Workbook>"};
.h.i.exo:"xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" xmlns:x=\"urn:schemas-microsoft-com:office:excel\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"";
.h.i.exz:"xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"";
.h.ed:{.h.i.exw[.h.i.exo;enlist .h.i.exs[`Sheet1;x]]};
.h.edsn:{.h.i.exw[.h.i.exz;.h.i.exs'[.h.i.exk key x;value x]]};

/ .h.tx: `json is the doc's printed source, JSON Lines; `xls is .h.ed (SpreadsheetML text, no zip).
.h.tx:`raw`json`csv`txt`xml`xls!((,:);{.j.j each $[.Q.qt x;0!x;x]};.h.cd;.h.td;.h.xd;.h.ed);

.h.hug:{h:"0123456789abcdef"; c:"c"$til 256; c!{[s;h;c] i:"i"$c; $[c in s;enlist c;"%",h[i div 16],h[i mod 16]]}[x;h] each c};
.h.hu:{raze .h.hug[.h.sc] x};
.h.uh:{s:"%" vs x; raze (enlist first s),{("c"$"X"$2#x),2_x} each 1_s};
.h.iso8601:{s:string "p"$x; (4#s),"-",(2#5_s),"-",(2#8_s),"T",11_s};

/ Default web console + download API, all peachq's own. .h.i.wr renders under `\C`; .h.i.wf reads .h.HOME through .Q.c.rd (O_NOFOLLOW-hardened) and its `::` is the DECLINE q_http.c falls through on. .h.i.wg routes STATIC-FIRST, then .h.i.wd serializes the query through save's own .h.tx (`a.json?expr[off,lim`), joining a serializer's LINES but sending a BYTE-vector result verbatim (the xlsx/parquet shape). `?expr` EVALS (.h.jx's own `value`); auth is the operator's via .z.ac/-u, which gate ahead of .z.ph.
.h.i.wr:{c:system"c"; system "c "," " sv string system"C"; r:@[.Q.s;x;{"'",x,"\n"}]; system "c "," " sv string c; -1_"\n" vs r};
.h.jx:{[x;y] t:$[a:0>type r:value y;enlist r;r]; n:count t; e:0|n-32; enlist[(" " sv .h.ha'[("?[",/:string(0;0|x-32;e&x+32;e));("home";"up";"down";"end")]),(" ",string[n],"[",string[x],"]")],enlist[""],.h.i.wr $[a;r;32 sublist x _ t]};
.h.i.wf:{d:.h.uh x; if[(any 32>"i"$d)|(any d in "\\:")|any ("/" vs d) in (enlist ".";"..");:.h.hn["404 Not Found";`txt;"not found\n"]]; d:$[0=count d;"index.html";"/"=last d;d,"index.html";d]; b:.Q.c.rd d; $[(::)~b;(::);.h.hy[`$last "." vs d;b]]};
.h.i.wp:{j:where x="["; d:$[count j;(1+last j)_x;""]; p:"," vs d; b:(0<count j)&((count p) in 1 2)&(all 0<count each p)&all d in "0123456789,"; $[b;((last j)#x;"J"$p 0;$[2=count p;"J"$p 1;0N]);(x;0;0N)]};
.h.i.wj:{g:.h.i.wp x; e:g 0; l:.h.jx[g 1;e]; .h.hp (enlist ("?",(.h.hu e),"[") sv "?[" vs first l),.h.xs each 1_l};
.h.i.wc:{t:tables[]; $[0=count x;.h.hy[`html;.h.fram["peachq";(string t),enlist 29#" ";("?.t";$[count t;"?",.h.hu string first t;"?.t"])]];x~".t";.h.hy[`html;.h.html[.h.logo,.h.br,$[count t;raze .h.hb'[("?",/:.h.hu each string t);.h.xs each string t],\:.h.br;"(no tables)"]]];.h.i.wj x]};
.h.i.wn:{[x;y;z;w] r:.h.hn[x;y;w]; n:((count r)-count w)-2; (n#r),(raze z,\:"\r\n"),"\r\n",w};
.h.i.we:{[x;y] $[x~`json;.h.hn["400 Bad Request";`json;"{\"error\":",(.j.j y),"}"];.h.he y]};
.h.i.wq:{[x;y] g:.h.i.wp y; r:value g 0; t:$[0>type r;enlist r;r]; n:count t; t:$[null g 2;(g 1)_t;(g 2) sublist (g 1)_t]; s:.h.tx[x] t; .h.i.wn["200 OK";x;enlist "X-Total-Count: ",string n;$[4h=type s;"c"$s;"\n" sv s]]};
.h.i.wd:{[x;y] e:`$last "." vs .h.uh x; $[e in key .h.tx;@[.h.i.wq[e];.h.uh y;.h.i.we e];(::)]};
.h.i.wg:{i:x?"?"; p:i#x; u:(1+i)_x; s:@[.h.i.wf;p;.h.he]; $[not (::)~s;s;0=count u;(::);.h.i.wd[p;u]]};
.h.i.ph:{r:first x; i:r?"?"; $[(0=i)&0<count r;@[.h.i.wc;.h.uh 1_r;.h.he];.h.i.wg r]};
.z.ph:.h.i.ph;

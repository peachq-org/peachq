/ h.q — openq's `.h` namespace, authored from the PUBLISHED qdocs (ref/doth.md, CC BY 4.0).
/ ALWAYS-ON: baked in by tools/gen-bootstrap.sh -> h_gen.h, loaded at q_runtime_create after
/ q.q+dotq.q. One definition per line (the loader is line-at-a-time; no LITERAL newline inside
/ one). The members absent here — .h.ed .h.edsn .h.jx .h.ht .h.tx .h.fram — are deliberate:
/ PLAN.md's "Known defects" table names the gap blocking each.

.h.br:"<br>";
.h.c0:`024C7E;
.h.c1:`958600;
.h.d:" ";
/ .h.logo: kdb ships the KX logo HTML here; openq carries NO KX branding -> empty (divergence).
.h.logo:"";
/ .h.HOME: doc'd as the root path with no fixed default; pinned to what #217's static server serves.
.h.HOME:"html";
/ .h.ty: doc values for the 7 doc-listed keys (csv/xml/xls thus differ from openq's C fallback
/ — doc fidelity wins); the rest is #217's C-table superset of web essentials.
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

/ .h.cd joins NESTED columns' subitems with .h.d before Prepare Text, which by ref/file-text.md
/ takes vectors or lists of strings only. The pre-pass is .h.cd's OWN (mirroring the source the
/ doc prints for `.h.tx[`csv]`); .h.td, printed as a bare `0:` call, has none.
.h.cd:{"," 0: $[.Q.qt x;{flip (cols x)!{$[0h<>type x;x;10h=type first x;x;.h.d sv/: string each x]} each value flip x} 0!x;x]};
.h.td:{"\t" 0: x};
/ .h.xd escapes cell text with .h.xs, as the printed source of `.h.tx[`xml]` does with `xs'`.
.h.xd:{c:cols x; (enlist "<R>"),({[c;r] "<r>",(raze {[k;v] "<",(string k),">",(.h.xs $[10h=type v;v;string v]),"</",(string k),">"}'[c;r]),"</r>"}[c;] each value each 0!x),enlist "</R>"};
.h.xt:{[x;y] .j.k each y};

.h.hug:{h:"0123456789abcdef"; c:"c"$til 256; c!{[s;h;c] i:"i"$c; $[c in s;enlist c;"%",h[i div 16],h[i mod 16]]}[x;h] each c};
/ Written as the natural dict lookup (ref/doth.md: .h.hug maps chars to their escapes).
/ RED until dict lookup stops missing on `" "`, the char null — see PLAN.md.
.h.hu:{raze .h.hug[.h.sc] x};
.h.uh:{s:"%" vs x; raze (enlist first s),{("c"$"X"$2#x),2_x} each 1_s};
.h.iso8601:{s:string "p"$x; (4#s),"-",(2#5_s),"-",(2#8_s),"T",11_s};

/ Pulse: a liquidity heatmap that scrolls through time - price vertically, time left to right with now at the
/ right edge, resting asks in red above the market and bids in green below, the touch traced in cyan, trades
/ printing yellow at the touch, and the current depth ladder at the far right.  Not a matching engine: one
/ random walk in screen rows, soft-pulled to the centre so it never leaves, plus up to 26 liquidity bands of
/ three kinds - resting (fixed rows, long-lived), quoting (an offset from the touch, so they flow with the
/ price) and flashes - that breathe, and which the price eats through unless one is big enough to hold it.
/ Run it:  q examples/q/pulse.q   (space pauses, r reseeds, +/- speed, b block/ascii glyphs, q or Ctrl-C quits).

\l pq

LW:18;                                  / cells for the depth ladder at the right
xt:{[r;g;b] .termbox.rgb[r;g;b]};      / the xterm-256 steps depthmap.py uses, as truecolor
GREEN:xt[0;;0] each 95 135 175 215 255;
RED:xt[;0;0] each 95 135 175 215 255;
YEL:xt[255;255;0]; CYAN:xt[0;255;255]; DIM:xt[68;68;68]; BG:xt[4;6;9];
BLK:32 46 9617 9618 9619 9608;          / " .░▒▓█" by shade; the ascii ramp below
ASC:" .:=+#";
MAXB:26;

gauss:{[s] s*-6+sum 12?1f};
expov:{[l] neg[log rand 1f]%l};

/ one band: row is absolute for rest/flash and an offset from the touch for quote; side 1 = ask (above), -1 = bid
band:{[kind;side;d]
    ttl:$[kind=`rest; 200+rand 1000; kind=`quote; 100+rand 400; 6+rand 44];
    ([] kind:enlist kind; side:enlist side; row:enlist `float$d; str:enlist 0.15+rand 0.35; tgt:enlist 0.3+rand 0.7;
        ttl:enlist ttl; phase:enlist rand 6.2832; rate:enlist 0.04+rand 0.26)};

spawn:{[st]
    side:-1+2*rand 2; kind:`rest`quote`flash 0.35 0.55 bin rand 1f;
    d:1+floor expov $[kind=`quote; 0.3; 0.2];
    band[kind;side;$[kind=`quote; d; side=1; (st`ask)-d; (st`bid)+d]]};

/ every band's screen row for the touch (ask;bid)
rows:{[b;ask;bid] `long$?[b[`kind]=`quote; ?[b[`side]=1; ask-b`row; bid+b`row]; b`row]};

fresh:{[st]
    w:.termbox.width[]; h:.termbox.height[]-1; rw:w-LW+1;
    st,`w`h`rw`mid`drift`spread`ask`bid`t`bands`chs`fgs`val`sd!(w;h;rw;h%2;0f;1;h div 2;1+h div 2;0;0#band[`rest;1;0];();();h#0f;h#0)};

newst:{[] fresh `speed`blocks`paused`quit`next!(70;1b;0b;0b;0)};

/ one frame: walk the mid (drift + a pull to the centre), let it eat the resting bands in its path - a strong one
/ holds it and takes a partial fill - then step, cull and spawn bands, and build the column that scrolls in
tick:{[st]
    st[`t]+:1; h:st`h; oa:st`ask; ob:st`bid;
    st[`drift]:(0.95*st`drift)+gauss 0.012;
    off:((h%2)-st`mid)%h%4;
    st[`mid]+:gauss[0.12]+(st`drift)+off*abs[off]*0.06;
    if[0.025>rand 1f; st[`mid]+:-2 -1 1 2 rand 4];
    if[0.05>rand 1f; st[`spread]:1 1 1 2 rand 4];
    st[`mid]:(h-5)&4f|st`mid;
    b:st`bands; prints:0#0; wall:0N;
    r:rows[b;floor st`mid;(st`spread)+floor st`mid];
    up:(b[`kind]<>`quote)&(b[`side]=1)&(r<oa)&r>=floor st`mid;                       / the price rose through these
    dn:(b[`kind]<>`quote)&(b[`side]=-1)&(r>ob)&r<=(st`spread)+floor st`mid;          / or fell through these
    eat:where up|dn;
    strong:eat where b[`str;eat]>0.6;
    if[count strong;
        wall:$[any up; strong r[strong]?max r strong; strong r[strong]?min r strong];  / the first one the price met
        st[`mid]:$[any up; 1+r wall; (r wall)-1+st`spread]+(st`mid)-floor st`mid;
        st[`drift]*:-0.4;
        b[`str;wall]*:0.7;
        eat:eat where $[any up; r[eat]>r wall; r[eat]<r wall]];
    prints:r eat;
    b:b where not (til count b) in eat;
    st[`ask]:floor st`mid; st[`bid]:(st`spread)+st`ask;
    n:count b;
    b[`ttl]-:1;
    b[`tgt]:?[0.03>n?1f; 0.1+n?0.9; b`tgt];
    b[`str]:1f&0f|b[`str]+(0.08*b[`tgt]-b`str)+0.015*-6+sum each 12?'n#1f;
    nudge:(b[`kind]<>`rest)&0.03>n?1f;
    b[`row]:?[nudge; 1f|b[`row]+-1+2*n?2; b`row];
    r:rows[b;st`ask;st`bid];
    keep:(b[`ttl]>0)&(r within 0,h-1)&?[b[`side]=1; r<st`ask; r>st`bid];
    b:b where keep; r:r where keep;
    if[(count[b]<MAXB)&0.25>rand 1f; nb:spawn st; if[first rows[nb;st`ask;st`bid] within 0,h-1; b,:nb]];
    st[`bands]:b;
    st,column[st;b;prints;wall]};

/ the new column as codepoints + styles: band strength (fading with distance from the touch, summed per row)
/ through the shade ramp, haze near the touch, the cyan touch rows, and yellow prints where the price ate a band
/ or a random print lands at the touch (a big one lights the level beside it, thins it and nudges the price)
column:{[st;b;prints;wall]
    h:st`h; ask:st`ask; bid:st`bid; n:count b;
    r:rows[b;ask;bid];
    v:b[`str]*0.8+0.25*sin (b[`rate]*st`t)+b`phase;
    d:abs[r-ask]&abs r-bid;
    val:1f&@[h#0f;r;+;v*0.35+0.65*exp neg d%7];
    sd:@[h#0;r;:;b`side];
    dd:abs[(til h)-ask]&abs (til h)-bid;
    haze:(sd=0)&(dd>0)&(h?1f)<0.35*exp neg dd%5;
    val:?[haze;0.05+0.25*h?1f;val]; sd:?[haze;?[(til h)<ask;1;-1];sd];
    sh:4&`long$5*val; sh:?[val<=0.02;-1;sh];
    ch:?[sh<0;32;$[st`blocks;BLK;`long$ASC]1+sh];
    fg:?[sd=1;RED 0|sh;GREEN 0|sh];
    ch[ask,bid]:$[st`blocks;9472;45]; fg[ask,bid]:CYAN;
    if[count prints; ch[prints]:$[st`blocks;9679;64]; fg[prints]:YEL];
    if[not null wall; ch[ask,bid]:$[st`blocks;9679;64]; fg[ask,bid]:YEL];
    if[0.12>rand 1f;
        hit:0.5>rand 1f; tr:$[hit;ask;bid]; big:0.25>rand 1f; nb:tr+$[hit;-1;1];
        ch[tr]:$[big;$[st`blocks;9679;64];$[st`blocks;111 42 183;111 42 46]rand 3]; fg[tr]:YEL;   / o * ·
        if[(nb within 0,h-1)&sd[nb]<>0;
            ch[nb]:$[st`blocks;9608;35]; fg[nb]:$[big;YEL;$[hit;RED;GREEN]4];
            i:where r=nb; if[count i; st[`bands;`str;i]*:0.5];
            if[big; st[`mid]+:$[hit;-1;1]]]];
    st[`chs]:tail[st`rw] st[`chs],enlist `long$ch;
    st[`fgs]:tail[st`rw] st[`fgs],enlist fg;
    st,`val`sd!(val;sd)};

tail:{[n;l] (0|count[l]-n)_l};

render:{[st]
    w:st`w; h:st`h; rw:st`rw; n:count st`chs;
    .termbox.clear[];
    .termbox.show[h#enlist w#" "; DIM; BG];
    if[n>0;
        ch:raze st`chs; fg:raze st`fgs; x:raze (h#'(rw-n)+til n); y:raze n#enlist til h;
        ok:where ch<>32;
        .termbox.set_cells[x ok;y ok;ch ok;fg ok;BG]];
    lx:rw;
    .termbox.set_cells[lx; til h; $[st`blocks;9474;"|"]; DIM; BG];
    ask:st`ask; bid:st`bid;
    .termbox.set_cells[raze 2#enlist lx+1+til LW-1; raze (LW-1)#'ask,bid; $[st`blocks;9472;"-"]; CYAN; BG];
    val:st`val; sd:st`sd; ok:where (sd<>0)&not (til h) in ask,bid;
    if[count ok; lens:`long$val[ok]*LW-2; sh:4&`long$5*val ok;
        .termbox.set_cells[raze (lx+1)+til each lens; raze lens#'ok; $[st`blocks;9608;"#"]; raze lens#'?[sd[ok]=1;RED sh;GREEN sh]; BG]];
    .termbox.print[0;h;CYAN;0;"PULSE  ",string[count st`bands]," bands  spread ",string[st`spread],"  ",string[st`speed],
        "ms  [space]",$[st`paused;"run";"pause"]," [r]eseed +/- speed [b]locks [q]uit";w;`left];
    .termbox.present[]};

onkey:{[st;k]
    if[k in `q`ctrl_c; st[`quit]:1b; :st];
    if[k in `resize`r; :fresh st];
    if[k=`space; st[`paused]:not st`paused];
    if[k=`b; st[`blocks]:not st`blocks];
    if[k in `plus`equals,`$"+"; st[`speed]:20|st[`speed]-10];
    if[k in `minus,`$"-"; st[`speed]:200&st[`speed]+10];
    st};

run:{[]
    st:newst[]; render st;
    while[not st`quit;
        st:onkey/[st;{$[count x; x`name; enlist `]} .termbox.events 15];
        if[(not st`paused)&st[`next]<=.termbox.clock[]; st:tick st; st[`next]:(st`speed)+.termbox.clock[]; render st]]};

.termbox.init[];
run[];
.termbox.shutdown[];
exit 0

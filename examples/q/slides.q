/ An ASCII slide deck on the terminal, the fourth worked example for the .termbox cell-terminal API.
/ Run it:  q examples/q/slides.q     (right/space/n next - a bullet at a time - left/p back, 'q' or Ctrl-C quits).
/ The deck is DATA - a list of slides, each a list of element/value pairs - rendered by a few functions inside a
/ static box-drawing frame; a change of slide scrolls both slides sideways as one sheet on the termbox clock; the
/ title slide twinkles.
/ The loop runs as its own statement, so .termbox restores the terminal however that statement ends.

\l pq

deck:( (`title;"peachq";`sub;"a q that runs anywhere");
       (`title;"Why cells";`bullets;("q matrices are screens";"show: matrix -> cells";"set_cells: sparse"));
       (`title;"Thanks";`body;"A screen is a list of strings and a style per row, so a slide is a value: build it, ",
                               "diff it, scroll it.  Press q to leave, or left to go back.") );

PERIOD:30;
FRAMES:12;
DIM:.termbox.color.bright_black;
HEAD:5;

todict:{[s] (s where 0=(til count s) mod 2)!s where 1=(til count s) mod 2};

pad:{[w;rows] w#'rows,\:w#" "};

/ n copies of a glyph: q strings are bytes, so n#"=" on a UTF-8 glyph would take bytes, not glyphs
glyphs:{[n;g] raze n#enlist g};

/ the title's style per char: a peach-to-yellow ramp on truecolor, bold cyan on 16 colours
ramp:{[n] $[.termbox.has_truecolor[]; .termbox.attribute.bold+{.termbox.rgb[255;160+(60*x) div y;122-(42*x) div y]}[;n] each til n; n#.termbox.color.cyan+.termbox.attribute.bold]};

/ a slide's scrolling body as interior rows (w wide, h of them) plus a style per CELL: HEAD blank rows for the
/ title block, then `revealed` bullets (bullet `flash` bright) or the wrapped body
layout:{[slide;revealed;flash;w;h]
    sl:todict slide;
    rows:HEAD#enlist ""; fgs:HEAD#enlist w#0;
    if[`bullets in key sl; rows,:"  * ",/:revealed#sl`bullets; fgs,:(revealed#0),\:w#0; if[flash>=0; fgs[HEAD+flash]:w#.termbox.color.bright_white+.termbox.attribute.bold]];
    if[`body in key sl; b:.termbox.wrap[w-4;sl`body]; rows,:"  ",/:b; fgs,:count[b]#enlist w#0];
    (pad[w;h#rows,h#enlist ""];h#fgs,h#enlist w#0)};

/ the title block, centred in a w-cell field from column x (off-screen columns are dropped, so it scrolls)
titles:{[slide;x;w]
    sl:todict slide; t:sl`title;
    .termbox.print[x;2;ramp count t;0;t;w;`centre];
    .termbox.print[x;3;ramp count t;0;glyphs[count t;"\342\225\220"];w;`centre];
    if[`sub in key sl; .termbox.print[x;4;.termbox.attribute.dim;0;sl`sub;w;`centre]]};

/ the interior rows between the frame's sides, ready for show: the sides ride along as the rows' first and last cell
boxed:{[rows;fgs] (enlist[""],("\342\225\221",/:rows,\:"\342\225\221"); enlist[0],(DIM,/:fgs,\:DIM); 0)};

/ the static double-line frame's top, bottom and footer (hints left, the page right), all dim
frame:{[w;h;page]
    .termbox.print[0;0;DIM;0;"\342\225\224",glyphs[w-2;"\342\225\220"],"\342\225\227"];
    .termbox.print[0;h-1;DIM;0;"\342\225\232",glyphs[w-2;"\342\225\220"],"\342\225\235"];
    .termbox.print[0;h-2;DIM;0;"\342\225\221"];
    .termbox.print[w-1;h-2;DIM;0;"\342\225\221"];
    .termbox.print[1;h-2;DIM;0;"right/space/n next   left/p back   q quit";w-2;`left];
    .termbox.print[1;h-2;DIM;0;string[1+page]," / ",string count deck;w-2;`right]};

/ a few dozen stars on the blank cells of the interior, never over text
stars:{[inner]
    blank:raze {[y;r] (1+where " "=r),'y+1} ./: flip (til count inner;inner);
    n:min 40,count blank; pick:blank -1+n?count blank;
    .termbox.set_cells[pick[;0];pick[;1];n?".*";n?DIM,.termbox.color.white;0]};

/ one screen: the boxed interior, the stars on the title slide, the title block over them, then the frame
render:{[i;revealed;flash]
    w:.termbox.width[]; h:.termbox.height[];
    lo:layout[deck i;revealed;flash;w-2;h-3];
    .termbox.clear[];
    .termbox.show . boxed[lo 0;lo 1];
    if[0=i; stars lo 0];
    titles[deck i;1;w-2];
    frame[w;h;i];
    .termbox.present[]};

/ sleep to a clock deadline; keys pressed meanwhile are dropped
until:{[t] while[.termbox.clock[]<t; .termbox.peek_event max 0,t-.termbox.clock[]]};

/ both interiors side by side as one sheet, shown at a column offset that walks across w in FRAMES steps,
/ each slide's title block riding along at its own offset
transition:{[i;j;from;to;dir]
    iw:.termbox.width[]-2;
    sheet:$[dir>0; from[0],'to 0; to[0],'from 0];
    fg:$[dir>0; from[1],'to 1; to[1],'from 1];
    offs:$[dir>0; (iw*til FRAMES) div FRAMES-1; (iw*reverse til FRAMES) div FRAMES-1];
    t0:.termbox.clock[];
    {[i;j;dir;sheet;fg;off;t]
        w:.termbox.width[]; iw:w-2;
        .termbox.clear[];
        .termbox.show . boxed[iw#'off _/:sheet;iw#'off _/:fg];
        titles[deck $[dir>0;i;j];1-off;iw]; titles[deck $[dir>0;j;i];1+iw-off;iw];
        frame[w;.termbox.height[];j]; .termbox.present[]; until t}[i;j;dir;sheet;fg;;]'[offs;t0+PERIOD*1+til FRAMES]};

nbullets:{[i] sl:todict deck i; $[`bullets in key sl; count sl`bullets; 0]};

/ move dir slides: scroll from the current slide to the next (all its bullets shown when going back)
go:{[st;dir]
    w:.termbox.width[]-2; h:.termbox.height[]-3; j:(st`i)+dir; rev:$[dir>0; 0; nbullets j];
    transition[st`i;j;layout[deck st`i;st`rev;-1;w;h];layout[deck j;rev;-1;w;h];dir];
    st,`i`rev!(j;rev)};

run:{[]
    st:`i`rev`quit!(0;0;0b);
    while[not st`quit;
        render[st`i;st`rev;-1];
        k:(.termbox.peek_event $[0=st`i; 100; 500])`name;
        if[k in `q`ctrl_c; st[`quit]:1b];
        if[(k in `right`space`n) and st[`rev]<nbullets st`i;
            render[st`i;1+st`rev;st`rev]; until 80+.termbox.clock[]; st[`rev]:1+st`rev; k:`];
        if[(k in `right`space`n) and st[`i]<count[deck]-1; st:go[st;1]];
        if[(k in `left`p) and st[`i]>0; st:go[st;-1]]]};

.termbox.init[];
run[];
.termbox.shutdown[];
exit 0

/ Pong on the terminal, the third worked example for the .termbox cell-terminal API.
/ Run it:  q examples/q/pong.q     (start screen: 1 = you vs the computer, 2 = two players, m = the mouse steers
/ your paddle; left paddle w/s, right paddle up/down arrows - against the computer the arrows steer the left
/ paddle too; the ball serves slow and speeds up per paddle hit; first to 2 points wins; 'r' back to the start
/ screen, 'q' or Ctrl-C quits).  Frames are scheduled on .termbox.clock[], not on the input timeout.
/ The loop runs as its own statement, so .termbox restores the terminal however that statement ends.

\l pq

PERIOD:40;
LEFT:.termbox.color.bright_cyan; RIGHT:.termbox.color.bright_magenta;

serve:{[st;dx] st,`bx`by`dx`dy`speed!((st`w) div 2;(st`h) div 2;dx;-1+rand 3;1)};

/ a fresh match on the current console size: one status row, paddles a fifth of the height
newmatch:{[mode]
    w:.termbox.width[]; h:.termbox.height[]-1;
    ph:max 3,h div 5;
    .termbox.set_input_mode .termbox.input.esc+.termbox.input.mouse*`mouse=mode;
    serve[`w`h`ph`mode`ly`ry`ls`rs`frame`next`quit!(w;h;ph;mode;(h-ph) div 2;(h-ph) div 2;0;0;0;PERIOD+.termbox.clock[];0b);1]};

status:{[st]
    ls:string st`ls; rs:string st`rs;
    (ls," : ",rs; (count[ls]#LEFT+.termbox.attribute.bold),(3#.termbox.color.cyan),count[rs]#RIGHT+.termbox.attribute.bold)};

render:{[st]
    .termbox.clear[];
    w:st`w; h:st`h; ph:st`ph;
    if[`menu=st`mode;
        .termbox.print[0;h div 2;.termbox.color.cyan;0;"PONG  [1] one player  [2] two players  [m] mouse  [q] quit";w;`centre];
        :.termbox.present[]];
    .termbox.set_cells[w div 2;til h;"|";.termbox.color.bright_black;0];
    .termbox.set_cells[(ph#1),ph#w-2;(st[`ly]+til ph),st[`ry]+til ph;"#";(ph#LEFT),ph#RIGHT;0];
    .termbox.set_cells[st`bx;st`by;"o";.termbox.color.bright_yellow;0];
    line:status st;
    .termbox.print[0;h;line 1;0;line 0];
    .termbox.print[0;h;.termbox.color.cyan;0;$[`won=st`mode; "PLAYER ",string[1+st[`rs]>st`ls]," WINS   [r]estart [q]uit"; "w/s  up/down   [r]estart [q]uit"];w;`right];
    .termbox.present[]};

clamp:{[st;k;v] st[k]:max 0,min ((st`h)-st`ph),v; st};

/ one keypress moves a paddle three cells (a terminal reports presses only, never holds)
onkey:{[st;k]
    if[k in `q`ctrl_c; st[`quit]:1b; :st];
    if[k=`r; :newmatch[`menu]];
    if[k=`resize; :newmatch[$[st[`mode] in `one`two`mouse; st`mode; `menu]]];
    if[`menu=st`mode; :$[k=`$"1"; newmatch[`one]; k=`$"2"; newmatch[`two]; k=`m; newmatch[`mouse]; st]];
    if[`won=st`mode; :st];
    left:k in `w`s,$[`two<>st`mode; `up`down; ()];
    if[left; :clamp[st;`ly;st[`ly]+3*-1+2*k in `s`down]];
    if[k in `up`down; :clamp[st;`ry;st[`ry]+3*-1+2*k=`down]];
    st};

/ the computer's paddle: one cell per two ball moves (half the ball's rate) and a one-cell dead zone
cpu:{[st]
    if[(not st[`mode] in `one`mouse) or 0<>(st`frame) mod 2*3-st`speed; :st];
    d:(st`by)-(st`ry)+(st`ph) div 2;
    clamp[st;`ry;st[`ry]+signum d*abs[d]>1]};

/ one frame: move the ball, bounce off the walls, the paddles (English from where it hit) or score
tick:{[st]
    if[not st[`mode] in `one`two`mouse; :st];
    st[`frame]:1+st`frame;
    st:cpu st;
    if[(1=st`speed) and 1=(st`frame) mod 2; :st];
    nx:(st`bx)+st`dx; ny:(st`by)+st`dy;
    if[(ny<0) or ny>=st`h; st[`dy]:neg st`dy; ny:(st`by)+st`dy];
    if[(nx<=1) and ny within st[`ly]+0,(st`ph)-1; .termbox.beep[880;20]; nx:2; st[`dx]:1; st[`dy]:signum[(ny-st`ly)-(st`ph) div 2]; st[`speed]:2];
    if[(nx>=(st`w)-2) and ny within st[`ry]+0,(st`ph)-1; .termbox.beep[880;20]; nx:(st`w)-3; st[`dx]:-1; st[`dy]:signum[(ny-st`ry)-(st`ph) div 2]; st[`speed]:2];
    if[nx<0; .termbox.beep[330;120]; st[`rs]:1+st`rs; :won serve[st;-1]];
    if[nx>=st`w; .termbox.beep[330;120]; st[`ls]:1+st`ls; :won serve[st;1]];
    st,`bx`by!(nx;ny)};

won:{[st] $[2 in st`ls`rs; st,enlist[`mode]!enlist`won; st]};

/ frames on a fixed clock deadline: wait only the remaining time, take every pending event, then advance once;
/ the pointer's last row becomes the left paddle's centre
run:{[]
    st:newmatch[`menu];
    while[not st`quit;
        render st;
        es:.termbox.events max 0,st[`next]-.termbox.clock[];
        st:onkey/[st;es`name];
        if[`mouse=st`mode; ys:exec y from es where kind=`mouse; if[count ys; st:clamp[st;`ly;last[ys]-(st`ph) div 2]]];
        if[.termbox.clock[]>=st`next;
            st:tick st;
            st[`next]:st[`next]+PERIOD;
            if[.termbox.clock[]>st`next; st[`next]:PERIOD+.termbox.clock[]]]]};

.termbox.init[];
run[];
.termbox.shutdown[];
exit 0

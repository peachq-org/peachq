/ Spinning galaxy: a rotozoomer (Second Reality / Unreal, Future Crew 1992-93) over an analytic log-spiral
/ galaxy with fractal (fBm) detail that morphs over time, on the .termbox cell-terminal API.
/ Run it:  q examples/q/galaxy.q   ('r' rerolls the parameters, space pauses, d/D darken/lighten, q or Ctrl-C quits).

\l pq

/ the shape: spiral arms (fractional = a blend of two arm counts), twist (log-spiral tightness), arm sharpness,
/ core glow radius, ellipticity, a warp of the arms, a ring (radius, strength, width), noise roughness, two palette
/ hues as r g b.  Every value eases toward a target: r rerolls all the targets, and one drifts on its own every
/ few seconds, so the galaxy morphs instead of spinning rigidly.
roll:{[]
    `arms`twist`sharp`core`ecc`warp`ring`ringamp`ringw`rough`ar`ag`ab`br`bg`bb!
        (1+rand 5f; -6+rand 12f; 0.5+rand 4f; 2+rand 8f; 0.5+rand 1f; rand 1.5; 3+rand 10f; rand 1.5; 1+rand 3f;
         0.3+rand 0.5),`float$6?256};
NUM:key roll[];
TAU:1.2;                                / seconds to close most of the gap to a target
DRIFT:3000;                             / ms between one parameter picking a new target of its own

/ hashed value noise on integer lattice points, 0..1 (all float arithmetic: mixed long/float is the slow lane)
hash:{[i;j;s] u:sin (i*12.9898)+(j*78.233)+s*37.719; u*:43758.5453; u-`float$floor u};
noise:{[u;v;s] i:`float$floor u; j:`float$floor v; fu:u-i; fv:v-j;
    fu:fu*fu*3f-2f*fu; fv:fv*fv*3f-2f*fv;
    a:hash[i;j;s]; b:hash[i+1f;j;s]; c:hash[i;j+1f;s]; d:hash[i+1f;j+1f;s];
    (a+fu*b-a)+fv*(c+fu*d-c)-a+fu*b-a};
fbm:{[p;u;v] w:1f; f:1f; acc:0f; tot:0f;
    do[p`oct; acc+:w*noise[f*u;f*v;f+p`seed]; tot+:w; w*:p`rough; f*:2.1]; acc%tot};

/ the brightness field for every cell after the rotozoom (u v are texture coordinates)
field:{[p;u;v;t]
    v*:p`ecc;
    r:sqrt (u*u)+v*v; th:(2*atan v%r+u)+(p`warp)*sin (0.35*r)+0.4*t;
    ph:(p`twist)*log 1f+r;
    a:`float$floor p`arms; f:(p`arms)-a;
    arm:0.5+0.5*((1f-f)*cos (a*th)+ph)+f*cos ((a+1f)*th)+ph;
    arm:arm xexp p`sharp;
    glow:exp neg r%p`core;
    d:(r-p`ring)%p`ringw; ring:(p`ringamp)*exp neg d*d;
    stars:fbm[p;2.5*u;2.5*v];
    (0.25+0.75*arm)*(glow+ring)*0.5+0.8*stars};

RAMP:" .:-=+*#%@";
/ style per brightness bucket: black -> hueA -> hueB -> white, 16-colour fallback
palette:{[p;tc] k:0.05*til 21; mix:{[a;b;k] `long$a+k*b-a}; ha:p`ar`ag`ab; hb:p`br`bg`bb;
    $[tc; .termbox.rgb ./: (mix[3#0f;ha] each 1&k%0.5),(mix[ha;hb] each 1&0|(k-0.5)%0.3),
          mix[hb;3#255f] each 1&0|(k-0.8)%0.2;
      raze 5 4 4 4 4#'.termbox.color`bright_black`blue`magenta`bright_magenta`bright_white]};

render:{[st]
    p:st`p; w:st`w; h:st`h; n:w*h;
    x:((`float$n#til w)-w%2)%2; y:(`float$raze w#'til h)-h%2;          / a cell is twice as tall as wide
    c:cos a:st`ang; s:sin a; z:st`scale;
    u:z*(x*c)-y*s; v:z*(x*s)+y*c;
    b:field[p;u;v;st`t];
    q:0f|((rank b)%n-1)-p`dark; q:(q%1f-p`dark) xexp 1.4;    / rank-equalised: exactly `dark of the cells stay black
    bucket:`long$20f&20f*q;
    ch:RAMP `long$9f&9f*q;
    .termbox.show[w cut ch; w cut st[`pal] bucket; 0];
    .termbox.print[0;h;.termbox.color.cyan;0;
        "arms ",(4#string p`arms),"  twist ",(5#string p`twist),"  ecc ",(4#string p`ecc),"  warp ",(4#string p`warp),
        "  ring ",(4#string p`ringamp),"  dark ",(4#string p`dark),"  [r]eroll [space]pause d/D dark [q]uit";w;`left];
    .termbox.present[]};

fresh:{[st;p] st,`p`pal!(p;palette[p;.termbox.has_truecolor[]])};
newst:{[] p:roll[],`dark`oct`spin`zoom`seed!(0.5;4;0.4;0.5;0f);
    fresh[`w`h`ang`scale`t`paused`quit`tg`drift!(.termbox.width[];.termbox.height[]-1;0f;0.4;0f;0b;0b;roll[];0);p]};

/ ease every shape value toward its target; every DRIFT ms one of them picks a new target
morph:{[st;dt]
    if[st[`drift]<.termbox.clock[]; st[`drift]:DRIFT+.termbox.clock[]; k:rand NUM; st[`tg;k]:roll[] k];
    p:st`p; p[NUM]:p[NUM]+(1f-exp neg dt%TAU)*(st[`tg]NUM)-p NUM;
    p[`seed]+:0.15*dt;
    fresh[st;p]};

step:{[st;k]
    if[k in `q`ctrl_c; st[`quit]:1b; :st];
    if[k=`resize; :fresh[st,`w`h!(.termbox.width[];.termbox.height[]-1);st`p]];
    if[k=`r; st[`tg]:roll[]; :st];
    if[k=`space; st[`paused]:not st`paused];
    if[k in `d`D; st[`p;`dark]:1&0|st[`p;`dark]+0.05*-1+2*k=`d; :fresh[st;st`p]];
    st};

run:{[]
    st:newst[]; prev:.termbox.clock[];
    while[not st`quit;
        render st;
        st:step/[st;{$[count x; x`name; enlist `]} .termbox.events 30];
        now:.termbox.clock[]; dt:(now-prev)%1000; prev:now;
        if[not st`paused;
            st:morph[st;dt]; st[`t]+:dt; p:st`p;
            st[`ang]+:dt*p`spin;
            st[`scale]:0.35*2 xexp 1.2*sin 0.5*(p`zoom)*st`t]]};

.termbox.init[];
run[];
.termbox.shutdown[];
exit 0

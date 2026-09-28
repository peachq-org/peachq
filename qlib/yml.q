/ YAML: .yml.load reads YAML lines or a file into q values, .yml.dump writes a q value out as YAML lines.
/ @author drewsteele
/ @link https://github.com/drewsteele/qYaml
/ Copyright (c) 2025 drewsteele, MIT License - the full licence text closes this file.

/ use global variable for anchors 
/ would be nice to do this in a more functional way without globals but this was easiest
.yml.i.anchors:(enlist`)!enlist (::); 

.yml.i.trim:{ 
    m:min{first where not" "=x}each (c:x?\:"#")#'x;
    :@[x;where c>m;m _]; / 
    };

.yml.i.rmComment:{#[;x]x?"#"};

.yml.i.splitDocs:{[yml]
    sep:where yml like\:"---*";
    if[0=count sep; :enlist yml];
    d:sep cut yml;
    :{#[;x]x?"..."}each d;
    };

/ Parse YAML into q: a mapping is a dictionary, a sequence a list (a sequence of like mappings collapses to a table),
/ and a scalar its natural q type.  A null or empty value is ::, true/false are booleans, .nan/.inf are 0n/0w,
/ 0x/0o numbers are longs, and an ISO timestamp is a kdb timestamp, shifted to UTC when it carries a zone offset.
/ Anchors and aliases resolve; custom tags are ignored.  Several documents, each opened by `---`, answer a list, less
/ any document that is null as a whole; when only one is left it comes back alone, and when none is left, ().
/ @param yml (any) the YAML as a list of strings, one per line (enlist a single line), or a file symbol: `:config.yml
/ @return (any) the parsed document, or a list of documents
/ @throws failed to read yaml file a file symbol that cannot be read
/ @throws no matching anchor found for alias an alias naming no earlier anchor
/ @eg .yml.load ("name: peach";"tags: [q, kdb]";"n: 3")
.yml.load:{[yml]
    if[-11h=type yml; 
        yml:@[read0; yml; {[x;e]'"failed to read yaml file ",string[x],": ",e}[yml;]];
    ];
    .yml.i.anchors:(enlist`)!enlist (::); / reset global anchors
    docs:.yml.i.splitDocs yml;
    res:.yml.i.parse each docs;
    res:res where not (::)~'res;
    :$[1=count res; first res; res];
    };

.yml.i.parse:{[s]

    if[10h=type s; :.yml.i.parseSimple s]; 

    if[s[0] like "---*";
        s:{$[0=count x 0;1_x;x]}@[s;0;trim@3_]
        ];

    s:_[;s]first where not trim[s] like "#*"; / drop any comments at the top of the stream

    f:first/[s];
    if[f in key .yml.i.parsers; :.yml.i.parsers[f] s];


    s:.yml.i.trim s;
    if[any "- "~/:s[;0 1];
        :.yml.i.parseList s;
        ];

    if[.yml.i.isDict s; 
        :.yml.i.parseDict s
        ];
    
    :@[.yml.i.parseSimple; s; {'"failed to parse yml - ",x}]
    
 };
.yml.i.parsers:(!) . flip (
    ("!"    ; `.yml.i.parseTag);
    ("|"    ; `.yml.i.parseLiteral);
    (">"    ; `.yml.i.parseFoldedScalar);
    ("&"    ; `.yml.i.parseAnchor);
    ("*"    ; `.yml.i.parseAlias);
    ("{"    ; `.yml.i.parseFlow);
    ("["    ; `.yml.i.parseFlow)
  );
  
 
.yml.i.parseTag:{[s]
    if[10h=type s; s:enlist s];
    tag:`$2_first " " vs s[0]; val:@[s;0; (3+count string tag)_];
    if[tag=`str; 
        :trim " " sv trim each val
        ];
    if[tag=`omap;
        if[not res~distinct res:.yml.i.parse val; '"duplicate value found in omap - should be unique list of maps"];
        :res
        ];
    if[tag=`set;
        res:.yml.i.parse val;
        if[not 99h=type res; '"invalid format for set type - should be a map with null values"];
        :.yml.i.parseSimple each string distinct key res / convert back to native types for a set
        ];

     :.yml.i.parse val;
   };

.yml.i.parseLiteral:{[s]
    if[10h=type s; s:enlist s];
    endWith:"\n"; / default to end with a newline
    origNewLines:1|count[s] - 1+last where not ""~/:trim each s;
    if["|"=first/[s]; s:@[s;0;1_]]; / remove the literal indicator
    if["-"=first/[s]; s:@[s;0;1_]; endWith:""]; / - sign means strip newline at end
    if["+"=first/[s]; s:@[s;0;1_]; endWith:origNewLines#"\n"]; / + sign means keep oringal new lines
    if[0=count trim s[0]; s:1_s]; / drop empty first line
    tl:first l:{first where  " "<>x}each s;
    if[not all {(x~"") or x like "#*"} each s where l<tl;
        '"invalid nesting level for literal string ",.Q.s s
        ];
    :("\n" sv .yml.i.trim s@where tl<=l),endWith;
    };
.yml.i.parseFoldedScalar:{[s]
    if[10h=type s; s:enlist s];
    endWith:"\n"; / default to end with a newline
    origNewLines:1|count[s] - 1+last where not ""~/:trim each s;
    if[">"=first/[s]; s:@[s;0;1_]]; / remove the flow indicator
    if["-"=first/[s]; s:@[s;0;1_]; endWith:""]; / - sign means strip newline at end
    if["+"=first/[s]; s:@[s;0;1_]; endWith:origNewLines#"\n"]; / + sign means keep oringal new lines
    if[0=count trim s[0]; s:.yml.i.trim 1_s]; / drop empty first line
    ni:-1_next i1:" "=/:s[;0];i:-1_i1; / indentation index but ignore the last line - we handle it at the end
    / fold a line with a space where it and the next line are not indented
    s:@[s;where not[i]and not ni;{x," "}];     
    s:@[s;where i; "\n",]; / fold a newline on indentation
    s:@[s;where i&not ni; {x,"\n"}]; / fold a newline where it gets unappended
    :raze s,endWith; / add newline at the end
    };

.yml.i.parseAnchor:{[s]
    if[10h=type s; s:enlist s];
    anchor:`$1_first " " vs s[0];
    val:.yml.i.parse @[s;0;(2+count string anchor)_];
    .yml.i.anchors:.yml.i.anchors,enlist[anchor]!enlist val;
    :val;
    };

.yml.i.parseAlias:{[s]
    alias:`$1_s:trim .yml.i.rmComment " " sv s;
    if[not alias in key .yml.i.anchors; '"no matching anchor found for alias ",string[alias]];
    :.yml.i.anchors alias
    };
    
.yml.i.parseFlow:{[s]
    d:first r:rtrim raze over s;
    if[not d in "{["; '"invalid opening delim for map or seq - should start with { or ["];
    if[not last[r]=c:"}]""{["?d; '"missing closing delim - expected ",c];
    g:sums 1 1 -1 -1"[{}]"?r1:",",1_-1_r;
    p:trim each 1_/:cut[;r1]where (0=g)&","=r1;
    / if it was a list give it proper list notation for parsing in standard way 
    if[d~"["; p:"- ",/:p]; 
    / if it was a map ensure any missing keys are filled with nulls
    if[d~"{"; p:@[p; where not ":" in/:p; {x,":"}]];
    :.yml.i.parse p
    };

.yml.i.parseList:{[s]
    s:rtrim each {x where 0<count each x} .yml.i.rmComment each s;
    li:where "- "~/:s[;0 1];
    :.yml.i.parse each li cut 2_/:s;
    };

.yml.i.isDict:{[s]
    tl:where not (s[;0]in " ]}")or trim[s] like "#*"; / top level
    if[0=count tl; :0b];
    / each top level entry should be like "a: ..." or should be a complex mapping
    / i.e. ("? key"; ": value")
    :all {(0<first ss[x;": "])or x[0] in "?:"}each (s@tl),\:" ";
    };

.yml.i.parseDict:{[s]
    tl:where not (s[;0]in " ]}")or trim[s] like "#*";
    stl:s@tl; / top level entiries in the yaml doc, i.e. should be the dict keys
    lvls:tl + til each(count[s]^next tl)-tl; / split nested levels

    res:()!();

    mkl:where 0<mks:first each ss[;": "]each stl,\:" "; / normal mapping levels i.e. "a: ..."
    mk:();
    if[count mkl;
        mk:`$#'[mks@mkl; stl@mkl]; / normal mapping keys
        mv:.yml.i.parse each @[s; tl@mkl; {y _ x}; 2+mks@mkl]@/:lvls@mkl;
        res:res,mk!mv;
        ];

    ckl:where stl[;0]="?"; / complex mappings i.e. "? a ..."
    ck:();
    if[count ckl; / complex keys
        ck:`$.yml.i.parse each trim each @[s;tl@ckl;2_]@/:lvls@ckl;
        ck:@[ck; where 1<count each ck; {`$"," sv string x}]; / join complex keys with comma rather than proper compound key
        cvl:where stl[;0]=":";
        cv:.yml.i.parse each trim each @[s;tl@cvl;2_]@/:lvls@cvl;
        valExists:(1+ckl) in cvl; / complex value is set explicitly, if not it should be null
        res:res,((ck where valExists)!cv) , (ck where not valExists)!(count where not valExists)#enlist (::); 
        ];

    :((mk,ck)@iasc[mkl,ckl])#res; / ensure it is ordered correctly
    };

/ parsing for atomic values, handles
/ single quotes, double quotes, numbers, timestamps, hex, octal, nulls/booleans/inf and standard strings
.yml.i.parseSimple:{[s]
    if[0=type s; s:trim " "sv trim each s]; / if it is a list, fold it
    s1:trim .yml.i.rmComment s; / for use once we have checked it is not a string literal with a '#'
    ls:lower s1;
    :$[
        "\""=first s             ; .yml.i.parseDoubleQuote s;
        "'"=first s              ; .yml.i.parseSingleQuote s;
        .yml.i.isNumber s1         ; @[value; s1; s1];
        .yml.i.isTs s1             ; @[.yml.i.parseTs; s1; s1]; / default to input string if parsing fails
        s1 like "0x*"            ; 16 sv "0123456789abcdef"?/:2_lower s1;
        (s1 like "0o*")          ; 8 sv 10 vs "J"$2_s1;
        ls in key .yml.i.nbi       ; .yml.i.nbi ls;
        s1 / default - return without comments
        ];
    };
/ nulls booleans and infinities
.yml.i.nbi:(!) . flip (
    (""     ; (::));
    ("null" ; (::));
    ("true" ; 1b);
    ("false"; 0b);
    (".nan" ; 0n);
    (".inf" ; 0w);
    ("+.inf"; 0w);
    ("-.inf"; -0w)
    );

.yml.i.parseDoubleQuote:{[s]
    d:1+ss[s;"\""] except 1+ss[s;"\\\""]; / ignore escaped double quotes
    if[(2<>count d) or 0<count trim .yml.i.rmComment last[d]_s;
        '"Failed to parse yaml - incorrect double quote usage: ",s]; 
    / try to .j.k to resolve some escape sequences that json handles
    :@[.j.k; ssr[last[d]#s; "#";"\\#"]; last[d]#s]; 
    };
.yml.i.parseSingleQuote:{[s]
    d:1+ss[s;"'"] except raze ss[s;"''"]+\:0 1; / ignore escaped single quotes
    if[(2<>count d) or 0<count trim .yml.i.rmComment last[d]_s; 
        '"Failed to parse yaml - incorrect single quote usage: ",s];
    :ssr[1_-1_last[d]#s; "''";"'"]
    };

/ it starts with a number as has valid number components (decimals or e notation)
.yml.i.isNumber:{(x[0]in .Q.n)& all x in .Q.n,".e+"}

/ allow iso or kdb date formats, e.g. 2001.01.01 or 2001-01-01
.yml.i.isTs:{x like "[1-9][0-9][0-9][0-9][.-][0-3][0-9][.-][0-3][0-9]*"};
.yml.i.parseTs:{
    if[not lower[x 10]in"td "; '"invalid ts type"];
    ts:trim @[x," ";10; :; "D"]; / convert to kdb timestamp format
    ts:$[2 = count tss:"+" vs 11_ts;
          ("D"$10#ts)+("N"$tss[0])-"U"$tss[1]; / +n means it is n hours ahead of GMT so subtract to convert to GMT
        2 = count tss:"-" vs 11_ts;
          ("D"$10#ts)+("N"$tss[0])+"U"$tss[1]; / -n means it is n hours behind GMT so add to convert to GMT
        "Z" = last ts;
          "P"$-1_ts;
        "P"$ts
       ];
   if[any null ts; '"failed to parse timestamp"];
   :ts
   };


/ TODO - add .yml.dump for writing out yaml from kdb objects
.yml.i.spaces:2;
/ Write a q value as simplified block-style YAML: a dictionary becomes a mapping, a list or a table (one mapping per
/ row) a sequence, :: an empty value, and a timestamp is written in ISO 8601.  A string holding a quote, a backslash,
/ a tab, a newline or a digit is single-quoted; any other is written plain, so one that reads as another scalar
/ (true, null, .inf) comes back from .yml.load as that value, not as a string.
/ @param x (any) the value to write
/ @return (any) the YAML as a list of strings, one per line - but a non-string atom answers its one line as a string
/ @eg .yml.dump `name`tags!("peach";`q`kdb)
.yml.dump:{[x]
    :$[10h~type x; enlist; (::)].yml.i.write[x;0];
    };

.yml.i.sc:"'\\\"\n\t",.Q.n; / if a string contains any of these characters then escape it in single quotes
.yml.i.write:{[x;lvl]
    s:(.yml.i.spaces*lvl)#" ";
    out:$[
      98h=type x; / table as list of dictionaries
        raze enlist["- "],/:.yml.i.write[;lvl+1] each x;
      99h=type x;
        raze .yml.i.writeKv[;;lvl]'[key x; value x];
      10h=type x; / escape single quotes, else leave as is for readability
        $[any x in .yml.i.sc; "'",ssr[x;"'";"''"],"'"; x];
      type[x] within 0 20h; 
        raze .yml.i.writeLi[;lvl]each x;
      type[x] in neg 19 12h;
        .h.iso8601 x;
      any nbi:x~/:value .yml.i.nbi;
        .yml.i.nbi?x;
      string x
      ];
    
     :$[10h=type out; out;s,/:out]; / nest to correct level
 
    };

.yml.i.writeKv:{[k;v;lvl]
    yk:$[10h=type k; k; string[k]],": ";
    / nested value so increase lvl by one and enlist the key so it sits on its own line
    if[nv:(type[v]>=0)&not 10h=type v; 
        lvl:lvl+1; 
        yk:enlist yk
        ];
    :$[not nv; enlist; (::)] yk , .yml.i.write[v; lvl];
    };

.yml.i.writeLi:{[li; lvl]
    sep:"- ";
    if[nv:(type[li]>=0)&not 10h=type li; 
        sep:enlist sep;
        lvl:lvl+1;
        ];
    :$[not nv; enlist; (::)] sep,.yml.i.write[li; lvl]
    };

/
MIT License

Copyright (c) 2025 drewsteele

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
\

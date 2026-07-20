.j.k:-29!;
.j.j:-31!;
/ .j.jd (x;d): serialize x's first element when x is a 2-list (d, the null->0w map, is moot in openq).
.j.jd:{$[((0h=type x)&2=count x);.j.j first x;.j.j x]};

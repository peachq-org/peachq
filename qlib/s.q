/ SQL on the process's own DuckDB database: s)SELECT ... hands the line to .s.e; .s.ex runs a statement with parameters and answers its table.

/ @ignore
.s.e:{[x] r:.duckdb.main[] x; if[(.s.i.head x) in ("CREATE";"ALTER"); .s.i.sync[]]; r}
/ @ignore
.s.i.sync:{[] h:.duckdb.main[]; .pq.i.load[h;.duckdb.i.tables[h] except key `.]}
/ @ignore
.s.i.head:{[s]
  s:((s in " \t\r\n")?0b)_s;
  if["--"~2#s; :.s.i.head (1+s?"\n")_s];
  if["/*"~2#s; k:first where (s="*")&next s="/"; :$[null k;"";.s.i.head (2+k)_s]];
  upper ((s in .Q.a,.Q.A)?0b)#s}

/ Run SQL on the main database, sql as .duckdb.exec takes it: a string, or the pair (string;dict) / (string;list) with parameters.
/ @eg .s.ex ("SELECT $a + $b AS n";`a`b!1 2)
.s.ex:{[sql] .duckdb.exec[.duckdb.main[];sql]}

/ The $name parameters a statement (string) declares, distinct and in first-occurrence order; $1 and ? are named by number.
/ @eg .s.params "SELECT $x FROM t WHERE s='$y' AND n>$z"
.s.params:{[sql] .duckdb.params sql}

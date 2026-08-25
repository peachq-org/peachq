/* q_csv — the incremental CSV core behind the `.csv` namespace (lib/csv.q).
 * Registered by the `\l pq` gate (q_pq.c), like q_regex_register. */
#ifndef Q_CSV_H
#define Q_CSV_H

#include <rayforce.h>

void q_csv_register(void);

/* THE C door on the decoder: a CSV/TSV RESOURCE (any read0 identifier) or
 * CONTENT decoded into a table, independent of the `.csv` namespace's binding.
 * `delim` is the field separator where the caller knows one (a `.tsv` does),
 * 0 to sniff as `.csv.read` does by default.  Owned table, or an owned error. */
ray_t* q_csv_read_table(ray_t* src, char delim);

#endif /* Q_CSV_H */

/* q_strfmt_pin — the vendored fmt tree's pin.  fmt's own FMT_VERSION names the
 * upstream release DuckDB forked, not DuckDB's patches, so the pin is the DuckDB
 * COMMIT plus a tree digest; tools/fmt-pin.sh checks the digest at every rebuild
 * of the fmt archive.  Bump recipe: third_party/fmt/README.peachq.md. */
#ifndef Q_STRFMT_PIN_H
#define Q_STRFMT_PIN_H

#define PQFMT_DUCKDB_PIN "b929b8640f8722144bc0474dae4277148240a57e"
#define PQFMT_SRC_SHA256 "e12db194dc6da94fa4cd7ca60fd771ac3e746679c3cde92823746d70c17cabc9"

#endif

/* q_re2_pin — the version pin of the vendored RE2 tree.
 *
 * RE2 as DuckDB vendors it is an unversioned, de-abseil'd snapshot: no
 * RE2_VERSION, nothing in its CMakeLists, no version header.  So the pin is
 * expressed as the DuckDB RELEASE the tree was taken from — the release whose
 * matching a peachq predicate is meant to agree with — plus a digest of the
 * tree itself, which is what makes silent drift detectable at all.
 *
 * Two consumers, deliberately: PQRE2_DUCKDB_PIN is what `.regexp.version`
 * reports, and tools/re2-pin.sh checks PQRE2_SRC_SHA256 against the tree at
 * every rebuild of the RE2 archive.  Bumping RE2 means editing both lines — see
 * third_party/re2/README.peachq.md for the recipe. */
#ifndef Q_RE2_PIN_H
#define Q_RE2_PIN_H

#define PQRE2_DUCKDB_PIN "v1.4.5"
#define PQRE2_SRC_SHA256 "fb94c254556b9794a060017e5d5af97f201bf2882dd9be43246647560ebded60"

#endif

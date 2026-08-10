#!/bin/sh
# re2-pin.sh — the vendored RE2 tree's digest, and the tripwire on it.
#
# RE2 as DuckDB vendors it has NO version marker of any kind, so a digest of the
# tree is the only thing that makes drift detectable.  This computes it and
# compares against PQRE2_SRC_SHA256 in src/qlang/io/q_re2_pin.h; the Makefile
# runs it at every relink of libpqre2, so a tree that no longer matches its
# recorded pin fails the build rather than quietly changing what a pattern means.
#
#   tools/re2-pin.sh          check (silent + exit 0 when they agree)
#   tools/re2-pin.sh --print  print the digest, for updating the header
#
# Skips (exit 0) where sha256sum is unavailable: the check is a rot-guard for
# the vendoring host, not a portability requirement.
set -eu
cd "$(dirname "$0")/.."

PIN_H=src/qlang/io/q_re2_pin.h
command -v sha256sum >/dev/null || exit 0

# README.openq.md is OURS, not upstream's, so it is not part of what we pinned.
actual=$(find third_party/re2 -type f ! -name 'README.openq.md' | LC_ALL=C sort |
         xargs sha256sum | sha256sum | cut -d' ' -f1)

[ "${1:-}" = "--print" ] && { echo "$actual"; exit 0; }

want=$(sed -n 's/.*PQRE2_SRC_SHA256 "\([0-9a-f]*\)".*/\1/p' "$PIN_H")
[ "$actual" = "$want" ] && exit 0
cat >&2 <<EOF
re2-pin: third_party/re2 does not match its recorded pin.
  recorded ($PIN_H): $want
  actual:                            $actual
If you deliberately re-vendored RE2, update PQRE2_DUCKDB_PIN and
PQRE2_SRC_SHA256 in that header (see third_party/re2/README.openq.md).
EOF
exit 1

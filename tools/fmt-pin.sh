#!/bin/sh
# fmt-pin.sh — the vendored fmt tree's digest, and the tripwire on it.
#
# The re2-pin.sh pattern, for the same reason: DuckDB's patched fmt carries an
# FMT_VERSION from upstream fmt but nothing that identifies DuckDB's PATCHES,
# which are exactly what we vendor it for (the printf thousands-separator flags,
# the 128-bit lanes).  So the pin is the DuckDB COMMIT the tree was taken from
# plus a digest of the tree itself, and the Makefile runs this at every rebuild
# of the fmt archive — a tree that no longer matches its pin fails the build
# rather than quietly changing what a format string means.
#
#   tools/fmt-pin.sh          check (silent + exit 0 when they agree)
#   tools/fmt-pin.sh --print  print the digest, for updating the header
#
# stub/ and README.peachq.md are OURS, not upstream's, so they are not part of
# what we pinned — that is what makes "the rest is verbatim" a checkable claim.
#
# Skips (exit 0) where sha256sum is unavailable: the check is a rot-guard for
# the vendoring host, not a portability requirement.
set -eu
cd "$(dirname "$0")/.."

PIN_H=src/qlang/io/q_strfmt_pin.h
command -v sha256sum >/dev/null || exit 0

actual=$(find third_party/fmt -type f ! -path 'third_party/fmt/stub/*' ! -name 'README.peachq.md' |
         LC_ALL=C sort | xargs sha256sum | sha256sum | cut -d' ' -f1)

[ "${1:-}" = "--print" ] && { echo "$actual"; exit 0; }

want=$(sed -n 's/.*PQFMT_SRC_SHA256 "\([0-9a-f]*\)".*/\1/p' "$PIN_H")
[ "$actual" = "$want" ] && exit 0
cat >&2 <<EOF
fmt-pin: third_party/fmt does not match its recorded pin.
  recorded ($PIN_H): $want
  actual:                                $actual
If you deliberately re-vendored fmt, update PQFMT_DUCKDB_PIN and
PQFMT_SRC_SHA256 in that header (see third_party/fmt/README.peachq.md).
EOF
exit 1

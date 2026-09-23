#!/bin/sh
# gen-hl-names.sh — bake the REPL highlighter's builtin-name table from the help TSV.
#
# Usage: gen-hl-names.sh <output.h> <help-builtins.tsv> <help-builtins-gaps.tsv>
# Emits `static const char* const Q_HL_NAMES[]`, byte-sorted for bsearch: every distinct
# TSV name the highlighter can look up (an identifier, dotted or bare, or an `N:` verb)
# minus the gaps file's `unimplemented` rows.  Glyphs, `\` commands and the synthetic
# doc topics (`-p`, `'type`) are dropped: the highlighter colours those by shape.
# POSIX sh + awk + sort, like tools/gen-bootstrap.sh; runs on the build host.
set -eu

out=$1
tsv=$2
gaps=$3

{
    printf '/* AUTO-GENERATED from %s minus %s by tools/gen-hl-names.sh — DO NOT EDIT. */\n' "$tsv" "$gaps"
    printf '#ifndef PEACHQ_HL_NAMES_GEN_H\n#define PEACHQ_HL_NAMES_GEN_H\n'
    printf 'static const char* const Q_HL_NAMES[] = {\n'
    awk -F '\t' '
        /^#/ && !/\t/ { next }
        FILENAME == ARGV[1] { if ($2 == "unimplemented") drop[$1] = 1; next }
        $1 ~ /^(\.?[A-Za-z][A-Za-z0-9._]*|[0-9]+:)$/ && !($1 in drop) { print $1 }
    ' "$gaps" "$tsv" | LC_ALL=C sort -u | awk '{ printf "    \"%s\",\n", $0 }'
    printf '};\n#endif\n'
} > "$out.tmp"
mv "$out.tmp" "$out"

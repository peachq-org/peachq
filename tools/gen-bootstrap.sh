#!/bin/sh
# gen-bootstrap.sh — turn the authored .q files into a generated C header that
# exposes their bytes as `static const char OPENQ_BOOTSTRAP[]`. Part of the
# openq embedded-bootstrap pipeline (ARCHITECTURE.md BIGDECISION): src/qlang/
# q.q + dotq.q are the source of truth, this bakes them into the binary. DO NOT
# edit the generated output — edit the .q sources and rebuild.
#
# Usage: gen-bootstrap.sh <output.h> <input.q>...
# Inputs are concatenated IN ORDER — the load order contract (q.q keyword
# definitions load before dotq.q, which may use them; never the reverse).
# Portable: POSIX sh + awk only; runs on the BUILD host (native and mingw
# cross-builds alike), so there is no target-side dependency.
set -eu

out=$1
shift

# Symbol/guard names. Default (no SYMBOL) reproduces the historical dotq_gen.h
# output BYTE-FOR-BYTE (same symbol AND guard). SYMBOL=... supplies a distinct
# symbol for a second embedded header (pq_gen.h) with a guard derived from it.
# SYMBOL is a build-internal constant set only by the Makefile, never user input.
if [ -n "${SYMBOL:-}" ]; then
    sym=$SYMBOL
    guard="${SYMBOL}_H"
else
    sym=OPENQ_BOOTSTRAP
    guard=OPENQ_DOTQ_GEN_H
fi

# Escape each source line for a C string literal: strip a trailing CR (CRLF
# safety), backslash first, then double-quote; emit one "..\n" chunk per line
# so the embedded text keeps its newlines (the loader splits on '\n' and skips
# blank / `/`-comment lines). One chunk per line keeps the seed's inner quotes
# (e.g. .Q.b6's "..+/") correctly escaped.
#
# The escaping walks the line CHARACTER BY CHARACTER rather than using gsub.
# gsub's REPLACEMENT text has its own escaping layer, and mawk and gawk disagree
# about how many backslashes it eats — the same script emitted `\"` on mawk and
# `\\"` on gawk, so the build passed locally and broke on the CI runner (which
# defaults to gawk). Concatenating plain string literals has no second layer, so
# every awk produces identical bytes.
{
    printf '/* AUTO-GENERATED from %s by tools/gen-bootstrap.sh — DO NOT EDIT. */\n' "$*"
    printf '#ifndef %s\n#define %s\n' "$guard" "$guard"
    printf 'static const char %s[] =\n' "$sym"
    awk '
        {
            sub(/\r$/, "");
            out = ""; n = length($0);
            for (i = 1; i <= n; i++) {
                c = substr($0, i, 1);
                # awk comments are #, not /* */ — a /*..*/ here parses as a regex.
                if      (c == "\\") out = out "\\\\";   # one backslash -> two
                else if (c == "\"")  out = out "\\\"";    # one quote -> backslash quote
                else                out = out c;
            }
            printf "\"%s\\n\"\n", out;
        }
        END { print ";" }
    ' "$@"
    printf '#endif /* %s */\n' "$guard"
} > "$out"

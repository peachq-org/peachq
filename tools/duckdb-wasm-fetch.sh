#!/usr/bin/env bash
# duckdb-wasm-fetch — fetch DuckDB's browser build into build/duckdb-wasm/<version>/ (the layout the site serves),
# sha256-checked against the committed pin table (wasm/duckdb-wasm.pin); a file that verifies is not re-fetched.
#
#   tools/duckdb-wasm-fetch.sh [<version>]    default: the pinned version
set -euo pipefail
cd "$(dirname "$0")/.."

PIN=wasm/duckdb-wasm.pin

_sha() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | cut -d' ' -f1; }
die() { echo "duckdb-wasm-fetch: $*" >&2; exit 1; }

version="${1:-}"
[ -n "$version" ] || version="$(awk '$1 == "pin" { print $2 }' "$PIN")"
case "$version" in *[!A-Za-z0-9._-]*|'') die "bad version '$version'";; esac

rows="$(awk -v v="$version" '$1 == v' "$PIN")"
[ -n "$rows" ] || die "$PIN has no rows for version $version"

dir="build/duckdb-wasm/$version"
while read -r _ path sha url; do
  case "$path" in /*|*..*) die "$PIN: unsafe path '$path'";; esac
  f="$dir/$path"
  if [ ! -f "$f" ] || [ "$(_sha "$f")" != "$sha" ]; then
    mkdir -p "$(dirname "$f")"
    curl -fsSL -o "$f.part" "$url" || die "$url: download failed"
    got="$(_sha "$f.part")"
    [ "$got" = "$sha" ] || { rm -f "$f.part"; die "$path: sha256 $got does not match the pinned $sha"; }
    mv "$f.part" "$f"
  fi
  echo "ok  $path  $sha"
done <<< "$rows"
echo "== $dir verified against $PIN"

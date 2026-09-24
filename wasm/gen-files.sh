#!/bin/sh
# gen-files.sh SRC OUT — stage SRC's files (bar termbox/, the terminal games) as OUT/files/
# and write OUT/files.json: [{"path","size","sha256"}], the manifest engine.js mounts lazily.
set -eu
src=$1 out=$2
sha() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | cut -d' ' -f1; }

rm -rf "$out/files"
mkdir -p "$out/files"
list=$(cd "$src" && find . -type f ! -path './termbox/*' | sed 's#^\./##' | LC_ALL=C sort)
{
  printf '['
  sep=''
  for p in $list; do
    case $p in *[!A-Za-z0-9._/-]*) echo "gen-files.sh: unsupported file name: $p" >&2; exit 1;; esac
    mkdir -p "$out/files/$(dirname "$p")"
    cp "$src/$p" "$out/files/$p"
    printf '%s\n  {"path": "%s", "size": %s, "sha256": "%s"}' "$sep" "$p" "$(wc -c < "$src/$p" | tr -d ' ')" "$(sha "$src/$p")"
    sep=','
  done
  printf '\n]\n'
} > "$out/files.json"

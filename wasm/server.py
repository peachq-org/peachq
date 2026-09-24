#!/usr/bin/env python3
"""Zero-dependency static server for the peachq WASM REPL (the wasm build's output dir).

Serves .wasm as application/wasm and answers single `Range: bytes=a-b` requests with 206:
the Worker mounts files with emscripten's createLazyFile, which HEADs a file for its length
and then reads it in Range chunks. Stdlib http.server does neither Range nor 206.

    python3 wasm/server.py                  # http://localhost:8000, serving build/wasm/www
    python3 wasm/server.py 9000 some/dir    # port 0 picks a free port
"""
import http.server
import os
import re
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
DIR = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "wasm", "www"))


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

    def __init__(self, *a, **k):
        super().__init__(*a, directory=DIR, **k)

    def end_headers(self):
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def send_head(self):
        m = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        path = self.translate_path(self.path)
        if not m or not os.path.isfile(path):
            return super().send_head()
        f = open(path, "rb")
        size = os.fstat(f.fileno()).st_size
        start = int(m[1])
        end = min(int(m[2]) if m[2] else size - 1, size - 1)
        if start > end:
            f.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        return _Slice(f, end - start + 1)


class _Slice:
    """The file object send_head hands back, cut to the requested range."""

    def __init__(self, f, n):
        self.f, self.left = f, n

    def read(self, n=-1):
        n = self.left if n < 0 else min(n, self.left)
        data = self.f.read(n)
        self.left -= len(data)
        return data

    def close(self):
        self.f.close()


if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("", PORT), Handler) as httpd:
        print(f"peachq WASM REPL: http://localhost:{httpd.server_address[1]}/  (serving {DIR})", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")

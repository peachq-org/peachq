#!/usr/bin/env python3
"""Zero-dependency static preview server for the openq WASM REPL.

Serves the wasm/ directory (index.html + peachq.js/.wasm) with the correct
MIME type for .wasm so the browser streams/instantiates it. Stdlib only —
upstream's livereload dependency is intentionally avoided to keep with the
project's zero-dependency ethos.

    python3 wasm/server.py            # http://localhost:8000
    python3 wasm/server.py 9000       # custom port
"""
import http.server
import os
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
DIR = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=DIR, **k)

    def end_headers(self):
        # Cross-origin isolation headers are harmless here and future-proof the
        # page if it ever needs SharedArrayBuffer / threads.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


# Ensure .wasm is served as application/wasm (older Pythons may lack it).
Handler.extensions_map[".wasm"] = "application/wasm"

if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("", PORT), Handler) as httpd:
        print(f"openq WASM REPL: http://localhost:{PORT}/  (serving {DIR})")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")

#!/usr/bin/env python3
# Preview server for _site/: same as `python3 -m http.server`, but with
# caching disabled -- matches every machine's own web/devserve.py. Without
# this, a browser can keep serving a stale wasm module/app.js/index.html
# after `make site` re-stages fresh ones underneath it, which looks
# exactly like a regression that isn't actually there.
import http.server
import sys

PORT = int(sys.argv[1])
DIR = sys.argv[2]
BIND = sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIR, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()


with http.server.ThreadingHTTPServer((BIND, PORT), Handler) as httpd:
    httpd.serve_forever()

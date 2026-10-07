#!/usr/bin/env python3
# Local dev server: serves web/ on the LAN with caching disabled.
import http.server, os, sys

os.chdir(os.path.dirname(os.path.abspath(__file__)))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()


with http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler) as httpd:
    print(f"serving web/ on http://0.0.0.0:{PORT}  (no-cache)")
    httpd.serve_forever()

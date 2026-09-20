#!/usr/bin/env python3
# Local dev server: serves web/ on the LAN with caching disabled.
import http.server, os, ssl, sys

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
https = "--https" in sys.argv[1:]
args = [a for a in sys.argv[1:] if a != "--https"]
PORT = int(args[0]) if args else 8000
TLS_DIR = os.path.normpath(os.path.join(HERE, "..", "..", ".preview-tls"))


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()


httpd = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
scheme = "http"
if https:
    cert = os.path.join(TLS_DIR, "cert.pem")
    key = os.path.join(TLS_DIR, "key.pem")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    scheme = "https"
print(f"serving web/ on {scheme}://0.0.0.0:{PORT}  (no-cache)")
httpd.serve_forever()

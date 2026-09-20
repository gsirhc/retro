#!/usr/bin/env python3
# Preview server for _site/: same as `python3 -m http.server`, but with
# caching disabled -- matches every machine's own web/devserve.py. Without
# this, a browser can keep serving a stale wasm module/app.js/index.html
# after `make site` re-stages fresh ones underneath it, which looks
# exactly like a regression that isn't actually there.
#
# Optional TLS (5th/6th args: cert, key): the deployed site is always
# https://, but a LAN preview served plain http:// from anything other than
# localhost itself is a browser "insecure context" -- Chrome disables
# AudioWorklet entirely there (see pc486/PC486_REVIEW.md, no sound over a
# plain-http LAN preview), and other secure-context-gated APIs could hit the
# same wall later. `make preview-cert` generates a locally-trusted cert via
# mkcert covering localhost/this Mac's LAN name+IP; Makefile's preview
# targets pass it here automatically when present.
import http.server
import os
import ssl
import sys

PORT = int(sys.argv[1])
DIR = sys.argv[2]
BIND = sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"
CERT = sys.argv[4] if len(sys.argv) > 4 else None
KEY = sys.argv[5] if len(sys.argv) > 5 else None


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIR, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()

    def send_head(self):
        # Transparently serve `<path>` from a precompressed `<path>.gz`
        # sibling (Content-Encoding: gzip) when one exists and the client
        # accepts gzip -- virtually every browser always does. A browser's
        # fetch().arrayBuffer() hands back the body already decompressed
        # regardless of what Content-Encoding moved it over the wire, so
        # this needs no front-end code changes; see pc486/web/Makefile's
        # `disks/freedos-hdd.img.gz` rule for what generates the sibling
        # and why (a mostly-unwritten FAT image gzips down to ~1/4 size).
        if "gzip" in self.headers.get("Accept-Encoding", ""):
            path = self.translate_path(self.path)
            gz_path = path + ".gz"
            if not os.path.isdir(path) and os.path.isfile(gz_path):
                try:
                    f = open(gz_path, "rb")
                except OSError:
                    self.send_error(404, "File not found")
                    return None
                fs = os.fstat(f.fileno())
                self.send_response(200)
                self.send_header("Content-type", self.guess_type(path))
                self.send_header("Content-Encoding", "gzip")
                self.send_header("Content-Length", str(fs.st_size))
                self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
                self.end_headers()
                return f
        return super().send_head()


class Server(http.server.ThreadingHTTPServer):
    tls_context = None

    def process_request_thread(self, request, client_address):
        # The naive approach -- wrapping the *listening* socket so accept()
        # itself does the TLS handshake -- runs that handshake on the single
        # accept loop shared by every client. One slow or stalled handshake
        # (flaky WiFi, a browser retry, anything short of a clean connect)
        # then blocks accept() outright, so no other request -- even a
        # brand new one from localhost -- can get in until it clears: every
        # tab looks hung at once. Accepting the plain socket instead (fast,
        # no handshake) and only wrapping it here, inside this request's own
        # worker thread ThreadingMixIn already spawned per-connection, means
        # a stuck handshake ties up just that one thread.
        if self.tls_context is not None:
            try:
                request = self.tls_context.wrap_socket(request, server_side=True)
            except (ssl.SSLError, OSError):
                try:
                    request.close()
                except OSError:
                    pass
                return
        super().process_request_thread(request, client_address)


httpd = Server((BIND, PORT), Handler)
if CERT and KEY:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=CERT, keyfile=KEY)
    httpd.tls_context = ctx
with httpd:
    httpd.serve_forever()

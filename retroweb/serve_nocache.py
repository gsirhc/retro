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
#
# Optional --watch: poll source files, rebuild/restage into _site/, and
# live-reload open tabs via Server-Sent Events at /__livereload. Optional
# --machine=NAME limits wasm rebuilds to one emulator (root + shared still
# restage). See watch_site.py.
import http.server
import io
import os
import ssl
import sys
import threading

PORT = None
DIR = None
BIND = "0.0.0.0"
CERT = None
KEY = None
WATCH = False
WATCH_MACHINE = None


def livereload_script(generation):
    # Bake the current generation into the URL so a fresh tab does not
    # immediately reload against an already-bumped counter.
    return (
        "<script>(function(){"
        "if(window.__retroLivereload)return;window.__retroLivereload=1;"
        "function connect(g){"
        "var es=new EventSource('/__livereload?g='+g);"
        "es.onmessage=function(ev){es.close();location.reload();};"
        "es.onerror=function(){es.close();setTimeout(function(){connect(g);},1500);};"
        "}"
        "connect(%d);"
        "})();</script>"
    ) % generation


def parse_args(argv):
    global PORT, DIR, BIND, CERT, KEY, WATCH, WATCH_MACHINE
    positional = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--watch":
            WATCH = True
        elif a.startswith("--machine="):
            WATCH_MACHINE = a.split("=", 1)[1] or None
        elif a == "--machine":
            i += 1
            if i >= len(argv):
                sys.exit("serve_nocache.py: --machine needs a name")
            WATCH_MACHINE = argv[i]
        elif a.startswith("-"):
            sys.exit("serve_nocache.py: unknown option %s" % a)
        else:
            positional.append(a)
        i += 1
    if len(positional) < 2:
        sys.exit(
            "usage: serve_nocache.py PORT DIR [BIND [CERT KEY]] [--watch] [--machine NAME]"
        )
    PORT = int(positional[0])
    DIR = positional[1]
    if len(positional) > 2:
        BIND = positional[2]
    if len(positional) > 3:
        CERT = positional[3]
    if len(positional) > 4:
        KEY = positional[4]
    if WATCH_MACHINE and not WATCH:
        WATCH = True


parse_args(sys.argv[1:])


class LiveReload:
    def __init__(self):
        self._cond = threading.Condition()
        self._generation = 0

    def generation(self):
        with self._cond:
            return self._generation

    def bump(self):
        with self._cond:
            self._generation += 1
            self._cond.notify_all()

    def wait(self, after, timeout=25.0):
        with self._cond:
            if self._generation > after:
                return self._generation
            self._cond.wait(timeout=timeout)
            return self._generation


livereload = LiveReload()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIR, **kwargs)

    def log_message(self, fmt, *args):
        # Keep /__livereload long-poll noise out of the preview log
        if self.path.startswith("/__livereload"):
            return
        super().log_message(fmt, *args)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()

    def do_GET(self):
        if WATCH and self.path.split("?", 1)[0] == "/__livereload":
            self._sse_livereload()
            return
        super().do_GET()

    def _sse_livereload(self):
        # Long-poll SSE: hold until generation > g, or send a comment
        # keepalive on timeout so EventSource reconnects without reloading.
        after = 0
        q = self.path.split("?", 1)
        if len(q) == 2:
            for part in q[1].split("&"):
                if part.startswith("g="):
                    try:
                        after = int(part[2:])
                    except ValueError:
                        pass
        gen = livereload.wait(after, timeout=25.0)
        if gen > after:
            body = ("id: %d\ndata: reload\n\n" % gen).encode("ascii")
        else:
            body = b": keepalive\n\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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

        if WATCH:
            injected = self._send_html_with_livereload()
            if injected is not None:
                return injected
        return super().send_head()

    def _send_html_with_livereload(self):
        path = self.translate_path(self.path)
        if os.path.isdir(path):
            for index in ("index.html", "index.htm"):
                index = os.path.join(path, index)
                if os.path.isfile(index):
                    path = index
                    break
            else:
                return None
        if not path.endswith(".html") and not path.endswith(".htm"):
            return None
        if not os.path.isfile(path):
            return None
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            return None
        # Only inject into text HTML we can decode
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            return None
        if "window.__retroLivereload" in text:
            return None
        snippet = livereload_script(livereload.generation())
        lower = text.lower()
        idx = lower.rfind("</body>")
        if idx != -1:
            text = text[:idx] + snippet + text[idx:]
        else:
            text = text + snippet
        out = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(out)))
        try:
            fs = os.stat(path)
            self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
        except OSError:
            pass
        self.end_headers()
        return io.BytesIO(out)


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


if WATCH:
    from watch_site import MACHINES, SiteWatcher
    if WATCH_MACHINE and WATCH_MACHINE not in MACHINES:
        sys.exit(
            "serve_nocache.py: unknown --machine %r (want one of %s)"
            % (WATCH_MACHINE, ", ".join(MACHINES))
        )
    SiteWatcher(notify_reload=livereload.bump, only_machine=WATCH_MACHINE).start()

httpd = Server((BIND, PORT), Handler)
if CERT and KEY:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=CERT, keyfile=KEY)
    httpd.tls_context = ctx
with httpd:
    httpd.serve_forever()

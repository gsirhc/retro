#!/usr/bin/env python3
# Preview server for _site/: http.server with caching disabled, so a restaged
# wasm/app.js is never served stale.
#
# Optional TLS (5th/6th args: cert, key) from `make preview-cert`; plain http on
# a LAN address is an insecure context and disables AudioWorklet.
# --watch: rebuild/restage on source changes and live-reload tabs via SSE at
# /__livereload. --machine=NAME limits wasm rebuilds. See watch_site.py.
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
    # fresh tab must not reload against an already-bumped counter
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
        # keep long-poll noise out of the log
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
        # hold until generation > g; comment keepalive on timeout
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
        # serve <path>.gz with Content-Encoding: gzip when it exists and the client
        # accepts gzip; fetch() decompresses transparently (see pc486/web/Makefile)
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
        # handshake here in the per-connection thread, not on the listening
        # socket, so one stalled handshake can't block accept() for everyone
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

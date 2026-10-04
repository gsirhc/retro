#!/usr/bin/env python3
# Local / Playwright server for web/: LAN-reachable, Cache-Control: no-store,
# and the same transparent .gz sibling serving as retroweb/serve_nocache.py
# (so disks/freedos-hdd.img ships as ~6MB gzip rather than 256MB raw on every
# page load — without this, Playwright remounts dominate CI wall clock).
import http.server, os, sys

os.chdir(os.path.dirname(os.path.abspath(__file__)))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        path = self.translate_path(self.path)
        if (
            "gzip" in self.headers.get("Accept-Encoding", "")
            and not path.endswith(".gz")
            and os.path.isfile(path + ".gz")
        ):
            ctype = self.guess_type(path)
            try:
                f = open(path + ".gz", "rb")
            except OSError:
                self.send_error(404, "File not found")
                return None
            fs = os.fstat(f.fileno())
            self.send_response(200)
            self.send_header("Content-type", ctype)
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(fs.st_size))
            self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
            self.end_headers()
            return f
        return super().send_head()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()


if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler) as httpd:
        print(f"serving web/ on http://0.0.0.0:{PORT}  (no-cache, gzip siblings)")
        httpd.serve_forever()

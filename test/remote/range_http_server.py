#!/usr/bin/env python3
# Minimal range-capable static HTTP server for the remote-search integration test.
# Unlike `python3 -m http.server`, this honors `Range:` requests (HTTP 206), which the
# Phase B remote search needs: an index larger than zim_remote_search_max_local_index is
# read in place via byte ranges instead of being downloaded. Usage:
#   range_http_server.py <port> <dir> [bind]
import http.server, os, re, sys

PORT = int(sys.argv[1])
DIRECTORY = sys.argv[2]
BIND = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"


class H(http.server.BaseHTTPRequestHandler):
    def _file(self):
        return os.path.join(DIRECTORY, self.path.lstrip("/").split("?")[0])

    def do_HEAD(self):
        p = self._file()
        if not os.path.isfile(p):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(os.path.getsize(p)))
        self.end_headers()

    def do_GET(self):
        p = self._file()
        if not os.path.isfile(p):
            self.send_error(404)
            return
        size = os.path.getsize(p)
        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d+)-(\d*)", rng)
            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else size - 1
            end = min(end, size - 1)
            length = end - start + 1
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            self.end_headers()
            with open(p, "rb") as f:
                f.seek(start)
                self.wfile.write(f.read(length))
        else:
            self.send_response(200)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(p, "rb") as f:
                self.wfile.write(f.read())

    def log_message(self, *a):
        pass


http.server.ThreadingHTTPServer((BIND, PORT), H).serve_forever()

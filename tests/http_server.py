#!/usr/bin/env python3
"""Test server za mrezni izvor.

Podrzava:
  - PROPFIND Depth:1 -> 207 multistatus (ili 405 ako SRV_NO_DAV=1)
  - GET na folder    -> nginx-stil autoindex
  - GET na fajl      -> puni sadrzaj ili 206 Partial Content za Range
  - ubacivanje gresaka: SRV_FAIL_EVERY=N obara svaki N-ti zahtjev

Okruzenje:
  SRV_ROOT        korijen koji se servira (default: tekuci direktorij)
  SRV_PORT        port (default 8099)
  SRV_NO_DAV      "1" -> PROPFIND vraca 405, tjera fallback na autoindex
  SRV_FAIL_EVERY  N   -> svaki N-ti zahtjev prekida vezu bez odgovora
  SRV_NO_RANGE    "1" -> ignorise Range i vraca 200 sa cijelim fajlom
"""
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT       = os.path.abspath(os.environ.get("SRV_ROOT", "."))
PORT       = int(os.environ.get("SRV_PORT", "8099"))
NO_DAV     = os.environ.get("SRV_NO_DAV") == "1"
NO_RANGE   = os.environ.get("SRV_NO_RANGE") == "1"
FAIL_EVERY = int(os.environ.get("SRV_FAIL_EVERY", "0"))

_count = [0]


def local_path(url_path):
    rel = urllib.parse.unquote(url_path.split("?", 1)[0]).lstrip("/")
    p = os.path.abspath(os.path.join(ROOT, rel))
    if not p.startswith(ROOT):
        return None
    return p


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _fail_if_due(self):
        """Ubacuje 503, ne prekid veze.

        Prekid keep-alive veze NE valja kao injekcija greske: curl sam
        ponovo salje zahtjev kad se perzistentna veza neocekivano zatvori,
        pa bi greska bila nevidljiva klijentu i test ne bi testirao nista.
        503 je deterministican i prolazi kroz curl do nas.
        """
        if FAIL_EVERY <= 0:
            return False
        _count[0] += 1
        if _count[0] % FAIL_EVERY != 0:
            return False

        body = b"privremeno nedostupno"
        self.send_response(503)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return True

    def do_PROPFIND(self):
        if self._fail_if_due():
            return
        if NO_DAV:
            self.send_response(405)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        p = local_path(self.path)
        if not p or not os.path.isdir(p):
            self.send_error(404)
            return

        base = self.path if self.path.endswith("/") else self.path + "/"
        parts = ['<?xml version="1.0" encoding="utf-8"?>',
                 '<D:multistatus xmlns:D="DAV:">']

        def resp(href, is_dir, size):
            rt = "<D:collection/>" if is_dir else ""
            cl = "" if is_dir else f"<D:getcontentlength>{size}</D:getcontentlength>"
            return (f"<D:response><D:href>{href}</D:href><D:propstat><D:prop>"
                    f"<D:resourcetype>{rt}</D:resourcetype>{cl}"
                    f"</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
                    f"</D:propstat></D:response>")

        parts.append(resp(base, True, 0))
        for name in sorted(os.listdir(p)):
            full = os.path.join(p, name)
            quoted = urllib.parse.quote(name)
            if os.path.isdir(full):
                parts.append(resp(base + quoted + "/", True, 0))
            else:
                parts.append(resp(base + quoted, False, os.path.getsize(full)))
        parts.append("</D:multistatus>")

        body = "".join(parts).encode()
        self.send_response(207)
        self.send_header("Content-Type", 'text/xml; charset="utf-8"')
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        self._serve(head_only=True)

    def do_GET(self):
        self._serve(head_only=False)

    def _serve(self, head_only):
        if self._fail_if_due():
            return

        p = local_path(self.path)
        if not p or not os.path.exists(p):
            self.send_error(404)
            return

        if os.path.isdir(p):
            base = self.path if self.path.endswith("/") else self.path + "/"
            rows = ['<html><body><pre><a href="../">../</a>']
            for name in sorted(os.listdir(p)):
                q = urllib.parse.quote(name)
                suffix = "/" if os.path.isdir(os.path.join(p, name)) else ""
                rows.append(f'<a href="{q}{suffix}">{name}{suffix}</a>')
            rows.append("</pre></body></html>")
            body = "\n".join(rows).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head_only:
                self.wfile.write(body)
            return

        size = os.path.getsize(p)
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        partial = False

        if rng and not NO_RANGE and rng.startswith("bytes="):
            spec = rng[6:].split(",")[0]
            a, _, b = spec.partition("-")
            if a:
                start = int(a)
                end = int(b) if b else size - 1
            else:
                start = max(0, size - int(b))
            end = min(end, size - 1)
            partial = True

        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        if head_only:
            return

        with open(p, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    sys.stderr.write(f"http_server na 127.0.0.1:{PORT}, root={ROOT}\n")
    sys.stderr.flush()
    srv.serve_forever()

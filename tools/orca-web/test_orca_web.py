#!/usr/bin/env python3
"""Tests for orca_web_server.py: sign-in, file transfer and the WebSocket gate.

Needs no display: a fake VNC server on a unix socket stands in for Xvnc.
Run: python3 tools/orca-web/test_orca_web.py
"""

import base64
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "orca_web_server.py")
PASSWORD = "correct horse battery"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class FakeVnc(threading.Thread):
    """Accepts one connection, sends an RFB banner, then echoes."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(path)
        self.sock.listen(4)

    def run(self):
        while True:
            conn, _ = self.sock.accept()
            conn.sendall(b"RFB 003.008\n")
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                conn.sendall(data)


class OrcaWebTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t = cls.tmp.name
        cls.files = os.path.join(t, "files")
        cls.config = os.path.join(t, "config")
        cls.novnc = os.path.join(t, "novnc")
        os.makedirs(cls.novnc)
        with open(os.path.join(cls.novnc, "vnc.html"), "w") as f:
            f.write("<html>novnc</html>")
        sock = os.path.join(t, "vnc.sock")
        FakeVnc(sock).start()
        base = [sys.executable, SERVER, "--vnc-socket", sock, "--files", cls.files,
                "--config", cls.config, "--novnc", cls.novnc]
        subprocess.run(base + ["--set-password"], input=PASSWORD + "\n", text=True, check=True)
        cls.port = free_port()
        cls.proc = subprocess.Popen(base + ["--listen", "127.0.0.1", "--port", str(cls.port)],
                                    stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", cls.port)).close()
                break
            except OSError:
                time.sleep(0.05)
        cls.cookie = cls.sign_in(PASSWORD)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        cls.tmp.cleanup()

    @classmethod
    def request(cls, method, path, body=None, headers=None, cookie=None):
        c = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=10)
        h = dict(headers or {})
        if cookie:
            h["Cookie"] = cookie
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r, data

    @classmethod
    def sign_in(cls, password):
        body = "password=" + password.replace(" ", "+")
        r, _ = cls.request("POST", "/login", body, {"Content-Type": "application/x-www-form-urlencoded"})
        if r.status != 303:
            return None
        return r.getheader("Set-Cookie").split(";")[0]

    def test_password_file_is_a_hash(self):
        with open(os.path.join(self.config, "password")) as f:
            stored = f.read()
        self.assertTrue(stored.startswith("scrypt$"))
        self.assertNotIn(PASSWORD, stored)
        self.assertEqual(os.stat(os.path.join(self.config, "password")).st_mode & 0o777, 0o600)

    def test_wrong_password_is_refused(self):
        self.assertIsNone(self.sign_in("not the password"))

    def test_everything_needs_sign_in(self):
        for path in ["/", "/novnc/vnc.html", "/api/files", "/files/x.stl"]:
            r, _ = self.request("GET", path)
            self.assertIn(r.status, (303, 403), path)
        r, _ = self.request("PUT", "/files/x.stl", b"solid x")
        self.assertEqual(r.status, 403)
        r, _ = self.request("GET", "/login")
        self.assertEqual(r.status, 200)

    def test_forged_cookie_is_refused(self):
        expiry = str(int(time.time()) + 3600)
        r, _ = self.request("GET", "/api/files", cookie="orca_web=%s.%s" % (expiry, "0" * 64))
        self.assertEqual(r.status, 403)

    def test_signed_in_pages(self):
        r, data = self.request("GET", "/", cookie=self.cookie)
        self.assertEqual(r.status, 200)
        self.assertIn(b"Open the slicer", data)
        r, data = self.request("GET", "/novnc/vnc.html", cookie=self.cookie)
        self.assertEqual((r.status, data), (200, b"<html>novnc</html>"))

    def test_upload_list_download_delete(self):
        payload = os.urandom(300000)
        r, _ = self.request("PUT", "/files/part%20one.stl", payload, cookie=self.cookie)
        self.assertEqual(r.status, 201)
        r, data = self.request("GET", "/api/files", cookie=self.cookie)
        self.assertIn("part one.stl", [f["name"] for f in json.loads(data)])
        r, data = self.request("GET", "/files/part%20one.stl", cookie=self.cookie)
        self.assertEqual(data, payload)
        r, _ = self.request("DELETE", "/files/part%20one.stl", cookie=self.cookie)
        self.assertEqual(r.status, 200)
        self.assertFalse(os.path.exists(os.path.join(self.files, "part one.stl")))

    def test_paths_cannot_escape(self):
        for name in ["..%2Fescape.stl", "%2Fetc%2Fpasswd", ".hidden", "a%2Fb.stl"]:
            r, _ = self.request("PUT", "/files/" + name, b"x", cookie=self.cookie)
            self.assertEqual(r.status, 400, name)
        r, _ = self.request("GET", "/novnc/../../etc/passwd", cookie=self.cookie)
        self.assertEqual(r.status, 404)
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(self.files), "escape.stl")))

    def websocket(self, cookie):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET /websockify HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: binary\r\n" % key)
        if cookie:
            req += "Cookie: %s\r\n" % cookie
        s.sendall((req + "\r\n").encode())
        return s

    def test_websocket_needs_sign_in(self):
        s = self.websocket(None)
        self.assertIn(b" 403 ", s.recv(4096).split(b"\r\n")[0] + b" ")
        s.close()

    def test_websocket_reaches_vnc(self):
        s = self.websocket(self.cookie)
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += s.recv(4096)
        self.assertIn(b"101", buf.split(b"\r\n")[0])
        rest = buf.split(b"\r\n\r\n", 1)[1]
        while len(rest) < 14:
            rest += s.recv(4096)
        # One unmasked binary frame from the server carrying the RFB banner.
        self.assertEqual(rest[0] & 0x0F, 2)
        self.assertEqual(rest[2:2 + rest[1]], b"RFB 003.008\n")
        s.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""orca-web server: the OrcaSlicer GUI in a browser, behind a password.

One port serves everything:
  /                 landing page: open the slicer, upload models, download G-code
  /novnc/...        the noVNC client (static files from the distro's novnc package)
  /websockify       WebSocket, proxied to the Xvnc unix socket that shows the GUI
  /files/NAME       GET downloads, PUT uploads, DELETE removes a file in the shared folder
  /login, /logout   cookie sign-in (the password hash lives in the config dir)

Xvnc listens only on a 0600 unix socket, so the browser can reach the GUI only
through this server, and only after signing in. orca-web starts Xvnc, the window
manager and OrcaSlicer; this script is only the web side.
"""

import argparse
import hashlib
import hmac
import html
import json
import mimetypes
import os
import secrets
import shutil
import socket
import sys
import time
import urllib.parse
from http.server import ThreadingHTTPServer

from websockify.websocketproxy import ProxyRequestHandler

COOKIE = "orca_web"
SESSION_DAYS = 30
MAX_UPLOAD = 1 << 30  # 1 GiB
MODEL_EXT = {".stl", ".3mf", ".step", ".stp", ".obj", ".amf", ".svg", ".gcode", ".bgcode", ".json", ".zip", ".png", ".jpg"}


# ---------------------------------------------------------------- password ---

def hash_password(password, salt=None):
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.scrypt(password.encode(), salt=salt, n=2 ** 14, r=8, p=1, dklen=32)
    return "scrypt$%s$%s" % (salt.hex(), digest.hex())


def check_password(password, stored):
    try:
        kind, salt, digest = stored.strip().split("$")
    except ValueError:
        return False
    if kind != "scrypt":
        return False
    return hmac.compare_digest(hash_password(password, bytes.fromhex(salt)), stored.strip())


def read_secret(path, create=False):
    if create and not os.path.exists(path):
        os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_hex(32))
    with open(path) as f:
        return f.read().strip()


# ----------------------------------------------------------------- handler ---

class Handler(ProxyRequestHandler):
    server_version = "orca-web"

    # -- auth --------------------------------------------------------------
    def session_ok(self):
        cookies = self.headers.get("Cookie", "")
        for part in cookies.split(";"):
            name, _, value = part.strip().partition("=")
            if name == COOKIE:
                expiry, _, mac = value.partition(".")
                if not expiry.isdigit() or int(expiry) < time.time():
                    return False
                good = hmac.new(self.server.key, expiry.encode(), hashlib.sha256).hexdigest()
                return hmac.compare_digest(good, mac)
        return False

    def new_session_cookie(self):
        expiry = str(int(time.time()) + SESSION_DAYS * 86400)
        mac = hmac.new(self.server.key, expiry.encode(), hashlib.sha256).hexdigest()
        return "%s=%s.%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict%s" % (
            COOKIE, expiry, mac, SESSION_DAYS * 86400, "; Secure" if self.server.secure_cookie else "")

    def auth_connection(self):
        # websockify calls this before the WebSocket upgrade.
        if not self.session_ok():
            self.send_error(403, "sign in first")
            raise self.CClose(1008, "not signed in")

    def handle_upgrade(self):
        if self.path.split("?")[0] != "/websockify":
            self.send_error(404)
            return
        try:
            super().handle_upgrade()
        except self.CClose:
            pass

    # -- helpers -----------------------------------------------------------
    def reply(self, code, body, ctype="text/html; charset=utf-8", headers=()):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Frame-Options", "SAMEORIGIN")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def redirect(self, where, headers=()):
        self.send_response(303)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()

    def file_path(self, raw):
        name = urllib.parse.unquote(raw)
        if not name or name != os.path.basename(name) or name.startswith(".") or "\0" in name:
            return None
        return os.path.join(self.server.files, name)

    def list_files(self):
        out = []
        for entry in os.scandir(self.server.files):
            if entry.is_file() and not entry.name.startswith("."):
                st = entry.stat()
                out.append({"name": entry.name, "size": st.st_size, "mtime": int(st.st_mtime)})
        out.sort(key=lambda f: -f["mtime"])
        return out

    # -- routes ------------------------------------------------------------
    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/login":
            return self.reply(200, LOGIN_PAGE.replace("{error}", ""))
        if path == "/healthz":
            return self.reply(200, "ok\n", "text/plain")
        if not self.session_ok():
            if path.startswith("/files") or path.startswith("/api"):
                return self.reply(403, "sign in first\n", "text/plain")
            return self.redirect("/login")
        if path == "/":
            return self.reply(200, INDEX_PAGE)
        if path == "/logout":
            return self.redirect("/login", [("Set-Cookie", "%s=; Path=/; Max-Age=0" % COOKIE)])
        if path == "/api/files":
            return self.reply(200, json.dumps(self.list_files()), "application/json")
        if path.startswith("/files/"):
            fp = self.file_path(path[len("/files/"):])
            if not fp or not os.path.isfile(fp):
                return self.reply(404, "no such file\n", "text/plain")
            size = os.path.getsize(fp)
            self.send_response(200)
            self.send_header("Content-Type", mimetypes.guess_type(fp)[0] or "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition", "attachment; filename*=UTF-8''%s" % urllib.parse.quote(os.path.basename(fp)))
            self.end_headers()
            with open(fp, "rb") as f:
                shutil.copyfileobj(f, self.wfile)
            return
        if path.startswith("/novnc/"):
            rel = os.path.normpath(urllib.parse.unquote(path[len("/novnc/"):]))
            if rel.startswith("..") or os.path.isabs(rel):
                return self.reply(404, "not found\n", "text/plain")
            fp = os.path.join(self.server.novnc, rel)
            if not os.path.isfile(fp):
                return self.reply(404, "not found\n", "text/plain")
            with open(fp, "rb") as f:
                data = f.read()
            ctype = mimetypes.guess_type(fp)[0] or "application/octet-stream"
            if fp.endswith(".js"):
                ctype = "text/javascript"
            return self.reply(200, data, ctype)
        return self.reply(404, "not found\n", "text/plain")

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        if path != "/login":
            return self.reply(404, "not found\n", "text/plain")
        length = int(self.headers.get("Content-Length") or 0)
        form = urllib.parse.parse_qs(self.rfile.read(min(length, 4096)).decode(errors="replace"))
        password = (form.get("password") or [""])[0]
        if check_password(password, self.server.password_hash()):
            return self.redirect("/", [("Set-Cookie", self.new_session_cookie())])
        time.sleep(1.5)  # slow down guessing
        self.log_message("failed sign-in")
        return self.reply(401, LOGIN_PAGE.replace("{error}", '<p class="err">Wrong password.</p>'))

    def do_PUT(self):
        path = urllib.parse.urlsplit(self.path).path
        if not self.session_ok():
            return self.reply(403, "sign in first\n", "text/plain")
        if not path.startswith("/files/"):
            return self.reply(404, "not found\n", "text/plain")
        fp = self.file_path(path[len("/files/"):])
        if not fp:
            return self.reply(400, "bad file name\n", "text/plain")
        length = int(self.headers.get("Content-Length") or -1)
        if length < 0 or length > MAX_UPLOAD:
            return self.reply(413, "missing or too large Content-Length\n", "text/plain")
        tmp = os.path.join(self.server.files, ".upload-%s" % secrets.token_hex(6))
        try:
            with open(tmp, "wb") as f:
                left = length
                while left:
                    chunk = self.rfile.read(min(left, 1 << 20))
                    if not chunk:
                        raise IOError("upload cut short")
                    f.write(chunk)
                    left -= len(chunk)
            os.replace(tmp, fp)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        self.log_message("uploaded %s (%d bytes)", os.path.basename(fp), length)
        return self.reply(201, json.dumps({"name": os.path.basename(fp), "size": length}), "application/json")

    def do_DELETE(self):
        path = urllib.parse.urlsplit(self.path).path
        if not self.session_ok():
            return self.reply(403, "sign in first\n", "text/plain")
        fp = self.file_path(path[len("/files/"):]) if path.startswith("/files/") else None
        if not fp or not os.path.isfile(fp):
            return self.reply(404, "no such file\n", "text/plain")
        os.unlink(fp)
        return self.reply(200, "{}", "application/json")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, args):
        self.files = os.path.abspath(args.files)
        self.novnc = os.path.abspath(args.novnc)
        self.config = os.path.abspath(args.config)
        self.key = read_secret(os.path.join(self.config, "session.key"), create=True).encode()
        self.secure_cookie = args.secure_cookie
        # websockify's ProxyRequestHandler reads these off the server.
        self.target_host, self.target_port = None, None
        self.unix_target = args.vnc_socket
        self.ssl_target = None
        self.wrap_cmd = None
        self.unix_listen = False
        self.heartbeat = 30
        self.token_plugin = None
        self.auth_plugin = None
        self.web_auth = False
        self.only_upgrade = False
        self.file_only = True
        self.verbose = False
        self.daemon = False
        self.record = None
        self.run_once = False
        self.handler_id = 0
        os.makedirs(self.files, exist_ok=True)
        family = socket.AF_INET6 if ":" in addr[0] else socket.AF_INET
        self.address_family = family
        super().__init__(addr, Handler)

    def password_hash(self):
        try:
            with open(os.path.join(self.config, "password")) as f:
                return f.read()
        except FileNotFoundError:
            return ""


# ------------------------------------------------------------------- pages ---

STYLE = """
:root{--bg:#f6f7f9;--fg:#1d2330;--muted:#5d6675;--card:#fff;--line:#dde1e7;--acc:#0b8a6f;--accfg:#fff;--err:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#14171c;--fg:#e6e9ee;--muted:#9aa3b2;--card:#1d2128;--line:#2e343e;--acc:#26b38f;--accfg:#06130f;--err:#ff8a80}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:760px;margin:0 auto;padding:24px 16px}h1{font-size:22px;margin:0 0 4px}p.sub{color:var(--muted);margin:0 0 20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin-bottom:16px}
a.btn,button,input[type=submit]{display:inline-block;background:var(--acc);color:var(--accfg);border:0;border-radius:8px;padding:12px 18px;font:inherit;font-weight:600;text-decoration:none;cursor:pointer}
button.ghost{background:transparent;color:var(--muted);padding:4px 8px;font-weight:400}
input[type=password]{width:100%;padding:12px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit;margin:8px 0 12px}
table{width:100%;border-collapse:collapse;font-size:14px}td{padding:6px 4px;border-top:1px solid var(--line);vertical-align:middle}
td.n{word-break:break-all}td.s{color:var(--muted);white-space:nowrap;text-align:right}a{color:var(--acc)}
.drop{border:2px dashed var(--line);border-radius:10px;padding:18px;text-align:center;color:var(--muted);margin-bottom:12px}
.drop.on{border-color:var(--acc);color:var(--fg)}.err{color:var(--err)}small{color:var(--muted)}
"""

LOGIN_PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Orca Web</title><style>""" + STYLE + """</style></head><body><main>
<h1>Orca Web</h1><p class="sub">OrcaSlicer on netdash</p>
<form class="card" method="post" action="/login">{error}<label>Password<input type="password" name="password" autofocus autocomplete="current-password"></label>
<input type="submit" value="Sign in"></form></main></body></html>"""

INDEX_PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Orca Web</title><style>""" + STYLE + """</style></head><body><main>
<h1>Orca Web</h1><p class="sub">The OrcaSlicer GUI running on netdash. <a href="/logout">Sign out</a></p>
<div class="card"><a class="btn" href="/novnc/vnc.html?autoconnect=1&amp;reconnect=1&amp;resize=remote&amp;path=websockify" target="_blank">Open the slicer</a>
<p><small>Opens in a new tab. The slicer keeps running when you close it, so you can pick up where you left off.
Open and save files in the shared folder <b>FILES_DIR</b>.</small></p></div>
<div class="card"><b>Shared folder</b>
<div class="drop" id="drop">Drop models here, or <label><a href="#" onclick="document.getElementById('pick').click();return false">choose files</a></label>
<input type="file" id="pick" multiple hidden><div id="status"></div></div>
<table id="files"></table></div>
<script>
const fmt=n=>n>1e6?(n/1e6).toFixed(1)+' MB':n>1e3?(n/1e3).toFixed(0)+' kB':n+' B';
async function load(){const r=await fetch('/api/files');if(r.status==403){location='/login';return}
const t=document.getElementById('files');t.innerHTML='';
for(const f of await r.json()){const tr=t.insertRow();const u='/files/'+encodeURIComponent(f.name);
tr.innerHTML='<td class="n"><a href="'+u+'"></a></td><td class="s">'+fmt(f.size)+'<br>'+new Date(f.mtime*1000).toLocaleString()+'</td><td><button class="ghost" title="Delete">✕</button></td>';
tr.querySelector('a').textContent=f.name;
tr.querySelector('button').onclick=async()=>{if(confirm('Delete '+f.name+'?')){await fetch(u,{method:'DELETE'});load()}}}
if(!t.rows.length)t.innerHTML='<tr><td><small>Empty. Upload a model, or export G-code here from the slicer.</small></td></tr>'}
async function upload(files){const s=document.getElementById('status');
for(const f of files){s.textContent='Uploading '+f.name+'…';
const r=await fetch('/files/'+encodeURIComponent(f.name),{method:'PUT',body:f});
s.textContent=r.ok?'Uploaded '+f.name:'Failed: '+f.name+' ('+r.status+')'}load()}
const d=document.getElementById('drop');
d.ondragover=e=>{e.preventDefault();d.classList.add('on')};d.ondragleave=()=>d.classList.remove('on');
d.ondrop=e=>{e.preventDefault();d.classList.remove('on');upload(e.dataTransfer.files)};
document.getElementById('pick').onchange=e=>upload(e.target.files);
load();setInterval(load,10000);
</script></main></body></html>"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=6080)
    ap.add_argument("--vnc-socket", required=True)
    ap.add_argument("--files", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--novnc", default="/usr/share/novnc")
    ap.add_argument("--secure-cookie", action="store_true", help="mark the cookie Secure (behind an HTTPS proxy)")
    ap.add_argument("--set-password", action="store_true", help="read a new password from stdin and exit")
    args = ap.parse_args()

    if args.set_password:
        password = sys.stdin.readline().rstrip("\n")
        if len(password) < 8:
            sys.exit("orca-web: use at least 8 characters")
        os.makedirs(args.config, mode=0o700, exist_ok=True)
        path = os.path.join(args.config, "password")
        fd = os.open(path + ".new", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(hash_password(password) + "\n")
        os.replace(path + ".new", path)
        return

    global INDEX_PAGE
    INDEX_PAGE = INDEX_PAGE.replace("FILES_DIR", html.escape(os.path.abspath(args.files)))
    srv = Server((args.listen, args.port), args)
    if not srv.password_hash():
        print("orca-web: no password set; run `orca-web passwd` first. Sign-in is refused until then.", file=sys.stderr)
    print("orca-web: serving on http://%s:%d/" % (args.listen, args.port), file=sys.stderr, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

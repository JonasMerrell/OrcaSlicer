# orca-web: the OrcaSlicer GUI in a browser

`orca-web` runs the normal OrcaSlicer GUI from this fork on a virtual display
on the server and serves it as a web page. You can slice from a phone, tablet or
laptop with nothing installed. It is the full desktop GUI (Prepare, Preview,
every setting and preset), not a cut-down web slicer.

```
browser ── http :6080 ── orca_web_server.py ── unix socket (0600) ── Xvnc :7 ── openbox + orca-slicer
             sign-in cookie, noVNC client,
             shared-folder upload/download
```

* **Xvnc** (TigerVNC) is the X server. It listens only on a 0600 unix socket,
  with no TCP port, so nothing reaches the GUI except through the web server.
  OpenGL runs on Mesa's software renderer, which handles the 3D view well.
* **orca_web_server.py** (Python, using the distro's websockify module) serves
  one port. It handles password sign-in (a scrypt hash in `~/.config/orca-web/password`,
  and an HMAC-signed 30-day cookie), the noVNC client, the WebSocket to the
  display, and a shared folder for getting models in and G-code out.
* **openbox** maximizes the OrcaSlicer window, without decorations, to fill the tab.
  The noVNC page resizes the virtual screen to fit the browser window.
* If OrcaSlicer is closed or crashes, it starts again. Closing the browser tab
  leaves the session running, so you can pick it up later or from another device.

## Setup (openSUSE)

```bash
sudo zypper in xorg-x11-Xvnc novnc python313-websockify openbox
tools/orca-web/orca-web passwd        # choose the sign-in password
tools/orca-web/orca-web install       # systemd user service, starts now and at boot
sudo firewall-cmd --permanent --add-port=6080/tcp && sudo firewall-cmd --reload
```

`install` asks for `loginctl enable-linger` if the service would otherwise stop
when you log out. To open it from outside the house, connect through the
WireGuard VPN and browse to `http://10.8.0.1:6080/`. Don't forward the port to the
internet. If you want a public URL, put an HTTPS proxy with its own access control
in front of it and start the server with `--secure-cookie`.

## Using it

1. Open `http://<server>:6080/` and sign in.
2. **Shared folder**: drop STL/3MF/STEP files on the page. They land in
   `~/orca-web/files`, which is also OrcaSlicer's working directory, so they are
   right there in *Import* and *Open project*.
3. **Open the slicer**: the GUI opens in a new tab. Slice as usual.
4. Export G-code or 3MF to the same folder, then download it from the landing page.
   You can also use OrcaSlicer's own *Print*/*Send* to a configured printer.
   That is a deliberate click in the GUI, and orca-web never does it for you.

Keyboard shortcuts, the mouse wheel and right-drag all work in the noVNC tab. On a
touch screen, noVNC's side panel has a keyboard button and the clipboard.

## Commands

| command | what it does |
|---|---|
| `orca-web passwd` | set the password (existing sessions stay valid; delete `~/.config/orca-web/session.key` and restart to end them) |
| `orca-web install` / `uninstall` | add or remove the systemd user service |
| `orca-web status` | service state and URLs |
| `orca-web restart-gui` | restart only OrcaSlicer |
| `orca-web run` | foreground, what the service runs |

Settings go in `~/.config/orca-web/env` (shell syntax): `ORCA_WEB_PORT`,
`ORCA_WEB_LISTEN`, `ORCA_WEB_GEOMETRY`, `ORCA_WEB_DISPLAY`, `ORCA_WEB_FILES`,
`ORCA_BIN`.

The GUI uses the same `~/.config/OrcaSlicer` as the desktop app and the
`orca-slice` CLI, so presets edited in the browser are the ones the CLI slices with.

## Tests

`python3 tools/orca-web/test_orca_web.py` covers sign-in, forged cookies, path
escapes, upload/download, and the WebSocket gate to a fake VNC socket. It needs
no display.

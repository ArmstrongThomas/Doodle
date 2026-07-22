# Collab Doodle 3DS Client

Collab Doodle is a Nintendo 3DS homebrew client for drawing together on shared server-backed canvases. The bottom screen is the drawing surface, and the top screen shows a minimap, channel/status information, controls, and the current app version.

## Current Release

- Version: `1.5.0`

## Features

- Real-time collaborative drawing with the 3DS touchscreen.
- Top-screen minimap with viewport marker.
- Zoom levels: `0.5x`, `1x`, `2x`, and `4x`.
- Named channels: `main`, `sketch`, and `test`.
- Channel switch UI on the 3DS.
- Color picker, color sampling, brush size/shape controls, and quick eraser.
- Color-square picker with hue strip and circle/box/dither/eraser brush modes.
- Device identity, display name, backup-code recovery, and connected-user list.
- Mod/admin canvas tools with snapshot, clear, and selection-style fill rectangle.
- Compressed canvas snapshots using zlib.
- Cloudflare-proxied WSS realtime transport with automatic sleep/Wi-Fi recovery, heartbeat detection, and reconnect backoff.
- HTTPS update checks and downloads with certificate, size, and SHA-256 verification.
- App metadata/icon via SMDH, including the visible app version/build label.
- Optional `.cia` packaging when `makerom.exe` is installed.

## Screenshots

<img width="325" height="392" alt="image" src="https://github.com/user-attachments/assets/bf6f6af4-396c-473d-bd6e-8dc747d2e07c" />
<img width="320" height="386" alt="image" src="https://github.com/user-attachments/assets/764a0ff5-6994-46f2-b583-b112c4a5ef61" />
<img width="321" height="389" alt="image" src="https://github.com/user-attachments/assets/f45f9464-cb0e-4d4b-9f0c-d0374a1a65e2" />



## Controls

- Touch bottom screen: Draw.
- Circle Pad: Pan viewport.
- Hold LEFT D-Pad or A + drag stylus: Pan viewport.
- Hold L or R: Temporarily switch to eraser; release to return to the previous brush.
- Hold RIGHT D-Pad: Show zoom buttons on the right side of the bottom screen.
- Hold Y: Show zoom buttons on the left side of the bottom screen.
- Hold RIGHT D-Pad or Y + tap `+`: Zoom in.
- Hold RIGHT D-Pad or Y + tap `-`: Zoom out.
- START: Refresh canvas from server.
- SELECT: Open menu.
- Menu includes channels, connected users, controls, status, identity, admin tools, and exit.
- B or D-Pad DOWN: Toggle color picker.
- Color picker tabs: `COLOR` and `MOD`.
- MOD tab: Snapshot, clear canvas, and fill rectangle. Fill rectangle arms a selection; release stylus to fill using the selected color.
- Hold D-Pad UP or X + tap canvas: Sample color.

## Server Links

- Live canvas viewer: [https://doodle.7db.pw/](https://doodle.7db.pw/)
- Gallery: [https://doodle.7db.pw/gallery.html](https://doodle.7db.pw/gallery.html)

## Build Requirements

- devkitPro with devkitARM and libctru.
- 3DS zlib portlib installed.
- The project expects `DEVKITPRO` and `DEVKITARM` to be set.

Example PowerShell environment:

```powershell
$env:DEVKITPRO='C:\devkitPro'
$env:DEVKITARM='C:\devkitPro\devkitARM'
$env:PATH='C:\devkitPro\msys2\usr\bin;C:\devkitPro\tools\bin;C:\devkitPro\devkitARM\bin;' + $env:PATH
```

Build:

```powershell
make clean
make
```

## Build-Time Configuration

The Makefile exposes these primary release/server variables:

```make
APP_VERSION ?= 1.5.0
CHAT_ENABLED ?= 0
TEST_MODE ?= 0
LOCAL_SERVER_HOST ?= 192.168.1.46
REMOTE_TEST_SERVER_HOST ?= server2.rpgwo.org
LIVE_SERVER_HOST ?= doodle.7db.pw
LIVE_SERVER_WS_PORT ?= 443
LIVE_SERVER_WS_PATH ?= /ws/3ds
LIVE_SERVER_HTTPS_PORT ?= 443
LOCAL_SERVER_PORT ?= 3000
REMOTE_TEST_SERVER_HTTPS_PORT ?= 443
```

`TEST_MODE` selects the compiled server target:

- `0`: `wss://doodle.7db.pw/ws/3ds` for realtime traffic and `https://doodle.7db.pw` for updates.
- `1`: `ws://192.168.1.46:3000/ws/3ds` for local LAN testing. The updater is disabled by default.
- `2`: `wss://server2.rpgwo.org/ws/3ds` and HTTPS on port `443`. The updater is disabled by default.

Build the normal live release:

```powershell
make TEST_MODE=0
make verify-release-config TEST_MODE=0
```

Local 3dslink test build with updater prompts disabled:

```powershell
make TEST_MODE=1
```

Remote server2 test build with updater prompts disabled:

```powershell
make TEST_MODE=2
```

When `TEST_MODE=2`, successful builds are copied into the local web public directory with stable names:

- `../Doodle-Server/public/builds/CollabDoodle-test-server2.3dsx`
- `../Doodle-Server/public/builds/CollabDoodle-test-server2.cia` after `make cia TEST_MODE=2`

If that public directory is hosted by the server2 web instance, the FBI remote install URL can stay stable:

```text
https://server2.rpgwo.org/builds/CollabDoodle-test-server2.cia
```

Override the realtime host for a one-off local build:

```powershell
make TEST_MODE=1 SERVER_WS_HOST=192.168.4.50
```

Realtime and update endpoints can be overridden independently:

```powershell
make TEST_MODE=0 SERVER_WS_HOST=staging.example.com SERVER_WS_PORT=443 SERVER_WS_PATH=/ws/3ds SERVER_WS_SECURE=1 SERVER_HTTPS_HOST=downloads.example.com SERVER_HTTPS_PORT=443
```

CIA updater test build using the test title ID but keeping update checks enabled:

```powershell
make TEST_MODE=2 DISABLE_UPDATER=0
make cia TEST_MODE=2 DISABLE_UPDATER=0
```

`SERVER_WS_HOST`, `SERVER_WS_PORT`, `SERVER_WS_PATH`, and `SERVER_WS_SECURE` control drawing/presence networking. `SERVER_HTTPS_HOST` and `SERVER_HTTPS_PORT` control updater requests. The older `SERVER_HOST` convenience override remains supported and sets both hosts for existing test scripts; use the explicit variables when the realtime and updater hosts differ.

`make verify-release-config TEST_MODE=0` verifies the updater-enable bit and both production endpoints in the linked binary. The build also records all compile-affecting profile values in `build/.build-config`, so switching between release, test, host, or updater settings triggers the required rebuild instead of reusing stale objects.

The updater always uses TLS. If it is intentionally enabled for `TEST_MODE=1`, point `SERVER_HTTPS_HOST` and `SERVER_HTTPS_PORT` at a real HTTPS endpoint whose certificate matches the host; the default local port `3000` is intended for the plain local WebSocket server, not an insecure updater fallback.

Client hello/version checks, SMDH metadata, and the top-screen version label use the same build settings. `CHAT_ENABLED` is currently off for public builds. Any non-zero `TEST_MODE` marks the build as a test build and uses the test CIA title ID. Test modes disable update prompts/downloads by default so they can be sent with `3dslink` without publishing a live update. Override with `DISABLE_UPDATER=0` only when intentionally testing against HTTPS. Test builds display labels such as `1.5.0-test1` or `1.5.0-test2`.

## CIA Packaging

The default build still creates `Doodle.3dsx`. To also build `Doodle.cia`, install `makerom.exe` so it is available on PATH or at `C:\devkitPro\tools\bin\makerom.exe`; this setup was verified with Project_CTR `makerom-v0.18.4`. Then build and package:

```powershell
make
make cia
```

Local test CIA build:

```powershell
make TEST_MODE=1
make cia TEST_MODE=1
```

Release and test CIA builds use separate title IDs/product codes so they can be installed side by side. Release uses title ID `000400000CE47500`; test uses `000400000CE47600`.

## Running on Hardware

Copy the built `.3dsx` to the SD card, or send it with `3dslink` while the Homebrew Launcher netloader is waiting:

```powershell
3dslink -a <3ds-ip> -r 10 Doodle.3dsx
```

For the current public build, use:

```text
CollabDoodle-update.3dsx
```

## Updates

The client checks `https://doodle.7db.pw/api/updates/latest` in release builds. Manifest and artifact transfers use certificate-verified HTTPS; there is no plaintext HTTP fallback.

The updater is independent of the realtime WebSocket connection. If the initial WSS connection fails, the client attempts the HTTPS update path before presenting a terminal connection error. This preserves update recovery after future realtime protocol changes.

When an update is available:

- The client prompts before downloading.
- The download shows progress.
- The client requests the artifact matching the running package type: `3dsx` or `cia`.
- 3DSX builds stage the downloaded `.3dsx` beside the running app.
- CIA builds download to `sdmc:/cias/CollabDoodle-update.cia`, verify it, then install it in-app through AM.
- File size and SHA-256 are verified before replacement/install.
- After 3DSX install, close the app and reopen Collab Doodle from the Homebrew Launcher.
- After CIA install, Collab Doodle attempts to relaunch the installed title automatically.
- If automatic CIA relaunch is refused by APT, close and reopen Collab Doodle from HOME Menu.
- If in-app CIA install fails repeatedly on hardware, the staged CIA path can still be installed manually with FBI as a fallback.

Homebrew Launcher `.3dsx` apps do not currently support a reliable in-app relaunch of the freshly replaced file, so the final step is manual reopen.

### 1.4.4 migration compatibility

Version `1.4.4` clients still connect over direct raw TCP and cannot use Cloudflare's HTTP/WebSocket proxy for realtime traffic. Keep the server's temporary legacy TCP bridge enabled during the upgrade window. If in-app migration from 1.4.4 is required, its legacy update endpoint must also remain reachable long enough to publish the `1.5.0` update. Version `1.5.0` uses WSS and HTTPS; after adoption is sufficient, the legacy TCP listener can be disabled and the origin can be restricted to Cloudflare/tunnel traffic.

## Repository Notes

This directory is only the 3DS client. The Node/web server is not publicly available and lives beside it in `Doodle-Server` and owns:

- Channel persistence.
- Gallery snapshots.
- Browser live viewer.
- Update manifest and release artifact hosting.

## License

MIT. See the project license file for details.

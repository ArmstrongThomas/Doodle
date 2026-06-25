# Collab Doodle 3DS Client

Collab Doodle is a Nintendo 3DS homebrew client for drawing together on shared server-backed canvases. The bottom screen is the drawing surface, and the top screen shows a minimap, channel/status information, controls, and the current app version.

## Current Release

- Version: `1.2.0`

## Features

- Real-time collaborative drawing with the 3DS touchscreen.
- Top-screen minimap with viewport marker.
- Zoom levels: `0.5x`, `1x`, `2x`, and `4x`.
- Named channels: `main`, `sketch`, and `test`.
- Channel switch UI on the 3DS.
- Color picker, color sampling, brush size/shape controls, and hex color entry.
- Color-square picker with hue strip and circle/box/dither brush modes.
- Device identity, display name, backup-code recovery, and connected-user list.
- Mod/admin canvas tools with snapshot, clear, and selection-style fill rectangle.
- Compressed canvas snapshots using zlib.
- Server update checks with manifest, size, and SHA-256 verification.
- App metadata/icon via SMDH, including the visible app version.

## Screenshots

<img width="325" height="392" alt="image" src="https://github.com/user-attachments/assets/bf6f6af4-396c-473d-bd6e-8dc747d2e07c" />
<img width="320" height="386" alt="image" src="https://github.com/user-attachments/assets/764a0ff5-6994-46f2-b583-b112c4a5ef61" />
<img width="321" height="389" alt="image" src="https://github.com/user-attachments/assets/f45f9464-cb0e-4d4b-9f0c-d0374a1a65e2" />



## Controls

- Touch bottom screen: Draw.
- Hold LEFT D-Pad or A + drag stylus: Pan viewport.
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
- Hold D-Pad UP + tap canvas: Sample color.
- X: Enter hex color.

## Server Links

- Live canvas viewer: [http://server1.rpgwo.org:3000/](http://server1.rpgwo.org:3000/)
- Gallery: [http://server1.rpgwo.org:3000/gallery.html](http://server1.rpgwo.org:3000/gallery.html)
- Legacy gallery redirect: [http://server1.rpgwo.org/](http://server1.rpgwo.org/)

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

The Makefile exposes release/server variables:

```make
APP_VERSION ?= 1.2.0
SERVER_HOST ?= server1.rpgwo.org
SERVER_TCP_PORT ?= 3030
SERVER_HTTP_PORT ?= 3000
TEST_MODE ?= 0
```

Override them when building local test versions:

```powershell
make SERVER_HOST=192.168.1.46 SERVER_TCP_PORT=3030 SERVER_HTTP_PORT=3000
```

Local 3dslink test build with updater prompts disabled:

```powershell
make TEST_MODE=1 SERVER_HOST=192.168.1.46 SERVER_TCP_PORT=3030 SERVER_HTTP_PORT=3000
```

The same values are compiled into networking, updater requests, client hello/version checks, SMDH metadata, and the top-screen version label. `TEST_MODE=1` disables client-side update prompts/downloads so local builds can be sent with `3dslink` without publishing a live update.

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

The client checks the same Doodle server for `/api/updates/latest`.

When an update is available:

- The client prompts before downloading.
- The download shows progress.
- The downloaded `.3dsx` is staged beside the running app.
- File size and SHA-256 are verified before replacement.
- After install, close the app and reopen Collab Doodle from the Homebrew Launcher.

Homebrew Launcher `.3dsx` apps do not currently support a reliable in-app relaunch of the freshly replaced file, so the final step is manual reopen.

## Repository Notes

This directory is only the 3DS client. The Node/web server is not publicly available and lives beside it in `Doodle-Server` and owns:

- Channel persistence.
- Gallery snapshots.
- Browser live viewer.
- Update manifest and release artifact hosting.

## License

MIT. See the project license file for details.

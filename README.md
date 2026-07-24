# Collab Doodle 3DS Client

Collab Doodle is a Nintendo 3DS homebrew client for drawing together on shared server-backed canvases. Version 1.6 uses a compact hybrid layout: the top screen presents the canvas dashboard or selected-item details, while the bottom screen provides drawing controls, touch navigation, and actions.

## Current Release

- Version: `1.6.1`

## Features

- Real-time collaborative drawing with the 3DS touchscreen.
- Compact top-screen minimap with viewport marker and current-channel presence.
- Zoom levels: `0.5x`, `1x`, `2x`, and `4x`.
- Named channels: `main`, `sketch`, and `test`.
- Touch and button-complete channel, people, support, staff, profile, options, and help pages.
- Circle, square, dither, and eraser shapes with a shared `1.0`–`12.0` touch slider in `0.1` steps and live preview.
- Validated `#RRGGBB` entry, current/previous colors, and eight persistent favorite swatches.
- Device identity, display name, masked backup-code recovery, and grouped connected-user sessions.
- Staff canvas tools with snapshot, fill/erase selection previews, and confirmed clear.
- Ticket drafts, preview-before-send, status/attention indicators, and cursor-based paging history.
- Balanced, right-handed stylus, left-handed stylus, and custom control layouts.
- Shared connection, syncing, restriction, update, and fatal-error overlays.
- Compressed canvas snapshots using zlib.
- Cloudflare-proxied WSS realtime transport with automatic sleep/Wi-Fi recovery, heartbeat detection, and reconnect backoff.
- HTTPS update checks and downloads with certificate, size, and SHA-256 verification.
- App metadata/icon via SMDH, including the visible app version/build label.
- Optional `.cia` packaging when `makerom.exe` is installed.

## Controls

- Touch bottom screen: Draw.
- Circle Pad: Pan viewport.
- SELECT: Open the root menu from the canvas, or escape back to it from any page.
- Lists: Touch or D-Pad/Circle Pad to select, A to confirm, and B to go back.
- The default Balanced canvas layout is:
  - D-Pad DOWN or B: Open/close tools.
  - Hold D-Pad LEFT or A + drag stylus: Pan viewport.
  - Hold D-Pad UP or X + tap canvas: Sample color.
  - Hold D-Pad RIGHT or Y: Show the zoom overlay; tap `+` or `-`.
  - Hold L or R: Temporarily use Quick Eraser.
  - START: Refresh the current canvas.

Canvas bindings are read from the active preset. `Help & Rules` therefore always shows the live bindings instead of a second hard-coded controls list. SELECT, touch, Circle Pad, and menu navigation are intentionally not rebindable.

## UI and Navigation

The root menu contains:

- Channels
- People
- Support
- Staff Center (staff and administrators only)
- Profile
- Options
- Help & Rules
- Exit

The old standalone Status and Controls pages are gone. Connection status still appears automatically when the client is connecting, syncing, recovering, updating, restricted, or unable to continue. Controls now live in `Options > Controls & Presets`. Every intentional Exit action asks for confirmation before disconnecting; fatal-error and completed-update shutdowns remain immediate.

The drawing sheet has `Draw` and role-gated `Staff` tabs. Its controls are touch-only: D-Pad, A, and shoulder input do not move focus or activate palette items. Press the configured Tools button again to close the sheet. Draw separates shape from the shared `1.0`–`12.0` size slider, keeps Rainbow available to every user, and provides current/previous colors, validated hex entry, and eight favorite swatches. Tap a swatch to apply it. Choose `Save`, then a slot, to replace that favorite explicitly. Reset asks for confirmation before restoring black, white, red, yellow, green, cyan, blue, and magenta. Action toasts dismiss automatically after about 1.5 seconds or immediately when tapped. Rainbow always starts disabled.

Staff actions report pending, success, denied, or error results. Fill Selection and Erase Selection show the chosen rectangle before A applies it or B cancels. Clear always requires confirmation.

Support offers bug reports, feature requests, user reports, and My Tickets. “Report a user” opens the People picker, records the selected account/session and channel in the draft, then presents the same editable final preview used by every other request. Restricted accounts continue to see only appeal creation and their existing appeals.

## Options and Device Settings

Options is divided into:

- Controls & Presets
- Drawing & Palette
- Connection & About

Settings are device-local at `sdmc:/3ds/CollabDoodle/settings.ini`. Version 1 stores the active preset, two button slots for each of the six canvas actions, zoom-overlay side, last successfully loaded channel, brush shape/size (including one-decimal sizes), solid color, and eight palette colors. Changes are staged through `settings.ini.tmp`; the last valid file is kept as `settings.ini.bak`. Unknown keys and individually invalid values do not block startup, and a valid backup is used when the primary is corrupt.

The identity credential file is separate and is never modified or merged by settings recovery.

Editing a named preset changes its label to Custom. If a newly selected button is already assigned, the UI offers Swap or Cancel. The presets are:

- Balanced: paired D-Pad/face-button actions, L/R Quick Eraser, START Refresh.
- Right-handed stylus: D-Pad modifiers, L Quick Eraser, START Refresh.
- Left-handed stylus: face-button modifiers, R Quick Eraser, START Refresh.

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
APP_VERSION ?= 1.6.1
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

Client hello/version checks, SMDH metadata, and the top-screen version label use the same build settings. `CHAT_ENABLED` is currently off for public builds. Any non-zero `TEST_MODE` marks the build as a test build and uses the test CIA title ID. Test modes disable update prompts/downloads by default so they can be sent with `3dslink` without publishing a live update. Override with `DISABLE_UPDATER=0` only when intentionally testing against HTTPS. Test builds display labels such as `1.6.1-test1` or `1.6.1-test2`.

## Client Fixture Tests

The host fixture harness exercises settings parsing and independent validation, primary/backup recovery, atomic-save history, palette defaults, preset mappings, binding conflicts and swaps, per-frame semantic action consumption, shared hitbox edges, clipped text, and rotated framebuffer addressing.

On Windows with Visual Studio C++ Build Tools:

```powershell
make host-tests
```

On a POSIX host:

```sh
make host-tests HOST_CXX=c++
```

Before release, also run the client test build and production configuration verification:

```powershell
make TEST_MODE=1
make verify-release-config TEST_MODE=0
```

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

### Protocol compatibility

Version 1.6 keeps native protocol version `6`. Its hello advertises the additive `ui2-channel-info`, `ui2-presence-compact`, `ui2-ticket-cursor`, and `draw-size-tenths` capabilities and may include the last successfully loaded channel as `preferredChannel`. Type-4 draw packets use the existing seven-byte draw header but encode the size byte in tenths (`45` means `4.5`); type 1 remains the whole-size legacy packet. Capable servers send exact fractional packets to capable clients and rounded type-1 packets to older clients. Compact channel metadata, bounded/grouped presence with channel totals, compound ticket cursors (`updatedAt + id`), legacy channel-name arrays, and `beforeId` ticket requests remain supported, so 1.5 clients continue to connect and the server minimum supported version does not change.

Version `1.4.4` clients still connect over direct raw TCP and cannot use Cloudflare's HTTP/WebSocket proxy for realtime traffic. Keep the server's temporary legacy TCP bridge enabled only while that migration path is still required. Versions `1.5.0` and newer use WSS and HTTPS.

## Repository Notes

This directory is only the 3DS client. The Node/web server is not publicly available and lives beside it in `Doodle-Server` and owns:

- Channel persistence.
- Gallery snapshots.
- Browser live viewer.
- Update manifest and release artifact hosting.

## License

MIT. See the project license file for details.

# Collab Doodle 1.6.0

Version 1.6 is a full-client UI and quality-of-life pass that keeps the lightweight native framebuffer renderer and protocol version 6.

## Highlights

- Introduces a compact navy/teal/gray component system shared across both screens.
- Replaces the old destination list with Channels, People, Support, role-gated Staff Center, Profile, Options, Help & Rules, and Exit.
- Confirms intentional Exit actions from the root menu, restricted Support shell, and required onboarding/rules flow before disconnecting.
- Uses the top screen for readable context/details and the bottom screen for touch navigation and actions.
- Adds complete D-Pad, Circle Pad, A, B, touch, and fixed SELECT navigation behavior.
- Keeps connection/status handling as automatic overlays instead of a standalone page.
- Fits confirmation titles to the available modal width and guards touch transitions so one press cannot activate two views.

## Drawing and Palette

- Separates Circle, Square, Dither, and Eraser from numeric brush size.
- Adds a live brush preview, current/previous colors, and validated `#RRGGBB` entry.
- Uses a true square saturation/value picker with shared render and touch bounds.
- Adds eight device-local favorite swatches. Defaults are black, white, red, yellow, green, cyan, blue, and magenta.
- Swatches apply immediately; Save explicitly enters slot-assignment mode, and Reset confirms before restoring the defaults.
- Action toasts clear after about 1.5 seconds, can be tapped to dismiss, and are removed when the tool sheet closes.
- Moves Rainbow to Draw for every user. It starts disabled each launch.
- Gives staff Snapshot, Fill Selection, Erase Selection, and confirmed Clear actions with explicit result feedback.

## Pages and Identity

- Channels shows occupancy/current state and only remembers a switch after its canvas snapshot loads.
- People defaults to the current channel, groups authenticated sessions, distinguishes anonymous viewers, and provides staff moderation actions with confirmations.
- Support keeps in-memory drafts, previews new requests/replies before sending, and shows accurate cursor-based paging and attention state. Its Report a User flow selects from People and attaches the target account/session context to the report ticket.
- Restricted accounts receive a dedicated support-access flow for appeals instead of unrelated canvas pages.
- Profile separates display name, account ID, role/state, and recovery. Backup codes are masked by default; Reveal and Rotate are separate actions.
- Onboarding is streamlined into Welcome, Create or Recover, Display Name, Rules, recovery-code explanation, and Canvas.

## Controls and Settings

- Adds Balanced, Right-handed stylus, Left-handed stylus, and Custom layouts.
- Each of Tools, Pan, Sample, Zoom, Quick Eraser, and Refresh has two configurable button slots.
- Binding conflicts offer Swap or Cancel. SELECT, touch, Circle Pad, and menu navigation remain fixed.
- Stores versioned device settings at `sdmc:/3ds/CollabDoodle/settings.ini` with temporary-file commits and backup recovery.
- Persists controls, zoom-overlay side, last successful channel, brush shape/size, solid color, and the eight favorite colors.
- Ignores unknown keys and validates each field independently. Settings recovery never modifies the identity credential file.

## Server Compatibility

- Native protocol version remains 6 and the minimum supported version is unchanged.
- New clients advertise `ui2-channel-info`, `ui2-presence-compact`, and `ui2-ticket-cursor`, plus an optional preferred channel.
- Channel metadata, compact presence totals/grouping, and compound ticket cursors are additive and capability-gated.
- Existing channel arrays and legacy `beforeId` pagination remain available to 1.5 clients.
- Recovery no longer replaces an established identity; only a genuinely unused onboarding placeholder can be reclaimed.

## Verification

- `make host-tests` covers settings parsing/recovery, presets and rebinding, semantic input consumption, hitboxes, clipping, and framebuffer layout.
- Release validation still requires the full server suite, `make TEST_MODE=1`, and `make verify-release-config TEST_MODE=0`.
- Native-scale acceptance should be completed in Azahar and on Old and New 3DS hardware, including sleep/resume, reconnect, restricted support, settings failure, every destructive confirmation, and maximum-canvas drawing performance.

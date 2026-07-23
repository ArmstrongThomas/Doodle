#ifndef DOODLE_CLIENT_SETTINGS_H
#define DOODLE_CLIENT_SETTINGS_H

#include <stddef.h>

#include "input_bindings.h"

namespace Doodle
{

static const unsigned int CLIENT_SETTINGS_VERSION = 1;
static const int CLIENT_PALETTE_COLOR_COUNT = 8;
static const int CLIENT_CHANNEL_CAPACITY = 25;

struct Rgb8
{
    u8 r;
    u8 g;
    u8 b;
};

enum ClientBrushShape
{
    CLIENT_BRUSH_CIRCLE = 0,
    CLIENT_BRUSH_SQUARE,
    CLIENT_BRUSH_DITHER,
    CLIENT_BRUSH_ERASER,
    CLIENT_BRUSH_SHAPE_COUNT
};

// AUTO preserves the 1.5 behavior: D-Pad Right puts the controls on the
// right, while Y puts them on the left.
enum ZoomOverlaySide
{
    ZOOM_OVERLAY_AUTO = 0,
    ZOOM_OVERLAY_LEFT,
    ZOOM_OVERLAY_RIGHT,
    ZOOM_OVERLAY_SIDE_COUNT
};

struct ClientSettings
{
    unsigned int version;
    ControlPreset controlPreset;
    InputBindings bindings;
    ZoomOverlaySide zoomOverlaySide;
    char lastSuccessfulChannel[CLIENT_CHANNEL_CAPACITY];
    ClientBrushShape brushShape;
    int brushSize;
    Rgb8 solidColor;
    Rgb8 palette[CLIENT_PALETTE_COLOR_COUNT];
};

enum SettingsLoadResult
{
    SETTINGS_LOAD_DEFAULTS = 0,
    SETTINGS_LOAD_PRIMARY,
    SETTINGS_LOAD_BACKUP
};

enum SettingsSaveResult
{
    SETTINGS_SAVE_OK = 0,
    SETTINGS_SAVE_INVALID,
    SETTINGS_SAVE_PATH_TOO_LONG,
    SETTINGS_SAVE_OPEN_FAILED,
    SETTINGS_SAVE_WRITE_FAILED,
    SETTINGS_SAVE_BACKUP_FAILED,
    SETTINGS_SAVE_COMMIT_FAILED
};

const char *clientSettingsPath();
const char *settingsLoadResultLabel(SettingsLoadResult result);
const char *settingsSaveResultLabel(SettingsSaveResult result);

const char *clientBrushShapeToken(ClientBrushShape shape);
const char *clientBrushShapeLabel(ClientBrushShape shape);
bool clientBrushShapeFromToken(const char *text, ClientBrushShape &shape);

const char *zoomOverlaySideToken(ZoomOverlaySide side);
const char *zoomOverlaySideLabel(ZoomOverlaySide side);
bool zoomOverlaySideFromToken(const char *text, ZoomOverlaySide &side);

bool parseRgbHex(const char *text, Rgb8 &color);
bool formatRgbHex(Rgb8 color, char *buffer, size_t bufferSize);

void resetClientPalette(ClientSettings &settings);
void resetClientSettings(ClientSettings &settings);

// Applying Custom preserves the current bindings and only changes the label.
// Applying a named preset replaces all six bindings.
void applyClientControlPreset(ClientSettings &settings, ControlPreset preset);

// Successful edits to a named preset mark it Custom. UNCHANGED does not.
BindingEditResult editClientBinding(ClientSettings &settings, InputAction action,
                                    int slot, ButtonToken button,
                                    BindingConflictPolicy policy,
                                    BindingConflict *conflict);

bool setLastSuccessfulChannel(ClientSettings &settings, const char *channel);
bool clientSettingsAreValid(const ClientSettings &settings);
bool clientSettingsEqual(const ClientSettings &left, const ClientSettings &right);

// A null path uses sdmc:/3ds/CollabDoodle/settings.ini. Supplying a path is
// intended for fixture tests; sibling .tmp and .bak paths are used in either
// case.
SettingsLoadResult loadClientSettings(ClientSettings &settings,
                                      const char *path = NULL);
SettingsSaveResult saveClientSettings(const ClientSettings &settings,
                                      const char *path = NULL);

} // namespace Doodle

#endif

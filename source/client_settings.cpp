#include "client_settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace Doodle
{
namespace
{

static const char SETTINGS_PATH[] = "sdmc:/3ds/CollabDoodle/settings.ini";
static const size_t SETTINGS_PATH_BUFFER_SIZE = 320;
static const size_t SETTINGS_LINE_SIZE = 256;

static const Rgb8 DEFAULT_PALETTE[CLIENT_PALETTE_COLOR_COUNT] = {
    {0, 0, 0},
    {255, 255, 255},
    {255, 0, 0},
    {255, 255, 0},
    {0, 255, 0},
    {0, 255, 255},
    {0, 0, 255},
    {255, 0, 255}};

struct ParsedBinding
{
    bool valid;
    ActionBinding value;
};

static bool asciiEqualIgnoreCase(const char *left, const char *right)
{
    if (!left || !right)
        return false;
    while (*left && *right)
    {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static char *trim(char *text)
{
    if (!text)
        return text;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        ++text;
    size_t length = strlen(text);
    while (length > 0)
    {
        char last = text[length - 1];
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n')
            break;
        text[--length] = '\0';
    }
    return text;
}

static bool parseInteger(const char *text, long minimum, long maximum, long &value)
{
    if (!text || !*text)
        return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < minimum || parsed > maximum)
        return false;
    value = parsed;
    return true;
}

static int hexValue(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static bool rgbEqual(Rgb8 left, Rgb8 right)
{
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

static bool isChannelValid(const char *channel)
{
    if (!channel)
        return false;
    size_t length = strlen(channel);
    if (length >= CLIENT_CHANNEL_CAPACITY)
        return false;
    for (size_t i = 0; i < length; ++i)
    {
        unsigned char value = (unsigned char)channel[i];
        if (value < 0x21 || value > 0x7e)
            return false;
    }
    return true;
}

static bool makeSiblingPath(const char *path, const char *suffix,
                            char *buffer, size_t bufferSize)
{
    if (!path || !suffix || !buffer || bufferSize == 0)
        return false;
    int written = snprintf(buffer, bufferSize, "%s%s", path, suffix);
    return written >= 0 && (size_t)written < bufferSize;
}

static bool fileExists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

static void discardLineRemainder(FILE *file)
{
    int value = 0;
    do
    {
        value = fgetc(file);
    } while (value != '\n' && value != EOF);
}

static bool parseBindingValue(const char *text, ActionBinding &binding)
{
    if (!text || strlen(text) >= 64)
        return false;

    char copy[64];
    snprintf(copy, sizeof(copy), "%s", text);
    char *first = trim(copy);
    char *comma = strchr(first, ',');
    char *second = NULL;
    if (comma)
    {
        *comma = '\0';
        second = trim(comma + 1);
        if (strchr(second, ','))
            return false;
    }

    first = trim(first);
    if (!*first || (second && !*second))
        return false;

    ButtonToken firstButton = BUTTON_NONE;
    ButtonToken secondButton = BUTTON_NONE;
    if (!buttonFromToken(first, firstButton))
        return false;
    if (second && !buttonFromToken(second, secondButton))
        return false;
    if (firstButton != BUTTON_NONE && firstButton == secondButton)
        return false;

    binding.button[0] = firstButton;
    binding.button[1] = secondButton;
    return true;
}

static void sanitizeParsedBindings(InputBindings &bindings,
                                   const InputBindings &defaults,
                                   bool parsed[INPUT_ACTION_COUNT])
{
    for (int pass = 0; pass < INPUT_ACTION_COUNT; ++pass)
    {
        int ownerAction[BUTTON_TOKEN_COUNT];
        bool invalidAction[INPUT_ACTION_COUNT];
        for (int button = 0; button < BUTTON_TOKEN_COUNT; ++button)
            ownerAction[button] = -1;
        for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
            invalidAction[action] = false;

        bool foundDuplicate = false;
        for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
        {
            for (int slot = 0; slot < 2; ++slot)
            {
                ButtonToken button = bindings.action[action].button[slot];
                if (button == BUTTON_NONE)
                    continue;
                if (!isBindableButton(button))
                {
                    invalidAction[action] = true;
                    foundDuplicate = true;
                    continue;
                }
                if (ownerAction[button] >= 0)
                {
                    invalidAction[action] = true;
                    invalidAction[ownerAction[button]] = true;
                    foundDuplicate = true;
                }
                else
                {
                    ownerAction[button] = action;
                }
            }
        }

        if (!foundDuplicate)
            return;

        bool reverted = false;
        for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
        {
            if (invalidAction[action] && parsed[action])
            {
                bindings.action[action] = defaults.action[action];
                parsed[action] = false;
                reverted = true;
            }
        }
        if (!reverted)
            break;
    }

    if (!bindingsAreValid(bindings))
        bindings = defaults;
}

static bool parseSettingsFile(const char *path, ClientSettings &settings)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

    resetClientSettings(settings);

    bool versionSeen = false;
    bool versionSupported = false;
    bool presetValid = false;
    ControlPreset parsedPreset = CONTROL_PRESET_BALANCED;
    ParsedBinding parsedBindings[INPUT_ACTION_COUNT];
    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        parsedBindings[action].valid = false;
        parsedBindings[action].value.button[0] = BUTTON_NONE;
        parsedBindings[action].value.button[1] = BUTTON_NONE;
    }

    char line[SETTINGS_LINE_SIZE];
    while (fgets(line, sizeof(line), file))
    {
        size_t length = strlen(line);
        bool complete = length > 0 && line[length - 1] == '\n';
        if (!complete && !feof(file))
        {
            discardLineRemainder(file);
            continue;
        }

        char *entry = trim(line);
        if (!*entry || *entry == '#' || *entry == ';' || *entry == '[')
            continue;

        char *equals = strchr(entry, '=');
        if (!equals)
            continue;
        *equals = '\0';
        char *key = trim(entry);
        char *value = trim(equals + 1);

        if (asciiEqualIgnoreCase(key, "version"))
        {
            long version = 0;
            versionSeen = true;
            versionSupported = parseInteger(value, 1, 0x7fffffffL, version) &&
                               version == (long)CLIENT_SETTINGS_VERSION;
        }
        else if (asciiEqualIgnoreCase(key, "preset"))
        {
            presetValid = controlPresetFromToken(value, parsedPreset);
        }
        else if (strlen(key) > 5 &&
                 (key[0] == 'b' || key[0] == 'B') &&
                 (key[1] == 'i' || key[1] == 'I') &&
                 (key[2] == 'n' || key[2] == 'N') &&
                 (key[3] == 'd' || key[3] == 'D') &&
                 key[4] == '.')
        {
            InputAction action = INPUT_ACTION_TOOLS;
            if (inputActionFromToken(key + 5, action))
                parsedBindings[action].valid =
                    parseBindingValue(value, parsedBindings[action].value);
        }
        else if (asciiEqualIgnoreCase(key, "zoom_side"))
        {
            ZoomOverlaySide side = ZOOM_OVERLAY_AUTO;
            if (zoomOverlaySideFromToken(value, side))
                settings.zoomOverlaySide = side;
        }
        else if (asciiEqualIgnoreCase(key, "last_channel"))
        {
            if (!*value)
                settings.lastSuccessfulChannel[0] = '\0';
            else if (isChannelValid(value))
                snprintf(settings.lastSuccessfulChannel,
                         sizeof(settings.lastSuccessfulChannel), "%s", value);
        }
        else if (asciiEqualIgnoreCase(key, "brush_shape"))
        {
            ClientBrushShape shape = CLIENT_BRUSH_CIRCLE;
            if (clientBrushShapeFromToken(value, shape))
                settings.brushShape = shape;
        }
        else if (asciiEqualIgnoreCase(key, "brush_size"))
        {
            long size = 0;
            if (parseInteger(value, 1, 12, size))
                settings.brushSize = (int)size;
        }
        else if (asciiEqualIgnoreCase(key, "solid_color"))
        {
            Rgb8 color;
            if (parseRgbHex(value, color))
                settings.solidColor = color;
        }
        else if (strlen(key) > 8 &&
                 (key[0] == 'p' || key[0] == 'P') &&
                 (key[1] == 'a' || key[1] == 'A') &&
                 (key[2] == 'l' || key[2] == 'L') &&
                 (key[3] == 'e' || key[3] == 'E') &&
                 (key[4] == 't' || key[4] == 'T') &&
                 (key[5] == 't' || key[5] == 'T') &&
                 (key[6] == 'e' || key[6] == 'E') &&
                 key[7] == '.')
        {
            long slot = 0;
            Rgb8 color;
            if (parseInteger(key + 8, 1, CLIENT_PALETTE_COLOR_COUNT, slot) &&
                parseRgbHex(value, color))
                settings.palette[slot - 1] = color;
        }
    }

    bool readOk = ferror(file) == 0;
    fclose(file);
    if (!readOk || !versionSeen || !versionSupported)
        return false;

    settings.version = CLIENT_SETTINGS_VERSION;
    settings.controlPreset = presetValid ? parsedPreset : CONTROL_PRESET_BALANCED;

    InputBindings defaults;
    setPresetBindings(settings.controlPreset, defaults);
    settings.bindings = defaults;
    bool accepted[INPUT_ACTION_COUNT];
    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        accepted[action] = parsedBindings[action].valid;
        if (accepted[action])
            settings.bindings.action[action] = parsedBindings[action].value;
    }
    sanitizeParsedBindings(settings.bindings, defaults, accepted);

    if (settings.controlPreset != CONTROL_PRESET_CUSTOM)
    {
        InputBindings namedPreset;
        setPresetBindings(settings.controlPreset, namedPreset);
        if (!bindingsEqual(settings.bindings, namedPreset))
            settings.controlPreset = CONTROL_PRESET_CUSTOM;
    }
    return true;
}

static bool writeSettingsFile(const char *path, const ClientSettings &settings)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;

    bool ok = true;
    ok = ok && fprintf(file, "# Collab Doodle client settings\n") >= 0;
    ok = ok && fprintf(file, "version=%u\n", CLIENT_SETTINGS_VERSION) >= 0;
    ok = ok && fprintf(file, "preset=%s\n",
                       controlPresetToken(settings.controlPreset)) >= 0;
    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        ok = ok && fprintf(file, "bind.%s=%s,%s\n",
                           inputActionToken((InputAction)action),
                           buttonTokenName(settings.bindings.action[action].button[0]),
                           buttonTokenName(settings.bindings.action[action].button[1])) >= 0;
    }
    ok = ok && fprintf(file, "zoom_side=%s\n",
                       zoomOverlaySideToken(settings.zoomOverlaySide)) >= 0;
    ok = ok && fprintf(file, "last_channel=%s\n",
                       settings.lastSuccessfulChannel) >= 0;
    ok = ok && fprintf(file, "brush_shape=%s\n",
                       clientBrushShapeToken(settings.brushShape)) >= 0;
    ok = ok && fprintf(file, "brush_size=%d\n", settings.brushSize) >= 0;
    ok = ok && fprintf(file, "solid_color=#%02X%02X%02X\n",
                       settings.solidColor.r, settings.solidColor.g,
                       settings.solidColor.b) >= 0;
    for (int slot = 0; slot < CLIENT_PALETTE_COLOR_COUNT; ++slot)
    {
        ok = ok && fprintf(file, "palette.%d=#%02X%02X%02X\n", slot + 1,
                           settings.palette[slot].r, settings.palette[slot].g,
                           settings.palette[slot].b) >= 0;
    }

    if (fflush(file) != 0 || ferror(file) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

} // namespace

const char *clientSettingsPath()
{
    return SETTINGS_PATH;
}

const char *settingsLoadResultLabel(SettingsLoadResult result)
{
    if (result == SETTINGS_LOAD_PRIMARY)
        return "Loaded";
    if (result == SETTINGS_LOAD_BACKUP)
        return "Recovered backup";
    return "Using defaults";
}

const char *settingsSaveResultLabel(SettingsSaveResult result)
{
    switch (result)
    {
    case SETTINGS_SAVE_OK:
        return "Saved";
    case SETTINGS_SAVE_INVALID:
        return "Invalid settings";
    case SETTINGS_SAVE_PATH_TOO_LONG:
        return "Settings path too long";
    case SETTINGS_SAVE_OPEN_FAILED:
        return "Could not open temporary settings";
    case SETTINGS_SAVE_WRITE_FAILED:
        return "Could not write temporary settings";
    case SETTINGS_SAVE_BACKUP_FAILED:
        return "Could not back up existing settings";
    case SETTINGS_SAVE_COMMIT_FAILED:
        return "Could not commit settings";
    default:
        return "Settings error";
    }
}

const char *clientBrushShapeToken(ClientBrushShape shape)
{
    static const char *TOKENS[CLIENT_BRUSH_SHAPE_COUNT] = {
        "circle", "square", "dither", "eraser"};
    return shape >= CLIENT_BRUSH_CIRCLE && shape < CLIENT_BRUSH_SHAPE_COUNT
               ? TOKENS[shape]
               : "invalid";
}

const char *clientBrushShapeLabel(ClientBrushShape shape)
{
    static const char *LABELS[CLIENT_BRUSH_SHAPE_COUNT] = {
        "Circle", "Square", "Dither", "Eraser"};
    return shape >= CLIENT_BRUSH_CIRCLE && shape < CLIENT_BRUSH_SHAPE_COUNT
               ? LABELS[shape]
               : "Invalid";
}

bool clientBrushShapeFromToken(const char *text, ClientBrushShape &shape)
{
    for (int i = 0; i < CLIENT_BRUSH_SHAPE_COUNT; ++i)
    {
        if (asciiEqualIgnoreCase(text, clientBrushShapeToken((ClientBrushShape)i)))
        {
            shape = (ClientBrushShape)i;
            return true;
        }
    }
    return false;
}

const char *zoomOverlaySideToken(ZoomOverlaySide side)
{
    static const char *TOKENS[ZOOM_OVERLAY_SIDE_COUNT] = {
        "auto", "left", "right"};
    return side >= ZOOM_OVERLAY_AUTO && side < ZOOM_OVERLAY_SIDE_COUNT
               ? TOKENS[side]
               : "invalid";
}

const char *zoomOverlaySideLabel(ZoomOverlaySide side)
{
    static const char *LABELS[ZOOM_OVERLAY_SIDE_COUNT] = {
        "Auto", "Left", "Right"};
    return side >= ZOOM_OVERLAY_AUTO && side < ZOOM_OVERLAY_SIDE_COUNT
               ? LABELS[side]
               : "Invalid";
}

bool zoomOverlaySideFromToken(const char *text, ZoomOverlaySide &side)
{
    for (int i = 0; i < ZOOM_OVERLAY_SIDE_COUNT; ++i)
    {
        if (asciiEqualIgnoreCase(text, zoomOverlaySideToken((ZoomOverlaySide)i)))
        {
            side = (ZoomOverlaySide)i;
            return true;
        }
    }
    return false;
}

bool parseRgbHex(const char *text, Rgb8 &color)
{
    if (!text)
        return false;
    if (*text == '#')
        ++text;
    if (strlen(text) != 6)
        return false;

    int values[6];
    for (int i = 0; i < 6; ++i)
    {
        values[i] = hexValue(text[i]);
        if (values[i] < 0)
            return false;
    }
    color.r = (u8)((values[0] << 4) | values[1]);
    color.g = (u8)((values[2] << 4) | values[3]);
    color.b = (u8)((values[4] << 4) | values[5]);
    return true;
}

bool formatRgbHex(Rgb8 color, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize < 8)
        return false;
    int written = snprintf(buffer, bufferSize, "#%02X%02X%02X",
                           color.r, color.g, color.b);
    return written == 7;
}

void resetClientPalette(ClientSettings &settings)
{
    for (int i = 0; i < CLIENT_PALETTE_COLOR_COUNT; ++i)
        settings.palette[i] = DEFAULT_PALETTE[i];
}

void resetClientSettings(ClientSettings &settings)
{
    settings.version = CLIENT_SETTINGS_VERSION;
    settings.controlPreset = CONTROL_PRESET_BALANCED;
    setPresetBindings(settings.controlPreset, settings.bindings);
    settings.zoomOverlaySide = ZOOM_OVERLAY_AUTO;
    settings.lastSuccessfulChannel[0] = '\0';
    settings.brushShape = CLIENT_BRUSH_CIRCLE;
    settings.brushSize = 1;
    settings.solidColor.r = 255;
    settings.solidColor.g = 0;
    settings.solidColor.b = 0;
    resetClientPalette(settings);
}

void applyClientControlPreset(ClientSettings &settings, ControlPreset preset)
{
    if (preset < CONTROL_PRESET_BALANCED || preset >= CONTROL_PRESET_COUNT)
        return;
    if (preset != CONTROL_PRESET_CUSTOM)
        setPresetBindings(preset, settings.bindings);
    settings.controlPreset = preset;
}

BindingEditResult editClientBinding(ClientSettings &settings, InputAction action,
                                    int slot, ButtonToken button,
                                    BindingConflictPolicy policy,
                                    BindingConflict *conflict)
{
    BindingEditResult result =
        assignBinding(settings.bindings, action, slot, button, policy, conflict);
    if (result == BINDING_EDIT_OK || result == BINDING_EDIT_SWAPPED)
        settings.controlPreset = CONTROL_PRESET_CUSTOM;
    return result;
}

bool setLastSuccessfulChannel(ClientSettings &settings, const char *channel)
{
    if (!channel)
        return false;
    if (!*channel)
    {
        settings.lastSuccessfulChannel[0] = '\0';
        return true;
    }
    if (!isChannelValid(channel))
        return false;
    snprintf(settings.lastSuccessfulChannel,
             sizeof(settings.lastSuccessfulChannel), "%s", channel);
    return true;
}

bool clientSettingsAreValid(const ClientSettings &settings)
{
    if (settings.version != CLIENT_SETTINGS_VERSION)
        return false;
    if (settings.controlPreset < CONTROL_PRESET_BALANCED ||
        settings.controlPreset >= CONTROL_PRESET_COUNT)
        return false;
    if (!bindingsAreValid(settings.bindings))
        return false;
    if (settings.controlPreset != CONTROL_PRESET_CUSTOM)
    {
        InputBindings presetBindings;
        setPresetBindings(settings.controlPreset, presetBindings);
        if (!bindingsEqual(settings.bindings, presetBindings))
            return false;
    }
    if (settings.zoomOverlaySide < ZOOM_OVERLAY_AUTO ||
        settings.zoomOverlaySide >= ZOOM_OVERLAY_SIDE_COUNT)
        return false;
    if (!isChannelValid(settings.lastSuccessfulChannel))
        return false;
    if (settings.brushShape < CLIENT_BRUSH_CIRCLE ||
        settings.brushShape >= CLIENT_BRUSH_SHAPE_COUNT)
        return false;
    if (settings.brushSize < 1 || settings.brushSize > 12)
        return false;
    return true;
}

bool clientSettingsEqual(const ClientSettings &left, const ClientSettings &right)
{
    if (left.version != right.version ||
        left.controlPreset != right.controlPreset ||
        !bindingsEqual(left.bindings, right.bindings) ||
        left.zoomOverlaySide != right.zoomOverlaySide ||
        strcmp(left.lastSuccessfulChannel, right.lastSuccessfulChannel) != 0 ||
        left.brushShape != right.brushShape ||
        left.brushSize != right.brushSize ||
        !rgbEqual(left.solidColor, right.solidColor))
        return false;

    for (int i = 0; i < CLIENT_PALETTE_COLOR_COUNT; ++i)
    {
        if (!rgbEqual(left.palette[i], right.palette[i]))
            return false;
    }
    return true;
}

SettingsLoadResult loadClientSettings(ClientSettings &settings, const char *path)
{
    const char *resolvedPath = path ? path : SETTINGS_PATH;
    ClientSettings loaded;
    if (parseSettingsFile(resolvedPath, loaded))
    {
        settings = loaded;
        return SETTINGS_LOAD_PRIMARY;
    }

    char backupPath[SETTINGS_PATH_BUFFER_SIZE];
    if (makeSiblingPath(resolvedPath, ".bak", backupPath, sizeof(backupPath)) &&
        parseSettingsFile(backupPath, loaded))
    {
        settings = loaded;
        return SETTINGS_LOAD_BACKUP;
    }

    resetClientSettings(settings);
    return SETTINGS_LOAD_DEFAULTS;
}

SettingsSaveResult saveClientSettings(const ClientSettings &settings,
                                      const char *path)
{
    if (!clientSettingsAreValid(settings))
        return SETTINGS_SAVE_INVALID;

    const char *resolvedPath = path ? path : SETTINGS_PATH;
    char temporaryPath[SETTINGS_PATH_BUFFER_SIZE];
    char backupPath[SETTINGS_PATH_BUFFER_SIZE];
    if (!makeSiblingPath(resolvedPath, ".tmp", temporaryPath,
                         sizeof(temporaryPath)) ||
        !makeSiblingPath(resolvedPath, ".bak", backupPath,
                         sizeof(backupPath)))
        return SETTINGS_SAVE_PATH_TOO_LONG;

    if (!path)
    {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/CollabDoodle", 0777);
    }

    remove(temporaryPath);
    errno = 0;
    FILE *probe = fopen(temporaryPath, "wb");
    if (!probe)
        return SETTINGS_SAVE_OPEN_FAILED;
    if (fclose(probe) != 0)
    {
        remove(temporaryPath);
        return SETTINGS_SAVE_OPEN_FAILED;
    }

    if (!writeSettingsFile(temporaryPath, settings))
    {
        remove(temporaryPath);
        return SETTINGS_SAVE_WRITE_FAILED;
    }

    bool hadPrimary = fileExists(resolvedPath);
    bool validPrimary = false;
    if (hadPrimary)
    {
        ClientSettings previous;
        validPrimary = parseSettingsFile(resolvedPath, previous);
    }

    if (validPrimary)
    {
        remove(backupPath);
        if (rename(resolvedPath, backupPath) != 0)
        {
            remove(temporaryPath);
            return SETTINGS_SAVE_BACKUP_FAILED;
        }
    }
    else if (hadPrimary && remove(resolvedPath) != 0)
    {
        remove(temporaryPath);
        return SETTINGS_SAVE_BACKUP_FAILED;
    }

    if (rename(temporaryPath, resolvedPath) == 0)
        return SETTINGS_SAVE_OK;

    // Best-effort rollback. Keep the backup intact when the primary was
    // already corrupt, so the next load can still recover from it.
    if (validPrimary)
        rename(backupPath, resolvedPath);
    remove(temporaryPath);
    return SETTINGS_SAVE_COMMIT_FAILED;
}

} // namespace Doodle

#include "client_settings.h"
#include "brush_render.h"
#include "canvas_sync.h"
#include "input_bindings.h"
#include "protocol.h"
#include "scoped_notice.h"
#include "timestamp_format.h"
#include "ticket_flow.h"
#include "ui_canvas.h"
#include "ui_route.h"

#include <stdio.h>
#include <string.h>

#include <vector>

using namespace Doodle;

namespace
{

int failures = 0;

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

void check(bool condition, const char *expression, const char *file, int line)
{
    if (condition)
        return;
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    ++failures;
}

bool sameColor(Rgb8 left, Rgb8 right)
{
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

bool fileExists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

bool writeText(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    const size_t length = strlen(text);
    const bool wroteAll = fwrite(text, 1, length, file) == length;
    return fclose(file) == 0 && wroteAll;
}

void removeSettingsFixture(const char *path)
{
    char sibling[320];
    remove(path);
    snprintf(sibling, sizeof(sibling), "%s.tmp", path);
    remove(sibling);
    snprintf(sibling, sizeof(sibling), "%s.bak", path);
    remove(sibling);
}

void testPresetDefaults()
{
    InputBindings bindings;

    setPresetBindings(CONTROL_PRESET_BALANCED, bindings);
    CHECK(bindingsAreValid(bindings));
    CHECK(identifyPreset(bindings) == CONTROL_PRESET_BALANCED);
    CHECK(bindings.action[INPUT_ACTION_TOOLS].button[0] == BUTTON_DPAD_DOWN);
    CHECK(bindings.action[INPUT_ACTION_TOOLS].button[1] == BUTTON_B);
    CHECK(bindings.action[INPUT_ACTION_PAN].button[0] == BUTTON_DPAD_LEFT);
    CHECK(bindings.action[INPUT_ACTION_PAN].button[1] == BUTTON_A);
    CHECK(bindings.action[INPUT_ACTION_SAMPLE].button[0] == BUTTON_DPAD_UP);
    CHECK(bindings.action[INPUT_ACTION_SAMPLE].button[1] == BUTTON_X);
    CHECK(bindings.action[INPUT_ACTION_ZOOM].button[0] == BUTTON_DPAD_RIGHT);
    CHECK(bindings.action[INPUT_ACTION_ZOOM].button[1] == BUTTON_Y);
    CHECK(bindings.action[INPUT_ACTION_QUICK_ERASER].button[0] == BUTTON_L);
    CHECK(bindings.action[INPUT_ACTION_QUICK_ERASER].button[1] == BUTTON_R);
    CHECK(bindings.action[INPUT_ACTION_REFRESH].button[0] == BUTTON_START);
    CHECK(bindings.action[INPUT_ACTION_REFRESH].button[1] == BUTTON_NONE);

    setPresetBindings(CONTROL_PRESET_RIGHT_HANDED_STYLUS, bindings);
    CHECK(bindingsAreValid(bindings));
    CHECK(identifyPreset(bindings) == CONTROL_PRESET_RIGHT_HANDED_STYLUS);
    CHECK(bindings.action[INPUT_ACTION_TOOLS].button[0] == BUTTON_DPAD_DOWN);
    CHECK(bindings.action[INPUT_ACTION_PAN].button[0] == BUTTON_DPAD_LEFT);
    CHECK(bindings.action[INPUT_ACTION_SAMPLE].button[0] == BUTTON_DPAD_UP);
    CHECK(bindings.action[INPUT_ACTION_ZOOM].button[0] == BUTTON_DPAD_RIGHT);
    CHECK(bindings.action[INPUT_ACTION_QUICK_ERASER].button[0] == BUTTON_L);

    setPresetBindings(CONTROL_PRESET_LEFT_HANDED_STYLUS, bindings);
    CHECK(bindingsAreValid(bindings));
    CHECK(identifyPreset(bindings) == CONTROL_PRESET_LEFT_HANDED_STYLUS);
    CHECK(bindings.action[INPUT_ACTION_TOOLS].button[0] == BUTTON_B);
    CHECK(bindings.action[INPUT_ACTION_PAN].button[0] == BUTTON_A);
    CHECK(bindings.action[INPUT_ACTION_SAMPLE].button[0] == BUTTON_X);
    CHECK(bindings.action[INPUT_ACTION_ZOOM].button[0] == BUTTON_Y);
    CHECK(bindings.action[INPUT_ACTION_QUICK_ERASER].button[0] == BUTTON_R);
}

void testBindingCollisionsAndSwap()
{
    InputBindings bindings;
    setPresetBindings(CONTROL_PRESET_BALANCED, bindings);

    BindingConflict conflict;
    BindingEditResult result =
        assignBinding(bindings, INPUT_ACTION_TOOLS, 0, BUTTON_DPAD_LEFT,
                      BINDING_CONFLICT_CANCEL, &conflict);
    CHECK(result == BINDING_EDIT_CONFLICT);
    CHECK(conflict.found);
    CHECK(conflict.action == INPUT_ACTION_PAN);
    CHECK(conflict.slot == 0);
    CHECK(bindings.action[INPUT_ACTION_TOOLS].button[0] == BUTTON_DPAD_DOWN);
    CHECK(bindings.action[INPUT_ACTION_PAN].button[0] == BUTTON_DPAD_LEFT);

    result = assignBinding(bindings, INPUT_ACTION_TOOLS, 0, BUTTON_DPAD_LEFT,
                           BINDING_CONFLICT_SWAP, &conflict);
    CHECK(result == BINDING_EDIT_SWAPPED);
    CHECK(bindings.action[INPUT_ACTION_TOOLS].button[0] == BUTTON_DPAD_LEFT);
    CHECK(bindings.action[INPUT_ACTION_PAN].button[0] == BUTTON_DPAD_DOWN);
    CHECK(bindingsAreValid(bindings));
    CHECK(identifyPreset(bindings) == CONTROL_PRESET_CUSTOM);

    ClientSettings settings;
    resetClientSettings(settings);
    result = editClientBinding(settings, INPUT_ACTION_TOOLS, 0, BUTTON_DPAD_LEFT,
                               BINDING_CONFLICT_SWAP, &conflict);
    CHECK(result == BINDING_EDIT_SWAPPED);
    CHECK(settings.controlPreset == CONTROL_PRESET_CUSTOM);

    ButtonToken token = BUTTON_NONE;
    CHECK(!buttonFromKeyMask(KEY_SELECT, token));
    CHECK(assignBinding(bindings, INPUT_ACTION_TOOLS, 0,
                        (ButtonToken)BUTTON_TOKEN_COUNT,
                        BINDING_CONFLICT_CANCEL, NULL) == BINDING_EDIT_INVALID);
}

void testSemanticConsumption()
{
    InputBindings bindings;
    setPresetBindings(CONTROL_PRESET_BALANCED, bindings);

    SemanticInputFrame frame(KEY_B | KEY_START, KEY_L | KEY_DRIGHT, KEY_B);
    CHECK(frame.isDown(bindings, INPUT_ACTION_TOOLS));
    CHECK(frame.consumeDown(bindings, INPUT_ACTION_TOOLS));
    CHECK(!frame.isDown(bindings, INPUT_ACTION_TOOLS));
    CHECK(!frame.consumeDown(bindings, INPUT_ACTION_TOOLS));
    CHECK(frame.isDown(bindings, INPUT_ACTION_REFRESH));
    CHECK(frame.remainingDown() == KEY_START);

    CHECK(frame.consumeHeld(bindings, INPUT_ACTION_QUICK_ERASER));
    CHECK(frame.consumeHeld(bindings, INPUT_ACTION_ZOOM));
    CHECK(frame.remainingHeld() == 0);

    // Consumption is phase-specific: a down event does not consume release.
    CHECK(frame.isUp(bindings, INPUT_ACTION_TOOLS));
    CHECK(frame.consume(bindings, INPUT_ACTION_TOOLS, INPUT_PHASE_UP));
    CHECK(!frame.consumeUp(bindings, INPUT_ACTION_TOOLS));
}

void testSettingsRoundTripAndRecovery()
{
    const char *path = "build/host-tests/settings-fixture.ini";
    removeSettingsFixture(path);

    ClientSettings first;
    resetClientSettings(first);
    CHECK(setLastSuccessfulChannel(first, "sketch"));
    first.zoomOverlaySide = ZOOM_OVERLAY_RIGHT;
    first.brushShape = CLIENT_BRUSH_SPRAY;
    first.brushSizeTenths = 75;
    first.brushFeatherEnabled = false;
    first.solidColor.r = 0x12;
    first.solidColor.g = 0x34;
    first.solidColor.b = 0x56;
    first.palette[3].r = 0x9A;
    first.palette[3].g = 0xBC;
    first.palette[3].b = 0xDE;
    applyClientControlPreset(first, CONTROL_PRESET_LEFT_HANDED_STYLUS);

    CHECK(saveClientSettings(first, path) == SETTINGS_SAVE_OK);
    CHECK(fileExists(path));
    CHECK(!fileExists("build/host-tests/settings-fixture.ini.tmp"));

    ClientSettings loaded;
    CHECK(loadClientSettings(loaded, path) == SETTINGS_LOAD_PRIMARY);
    CHECK(clientSettingsEqual(first, loaded));

    ClientSettings second = first;
    second.palette[0].r = 7;
    second.palette[0].g = 8;
    second.palette[0].b = 9;
    second.brushSizeTenths = 120;
    CHECK(saveClientSettings(second, path) == SETTINGS_SAVE_OK);
    CHECK(fileExists("build/host-tests/settings-fixture.ini.bak"));

    // The second atomic save kept the last known-good primary as .bak.
    CHECK(writeText(path, "version=this-is-corrupt\n"));
    CHECK(loadClientSettings(loaded, path) == SETTINGS_LOAD_BACKUP);
    CHECK(clientSettingsEqual(first, loaded));

    CHECK(writeText("build/host-tests/settings-fixture.ini.bak",
                    "version=999\n"));
    CHECK(loadClientSettings(loaded, path) == SETTINGS_LOAD_DEFAULTS);
    ClientSettings defaults;
    resetClientSettings(defaults);
    CHECK(clientSettingsEqual(defaults, loaded));

    removeSettingsFixture(path);
}

void testSettingsIndependentValidation()
{
    const char *path = "build/host-tests/settings-fixture.ini";
    removeSettingsFixture(path);
    const char *fixture =
        "# valid and invalid fields are intentionally mixed\n"
        "version=1\n"
        "preset=right_handed_stylus\n"
        "zoom_side=left\n"
        "last_channel=contains a space\n"
        "brush_shape=dither\n"
        "brush_size=4.5\n"
        "brush_feather=off\n"
        "solid_color=#12GG56\n"
        "palette.1=#010203\n"
        "palette.2=not-a-color\n"
        "future_option=is ignored\n";
    CHECK(writeText(path, fixture));

    ClientSettings settings;
    CHECK(loadClientSettings(settings, path) == SETTINGS_LOAD_PRIMARY);
    CHECK(settings.controlPreset == CONTROL_PRESET_RIGHT_HANDED_STYLUS);
    CHECK(settings.zoomOverlaySide == ZOOM_OVERLAY_LEFT);
    CHECK(settings.lastSuccessfulChannel[0] == '\0');
    CHECK(settings.brushShape == CLIENT_BRUSH_DITHER);
    CHECK(settings.brushSizeTenths == 45);
    CHECK(!settings.brushFeatherEnabled);
    CHECK(sameColor(settings.solidColor, Rgb8{255, 0, 0}));
    CHECK(sameColor(settings.palette[0], Rgb8{1, 2, 3}));
    CHECK(sameColor(settings.palette[1], Rgb8{255, 255, 255}));

    // Invalid data is rejected before touching the valid primary.
    ClientSettings invalid = settings;
    invalid.brushSizeTenths = 0;
    CHECK(saveClientSettings(invalid, path) == SETTINGS_SAVE_INVALID);
    ClientSettings reloaded;
    CHECK(loadClientSettings(reloaded, path) == SETTINGS_LOAD_PRIMARY);
    CHECK(clientSettingsEqual(settings, reloaded));

    ClientSettings valid;
    resetClientSettings(valid);
    const char *blockedComponent =
        "build/host-tests/settings-path-component-is-a-file";
    remove(blockedComponent);
    CHECK(writeText(blockedComponent, "not a directory"));
    CHECK(saveClientSettings(valid,
                             "build/host-tests/settings-path-component-is-a-file/settings.ini") ==
          SETTINGS_SAVE_OPEN_FAILED);
    remove(blockedComponent);

    removeSettingsFixture(path);
}

void testPaletteDefaultsAndHex()
{
    ClientSettings settings;
    resetClientSettings(settings);
    const Rgb8 expected[CLIENT_PALETTE_COLOR_COUNT] = {
        {0, 0, 0},       {255, 255, 255}, {255, 0, 0},   {255, 255, 0},
        {0, 255, 0},     {0, 255, 255},   {0, 0, 255},   {255, 0, 255}};
    for (int i = 0; i < CLIENT_PALETTE_COLOR_COUNT; ++i)
        CHECK(sameColor(settings.palette[i], expected[i]));

    Rgb8 color = {0, 0, 0};
    CHECK(parseRgbHex("#a1B2c3", color));
    CHECK(sameColor(color, Rgb8{0xA1, 0xB2, 0xC3}));
    CHECK(!parseRgbHex("#12345", color));
    CHECK(!parseRgbHex("12345Z", color));
    char text[8];
    CHECK(formatRgbHex(color, text, sizeof(text)));
    CHECK(strcmp(text, "#A1B2C3") == 0);
}

void testUiRectAndClipping()
{
    const UiRect rect(10, 20, 30, 40);
    CHECK(rect.contains(10, 20));
    CHECK(rect.contains(39, 59));
    CHECK(!rect.contains(9, 20));
    CHECK(!rect.contains(40, 20));
    CHECK(!rect.contains(10, 60));

    CHECK(UiGeometry::normalizedPositionClamped(4, 5, 15) == 0.0f);
    CHECK(UiGeometry::normalizedPositionClamped(5, 5, 15) == 0.0f);
    CHECK(UiGeometry::normalizedPositionClamped(10, 5, 15) == 0.5f);
    CHECK(UiGeometry::normalizedPositionClamped(15, 5, 15) == 1.0f);
    CHECK(UiGeometry::normalizedPositionClamped(16, 5, 15) == 1.0f);
    CHECK(UiGeometry::normalizedPositionClamped(10, 15, 5) == 0.0f);

    CHECK(UiCanvas::textWidth("ABCD") == 24);
    CHECK(UiCanvas::fitTextScale("Connection", 260, 2) == 2);
    CHECK(UiCanvas::fitTextScale("Reset favorite colors?", 260, 2) == 1);
    CHECK(UiCanvas::fitTextScale("Any title", 0, 2) == 1);
    std::vector<u8> clipped(80 * 12 * 3, 0);
    std::vector<u8> expected(80 * 12 * 3, 0);
    UiCanvas clippedCanvas(&clipped[0], 80, 12, UI_BUFFER_RGB);
    UiCanvas expectedCanvas(&expected[0], 80, 12, UI_BUFFER_RGB);
    clippedCanvas.textClipped(0, 0, "ABCDE", UiColor(8, 16, 24), 24, 1, true);
    expectedCanvas.text(0, 0, "A...", UiColor(8, 16, 24));
    CHECK(clipped == expected);

    std::vector<u8> wrapped(120 * 40 * 3, 0);
    UiCanvas wrappedCanvas(&wrapped[0], 120, 40, UI_BUFFER_RGB);
    CHECK(wrappedCanvas.wrappedText(0, 0, "first\nsecond",
                                    UiColor(8, 16, 24), 120, 4) == 2);
    CHECK(wrappedCanvas.wrappedText(0, 0,
                                    "one two three four five six",
                                    UiColor(8, 16, 24), 36, 2) == 2);

    std::vector<u8> rotated(4 * 6 * 3, 0);
    UiCanvas rotatedCanvas(&rotated[0], 4, 6, UI_BUFFER_3DS_ROTATED_BGR);
    CHECK(rotatedCanvas.logicalWidth() == 6);
    CHECK(rotatedCanvas.logicalHeight() == 4);
    rotatedCanvas.pixel(2, 1, UiColor(1, 2, 3));
    const int mappedIndex = 3 * (2 * 4 + 2);
    CHECK(rotated[mappedIndex] == 3);
    CHECK(rotated[mappedIndex + 1] == 2);
    CHECK(rotated[mappedIndex + 2] == 1);
}

void testRouteStack()
{
    UiRouteStack routes;
    CHECK(routes.current() == TOP_MODE_CANVAS);
    CHECK(routes.depth() == 1);

    routes.reset(TOP_MODE_MENU);
    routes.state().focus = 2;
    routes.state().scroll = 1;
    CHECK(routes.push(TOP_MODE_TICKETS));
    routes.state().focus = 3;
    CHECK(routes.current() == TOP_MODE_TICKETS);
    CHECK(routes.depth() == 2);
    CHECK(routes.pop());
    CHECK(routes.current() == TOP_MODE_MENU);
    CHECK(routes.state().focus == 2);
    CHECK(routes.state().scroll == 1);
    CHECK(!routes.pop());

    routes.showOverlay(UI_OVERLAY_CONFIRMATION);
    CHECK(routes.overlay() == UI_OVERLAY_CONFIRMATION);
    routes.clearOverlay();
    CHECK(routes.overlay() == UI_OVERLAY_NONE);

    for (int index = 1; index < UiRouteStack::CAPACITY; ++index)
        CHECK(routes.push((index & 1) ? TOP_MODE_USERS : TOP_MODE_OPTIONS));
    CHECK(!routes.push(TOP_MODE_CHANNELS));
}

void testProtocolAdditionsAndEscaping()
{
    char command[1024];
    Protocol::buildHello(
        command, sizeof(command), "collab-doodle", "1.6.0", true,
        "device", "secret", "hardware", "new-3ds-xl", "Doodler", "3dsx");
    CHECK(strstr(command, "\"protocol\":6") != NULL);
    CHECK(strstr(command, "\"draw-size-tenths\"") != NULL);
    CHECK(strstr(command, "\"brush-feather\"") != NULL);
    CHECK(strstr(command, "\"brush-suite-v2\"") != NULL);

    Protocol::buildTicketCreate(
        command, sizeof(command), "report", "A \"quoted\" title",
        "line 1\nline 2 \\ path");
    CHECK(strstr(command, "\"category\":\"report\"") != NULL);
    CHECK(strstr(command, "A \\\"quoted\\\" title") != NULL);
    CHECK(strstr(command, "line 1\\nline 2 \\\\ path") != NULL);

    char channels[8][25];
    ChannelInfo metadata[8];
    int count = 0;
    char current[25];
    CHECK(Protocol::parseChannels(
        "{\"type\":\"channels\",\"channels\":[\"main\",\"art\"],"
        "\"currentChannel\":\"main\"}",
        channels, metadata, 8, count, current));
    CHECK(count == 2);
    CHECK(strcmp(current, "main") == 0);
    CHECK(metadata[0].name[0] == '\0');
    CHECK(metadata[1].name[0] == '\0');

    CHECK(Protocol::parseChannels(
        "{\"type\":\"channels\",\"channels\":[\"main\",\"art\"],"
        "\"channelInfo\":[{\"name\":\"main\",\"userCount\":3,"
        "\"width\":1920,\"height\":1080,"
        "\"staffOnly\":false,\"adminOnly\":false,\"readOnly\":false}],"
        "\"currentChannel\":\"main\"}",
        channels, metadata, 8, count, current));
    CHECK(strcmp(metadata[0].name, "main") == 0);
    CHECK(metadata[0].userCount == 3);
    CHECK(metadata[0].width == 1920);
    CHECK(metadata[0].height == 1080);
    CHECK(metadata[1].name[0] == '\0');

    CHECK(canvasSnapshotTimeoutMs(1) == 38000);
    CHECK(canvasSnapshotTimeoutMs(1024 * 1024) == 38000);
    CHECK(canvasSnapshotTimeoutMs(6 * 1024 * 1024) == 78000);
    CHECK(canvasSnapshotTimeoutMs(10 * 1024 * 1024) == 90000);
    CHECK(isSupportedCanvasSnapshot(320, 240, 100));
    CHECK(isSupportedCanvasSnapshot(640, 480, 640 * 480 * 3));
    CHECK(isSupportedCanvasSnapshot(960, 540, 960 * 540 * 3));
    CHECK(isSupportedCanvasSnapshot(1280, 720, 1280 * 720 * 3));
    CHECK(isSupportedCanvasSnapshot(1920, 1080, 6222706));
    CHECK(!isSupportedCanvasSnapshot(1921, 1080, 100));
    CHECK(!isSupportedCanvasSnapshot(1920, 1081, 100));
    CHECK(!isSupportedCanvasSnapshot(1920, 1080, 10000001));
    CHECK(!shouldResetCanvasViewport("size-small", "size-small"));
    CHECK(shouldResetCanvasViewport("size-small", "size-large"));
    CHECK(shouldResetCanvasViewport("", "main"));
}

void testTimestampAndScopedNotices()
{
    char timestamp[24];
    CHECK(formatIsoMinuteTimestamp(
        "2026-07-24T09:23:45.123Z", timestamp, sizeof(timestamp)));
    CHECK(strcmp(timestamp, "2026-07-24 09:23Z") == 0);
    CHECK(formatIsoMinuteTimestamp(
        "2026-12-31 23:59:00.000Z", timestamp, sizeof(timestamp)));
    CHECK(strcmp(timestamp, "2026-12-31 23:59Z") == 0);
    CHECK(!formatIsoMinuteTimestamp("not-a-date", timestamp, sizeof(timestamp)));
    CHECK(strcmp(timestamp, "Unknown date") == 0);

    ScopedNotice<64> notice;
    CHECK(!notice.visible(1, 1000));
    notice.set("REFRESHING", 1, 1000, 10000, true);
    CHECK(notice.visible(1, 1000));
    CHECK(!notice.visible(2, 1000));
    CHECK(notice.pending());
    CHECK(!notice.expire(10999));
    CHECK(notice.expire(11000));
    CHECK(!notice.visible(1, 11000));
    notice.set("DONE", 2, 12000, 1800);
    CHECK(!notice.pending());
    CHECK(strcmp(notice.text(), "DONE") == 0);
    CHECK(!notice.clearPending());
    notice.set("LOADING", 2, 12000, 10000, true);
    CHECK(notice.clearPending());
    CHECK(!notice.visible(2, 12000));
    notice.set("DONE", 2, 12000, 1800);
    notice.clear();
    CHECK(!notice.visible(2, 12000));
}

void testTicketFlowHelpers()
{
    char subject[65];
    char details[241];
    buildUserReportDraft(
        subject, sizeof(subject), details, sizeof(details),
        "Reported Person", "reported_user", "identity-123",
        "size-small", "Repeatedly drawing over other people");
    CHECK(strcmp(subject, "Report: Reported Person") == 0);
    CHECK(strstr(details, "User: Reported Person (reported_user)") != NULL);
    CHECK(strstr(details, "Identity: identity-123") != NULL);
    CHECK(strstr(details, "Channel: size-small") != NULL);
    CHECK(strstr(details, "Reason: Repeatedly drawing over other people") != NULL);

    buildUserReportDraft(
        subject, sizeof(subject), details, sizeof(details),
        "", "", "", "", "");
    CHECK(strcmp(subject, "Report: Anonymous") == 0);
    CHECK(strstr(details, "Reason: No reason provided") != NULL);
}

void testBrushRenderingMath()
{
    CHECK(brushStrokeSpacing(10, CLIENT_BRUSH_CIRCLE) == 1);
    CHECK(brushStrokeSpacing(10, CLIENT_BRUSH_DITHER) == 3);
    CHECK(brushStrokeSpacing(60, CLIENT_BRUSH_DITHER) == 3);
    CHECK(brushStrokeSpacing(61, CLIENT_BRUSH_DITHER) == 4);
    CHECK(brushStrokeSpacing(120, CLIENT_BRUSH_DITHER) == 6);
    CHECK(brushStrokeSpacing(40, CLIENT_BRUSH_SPRAY) == 3);
    CHECK(brushStrokeSpacing(120, CLIENT_BRUSH_SPRAY) == 6);
    CHECK(brushStrokeSegmentSteps(8, 0, 40, CLIENT_BRUSH_CIRCLE) == 8);
    CHECK(brushStrokeSegmentSteps(8, 0, 40, CLIENT_BRUSH_DITHER) == 3);
    CHECK(brushStrokeSegmentSteps(-8, 3, 120, CLIENT_BRUSH_DITHER) == 2);
    CHECK(brushStrokeSegmentSteps(0, 0, 120, CLIENT_BRUSH_DITHER) == 1);

    CHECK(fractionalBrushCoverageTenths(
              true, true, true, 1, CLIENT_BRUSH_CIRCLE,
              4, 4, false, false) == 10);
    CHECK(fractionalBrushCoverageTenths(
              true, true, true, 1, CLIENT_BRUSH_CIRCLE,
              4, 4, true, false) == 7);
    CHECK(fractionalBrushCoverageTenths(
              true, true, true, 1, CLIENT_BRUSH_CIRCLE,
              4, 4, true, true) == 10);
    CHECK(fractionalBrushCoverageTenths(
              false, true, false, 1, CLIENT_BRUSH_CIRCLE,
              4, 4, true, false) == 1);
    CHECK(fractionalBrushCoverageTenths(
              false, true, false, 9, CLIENT_BRUSH_DITHER,
              4, 4, true, false) == 7);
    CHECK(fractionalBrushCoverageTenths(
              false, false, false, 9, CLIENT_BRUSH_CIRCLE,
              4, 4, true, false) == 0);

    CHECK(blendBrushChannel(255, 0, 1) == 230);
    CHECK(blendBrushChannel(255, 0, 5) == 128);
    CHECK(blendBrushChannel(255, 0, 9) == 26);
    CHECK(blendBrushChannel(42, 200, 10) == 200);
}

} // namespace

int main()
{
    testPresetDefaults();
    testBindingCollisionsAndSwap();
    testSemanticConsumption();
    testSettingsRoundTripAndRecovery();
    testSettingsIndependentValidation();
    testPaletteDefaultsAndHex();
    testUiRectAndClipping();
    testRouteStack();
    testProtocolAdditionsAndEscaping();
    testTimestampAndScopedNotices();
    testTicketFlowHelpers();
    testBrushRenderingMath();

    if (failures)
    {
        fprintf(stderr, "%d client fixture check(s) failed\n", failures);
        return 1;
    }
    printf("All client fixture checks passed.\n");
    return 0;
}

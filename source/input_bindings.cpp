#include "input_bindings.h"

#include <string.h>

namespace Doodle
{
namespace
{

struct ActionInfo
{
    const char *token;
    const char *label;
    bool held;
};

struct ButtonInfo
{
    const char *token;
    const char *label;
    u32 keyMask;
};

struct PresetInfo
{
    const char *token;
    const char *label;
};

static const ActionInfo ACTION_INFO[INPUT_ACTION_COUNT] = {
    {"tools", "Tools", false},
    {"pan", "Pan", true},
    {"sample", "Sample", true},
    {"zoom", "Zoom", true},
    {"quick_eraser", "Quick Eraser", true},
    {"refresh", "Refresh", false}};

static const ButtonInfo BUTTON_INFO[BUTTON_TOKEN_COUNT] = {
    {"none", "None", 0},
    {"a", "A", KEY_A},
    {"b", "B", KEY_B},
    {"x", "X", KEY_X},
    {"y", "Y", KEY_Y},
    {"l", "L", KEY_L},
    {"r", "R", KEY_R},
    {"start", "START", KEY_START},
    {"d_up", "D-Pad Up", KEY_DUP},
    {"d_down", "D-Pad Down", KEY_DDOWN},
    {"d_left", "D-Pad Left", KEY_DLEFT},
    {"d_right", "D-Pad Right", KEY_DRIGHT}};

static const PresetInfo PRESET_INFO[CONTROL_PRESET_COUNT] = {
    {"balanced", "Balanced"},
    {"right_handed_stylus", "Right-handed stylus"},
    {"left_handed_stylus", "Left-handed stylus"},
    {"custom", "Custom"}};

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

static bool validAction(InputAction action)
{
    return action >= INPUT_ACTION_TOOLS && action < INPUT_ACTION_COUNT;
}

static bool validPreset(ControlPreset preset)
{
    return preset >= CONTROL_PRESET_BALANCED && preset < CONTROL_PRESET_COUNT;
}

static void clearBindings(InputBindings &bindings)
{
    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        bindings.action[action].button[0] = BUTTON_NONE;
        bindings.action[action].button[1] = BUTTON_NONE;
    }
}

static void setPair(InputBindings &bindings, InputAction action,
                    ButtonToken first, ButtonToken second)
{
    bindings.action[action].button[0] = first;
    bindings.action[action].button[1] = second;
}

} // namespace

const char *inputActionToken(InputAction action)
{
    return validAction(action) ? ACTION_INFO[action].token : "invalid";
}

const char *inputActionLabel(InputAction action)
{
    return validAction(action) ? ACTION_INFO[action].label : "Invalid";
}

bool inputActionFromToken(const char *text, InputAction &action)
{
    for (int i = 0; i < INPUT_ACTION_COUNT; ++i)
    {
        if (asciiEqualIgnoreCase(text, ACTION_INFO[i].token))
        {
            action = (InputAction)i;
            return true;
        }
    }
    return false;
}

bool inputActionUsesHeldState(InputAction action)
{
    return validAction(action) && ACTION_INFO[action].held;
}

const char *buttonTokenName(ButtonToken button)
{
    return button >= BUTTON_NONE && button < BUTTON_TOKEN_COUNT
               ? BUTTON_INFO[button].token
               : "invalid";
}

const char *buttonLabel(ButtonToken button)
{
    return button >= BUTTON_NONE && button < BUTTON_TOKEN_COUNT
               ? BUTTON_INFO[button].label
               : "Invalid";
}

bool buttonFromToken(const char *text, ButtonToken &button)
{
    for (int i = 0; i < BUTTON_TOKEN_COUNT; ++i)
    {
        if (asciiEqualIgnoreCase(text, BUTTON_INFO[i].token))
        {
            button = (ButtonToken)i;
            return true;
        }
    }
    return false;
}

bool isBindableButton(ButtonToken button)
{
    return button >= BUTTON_A && button < BUTTON_TOKEN_COUNT;
}

u32 buttonKeyMask(ButtonToken button)
{
    return button >= BUTTON_NONE && button < BUTTON_TOKEN_COUNT
               ? BUTTON_INFO[button].keyMask
               : 0;
}

bool buttonFromKeyMask(u32 keyMask, ButtonToken &button)
{
    for (int i = BUTTON_A; i < BUTTON_TOKEN_COUNT; ++i)
    {
        if (BUTTON_INFO[i].keyMask == keyMask)
        {
            button = (ButtonToken)i;
            return true;
        }
    }
    if (keyMask == 0)
    {
        button = BUTTON_NONE;
        return true;
    }
    return false;
}

const char *controlPresetToken(ControlPreset preset)
{
    return validPreset(preset) ? PRESET_INFO[preset].token : "invalid";
}

const char *controlPresetLabel(ControlPreset preset)
{
    return validPreset(preset) ? PRESET_INFO[preset].label : "Invalid";
}

bool controlPresetFromToken(const char *text, ControlPreset &preset)
{
    for (int i = 0; i < CONTROL_PRESET_COUNT; ++i)
    {
        if (asciiEqualIgnoreCase(text, PRESET_INFO[i].token))
        {
            preset = (ControlPreset)i;
            return true;
        }
    }
    return false;
}

void setPresetBindings(ControlPreset preset, InputBindings &bindings)
{
    clearBindings(bindings);

    if (preset == CONTROL_PRESET_RIGHT_HANDED_STYLUS)
    {
        setPair(bindings, INPUT_ACTION_TOOLS, BUTTON_DPAD_DOWN, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_PAN, BUTTON_DPAD_LEFT, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_SAMPLE, BUTTON_DPAD_UP, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_ZOOM, BUTTON_DPAD_RIGHT, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_QUICK_ERASER, BUTTON_L, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_REFRESH, BUTTON_START, BUTTON_NONE);
        return;
    }

    if (preset == CONTROL_PRESET_LEFT_HANDED_STYLUS)
    {
        setPair(bindings, INPUT_ACTION_TOOLS, BUTTON_B, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_PAN, BUTTON_A, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_SAMPLE, BUTTON_X, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_ZOOM, BUTTON_Y, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_QUICK_ERASER, BUTTON_R, BUTTON_NONE);
        setPair(bindings, INPUT_ACTION_REFRESH, BUTTON_START, BUTTON_NONE);
        return;
    }

    // Custom has no canonical mapping. Falling back to Balanced gives callers
    // a safe complete set when a custom settings file omits a binding.
    setPair(bindings, INPUT_ACTION_TOOLS, BUTTON_DPAD_DOWN, BUTTON_B);
    setPair(bindings, INPUT_ACTION_PAN, BUTTON_DPAD_LEFT, BUTTON_A);
    setPair(bindings, INPUT_ACTION_SAMPLE, BUTTON_DPAD_UP, BUTTON_X);
    setPair(bindings, INPUT_ACTION_ZOOM, BUTTON_DPAD_RIGHT, BUTTON_Y);
    setPair(bindings, INPUT_ACTION_QUICK_ERASER, BUTTON_L, BUTTON_R);
    setPair(bindings, INPUT_ACTION_REFRESH, BUTTON_START, BUTTON_NONE);
}

bool bindingsEqual(const InputBindings &left, const InputBindings &right)
{
    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if (left.action[action].button[slot] != right.action[action].button[slot])
                return false;
        }
    }
    return true;
}

ControlPreset identifyPreset(const InputBindings &bindings)
{
    InputBindings candidate;
    setPresetBindings(CONTROL_PRESET_BALANCED, candidate);
    if (bindingsEqual(bindings, candidate))
        return CONTROL_PRESET_BALANCED;

    setPresetBindings(CONTROL_PRESET_RIGHT_HANDED_STYLUS, candidate);
    if (bindingsEqual(bindings, candidate))
        return CONTROL_PRESET_RIGHT_HANDED_STYLUS;

    setPresetBindings(CONTROL_PRESET_LEFT_HANDED_STYLUS, candidate);
    if (bindingsEqual(bindings, candidate))
        return CONTROL_PRESET_LEFT_HANDED_STYLUS;

    return CONTROL_PRESET_CUSTOM;
}

bool bindingsAreValid(const InputBindings &bindings)
{
    u32 usedMask = 0;
    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            ButtonToken button = bindings.action[action].button[slot];
            if (button == BUTTON_NONE)
                continue;
            if (!isBindableButton(button))
                return false;
            u32 mask = buttonKeyMask(button);
            if (!mask || (usedMask & mask) != 0)
                return false;
            usedMask |= mask;
        }
    }
    return true;
}

u32 actionKeyMask(const InputBindings &bindings, InputAction action)
{
    if (!validAction(action))
        return 0;
    return buttonKeyMask(bindings.action[action].button[0]) |
           buttonKeyMask(bindings.action[action].button[1]);
}

bool actionIsDown(const InputBindings &bindings, InputAction action, u32 downMask)
{
    return (actionKeyMask(bindings, action) & downMask) != 0;
}

bool actionIsHeld(const InputBindings &bindings, InputAction action, u32 heldMask)
{
    return (actionKeyMask(bindings, action) & heldMask) != 0;
}

bool actionIsUp(const InputBindings &bindings, InputAction action, u32 upMask)
{
    return (actionKeyMask(bindings, action) & upMask) != 0;
}

bool findBindingConflict(const InputBindings &bindings, ButtonToken button,
                         InputAction ignoreAction, int ignoreSlot,
                         BindingConflict &conflict)
{
    conflict.found = false;
    conflict.action = INPUT_ACTION_TOOLS;
    conflict.slot = -1;
    conflict.button = button;

    if (button == BUTTON_NONE)
        return false;

    for (int action = 0; action < INPUT_ACTION_COUNT; ++action)
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if (action == (int)ignoreAction && slot == ignoreSlot)
                continue;
            if (bindings.action[action].button[slot] == button)
            {
                conflict.found = true;
                conflict.action = (InputAction)action;
                conflict.slot = slot;
                return true;
            }
        }
    }
    return false;
}

BindingEditResult assignBinding(InputBindings &bindings, InputAction action,
                                int slot, ButtonToken button,
                                BindingConflictPolicy policy,
                                BindingConflict *conflict)
{
    if (conflict)
    {
        conflict->found = false;
        conflict->action = validAction(action) ? action : INPUT_ACTION_TOOLS;
        conflict->slot = -1;
        conflict->button = button;
    }
    if (!validAction(action) || slot < 0 || slot > 1 ||
        (button != BUTTON_NONE && !isBindableButton(button)) ||
        (policy != BINDING_CONFLICT_CANCEL &&
         policy != BINDING_CONFLICT_SWAP))
        return BINDING_EDIT_INVALID;

    ButtonToken oldButton = bindings.action[action].button[slot];
    if (oldButton == button)
        return BINDING_EDIT_UNCHANGED;

    BindingConflict found;
    bool hasConflict = findBindingConflict(bindings, button, action, slot, found);
    if (conflict)
        *conflict = found;

    if (hasConflict && policy == BINDING_CONFLICT_CANCEL)
        return BINDING_EDIT_CONFLICT;

    if (hasConflict)
    {
        bindings.action[found.action].button[found.slot] = oldButton;
        bindings.action[action].button[slot] = button;
        return BINDING_EDIT_SWAPPED;
    }

    bindings.action[action].button[slot] = button;
    return BINDING_EDIT_OK;
}

SemanticInputFrame::SemanticInputFrame()
    : down(0), held(0), up(0),
      consumedDown(0), consumedHeld(0), consumedUp(0)
{
}

SemanticInputFrame::SemanticInputFrame(u32 downMask, u32 heldMask, u32 upMask)
    : down(downMask), held(heldMask), up(upMask),
      consumedDown(0), consumedHeld(0), consumedUp(0)
{
}

void SemanticInputFrame::reset(u32 downMask, u32 heldMask, u32 upMask)
{
    down = downMask;
    held = heldMask;
    up = upMask;
    consumedDown = 0;
    consumedHeld = 0;
    consumedUp = 0;
}

bool SemanticInputFrame::matches(const InputBindings &bindings, InputAction action,
                                 u32 rawMask, u32 consumedMask) const
{
    return (actionKeyMask(bindings, action) & rawMask & ~consumedMask) != 0;
}

bool SemanticInputFrame::consumeMask(const InputBindings &bindings, InputAction action,
                                     u32 rawMask, u32 &consumedMask)
{
    u32 matched = actionKeyMask(bindings, action) & rawMask & ~consumedMask;
    if (!matched)
        return false;
    consumedMask |= matched;
    return true;
}

bool SemanticInputFrame::isDown(const InputBindings &bindings, InputAction action) const
{
    return matches(bindings, action, down, consumedDown);
}

bool SemanticInputFrame::isHeld(const InputBindings &bindings, InputAction action) const
{
    return matches(bindings, action, held, consumedHeld);
}

bool SemanticInputFrame::isUp(const InputBindings &bindings, InputAction action) const
{
    return matches(bindings, action, up, consumedUp);
}

bool SemanticInputFrame::consumeDown(const InputBindings &bindings, InputAction action)
{
    return consumeMask(bindings, action, down, consumedDown);
}

bool SemanticInputFrame::consumeHeld(const InputBindings &bindings, InputAction action)
{
    return consumeMask(bindings, action, held, consumedHeld);
}

bool SemanticInputFrame::consumeUp(const InputBindings &bindings, InputAction action)
{
    return consumeMask(bindings, action, up, consumedUp);
}

bool SemanticInputFrame::consume(const InputBindings &bindings, InputAction action,
                                 InputPhase phase)
{
    if (phase == INPUT_PHASE_DOWN)
        return consumeDown(bindings, action);
    if (phase == INPUT_PHASE_HELD)
        return consumeHeld(bindings, action);
    if (phase == INPUT_PHASE_UP)
        return consumeUp(bindings, action);
    return false;
}

u32 SemanticInputFrame::remainingDown() const
{
    return down & ~consumedDown;
}

u32 SemanticInputFrame::remainingHeld() const
{
    return held & ~consumedHeld;
}

u32 SemanticInputFrame::remainingUp() const
{
    return up & ~consumedUp;
}

} // namespace Doodle

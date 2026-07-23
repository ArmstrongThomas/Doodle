#ifndef DOODLE_INPUT_BINDINGS_H
#define DOODLE_INPUT_BINDINGS_H

#include <3ds.h>

namespace Doodle
{

// Canvas-only actions. Menu navigation, SELECT, touch input, and analog input
// intentionally do not pass through the rebindable action layer.
enum InputAction
{
    INPUT_ACTION_TOOLS = 0,
    INPUT_ACTION_PAN,
    INPUT_ACTION_SAMPLE,
    INPUT_ACTION_ZOOM,
    INPUT_ACTION_QUICK_ERASER,
    INPUT_ACTION_REFRESH,
    INPUT_ACTION_COUNT
};

// Keep these values stable. Settings use the string tokens exposed below,
// while the numeric values are useful for fixed-size UI lists.
enum ButtonToken
{
    BUTTON_NONE = 0,
    BUTTON_A,
    BUTTON_B,
    BUTTON_X,
    BUTTON_Y,
    BUTTON_L,
    BUTTON_R,
    BUTTON_START,
    BUTTON_DPAD_UP,
    BUTTON_DPAD_DOWN,
    BUTTON_DPAD_LEFT,
    BUTTON_DPAD_RIGHT,
    BUTTON_TOKEN_COUNT
};

enum ControlPreset
{
    CONTROL_PRESET_BALANCED = 0,
    CONTROL_PRESET_RIGHT_HANDED_STYLUS,
    CONTROL_PRESET_LEFT_HANDED_STYLUS,
    CONTROL_PRESET_CUSTOM,
    CONTROL_PRESET_COUNT
};

struct ActionBinding
{
    ButtonToken button[2];
};

struct InputBindings
{
    ActionBinding action[INPUT_ACTION_COUNT];
};

struct BindingConflict
{
    bool found;
    InputAction action;
    int slot;
    ButtonToken button;
};

enum BindingConflictPolicy
{
    BINDING_CONFLICT_CANCEL = 0,
    BINDING_CONFLICT_SWAP
};

enum BindingEditResult
{
    BINDING_EDIT_OK = 0,
    BINDING_EDIT_UNCHANGED,
    BINDING_EDIT_SWAPPED,
    BINDING_EDIT_CONFLICT,
    BINDING_EDIT_INVALID
};

enum InputPhase
{
    INPUT_PHASE_DOWN = 0,
    INPUT_PHASE_HELD,
    INPUT_PHASE_UP
};

const char *inputActionToken(InputAction action);
const char *inputActionLabel(InputAction action);
bool inputActionFromToken(const char *text, InputAction &action);
bool inputActionUsesHeldState(InputAction action);

const char *buttonTokenName(ButtonToken button);
const char *buttonLabel(ButtonToken button);
bool buttonFromToken(const char *text, ButtonToken &button);
bool isBindableButton(ButtonToken button);
u32 buttonKeyMask(ButtonToken button);
bool buttonFromKeyMask(u32 keyMask, ButtonToken &button);

const char *controlPresetToken(ControlPreset preset);
const char *controlPresetLabel(ControlPreset preset);
bool controlPresetFromToken(const char *text, ControlPreset &preset);

void setPresetBindings(ControlPreset preset, InputBindings &bindings);
ControlPreset identifyPreset(const InputBindings &bindings);
bool bindingsEqual(const InputBindings &left, const InputBindings &right);
bool bindingsAreValid(const InputBindings &bindings);

u32 actionKeyMask(const InputBindings &bindings, InputAction action);
bool actionIsDown(const InputBindings &bindings, InputAction action, u32 downMask);
bool actionIsHeld(const InputBindings &bindings, InputAction action, u32 heldMask);
bool actionIsUp(const InputBindings &bindings, InputAction action, u32 upMask);

bool findBindingConflict(const InputBindings &bindings, ButtonToken button,
                         InputAction ignoreAction, int ignoreSlot,
                         BindingConflict &conflict);

// A swap moves the target slot's old button to the conflicting slot. Assigning
// BUTTON_NONE clears a slot and never conflicts.
BindingEditResult assignBinding(InputBindings &bindings, InputAction action,
                                int slot, ButtonToken button,
                                BindingConflictPolicy policy,
                                BindingConflict *conflict);

// Per-frame helper that lets a route consume a semantic action once without
// mutating the raw HID masks. DOWN, HELD, and UP consumption are independent.
class SemanticInputFrame
{
public:
    SemanticInputFrame();
    SemanticInputFrame(u32 downMask, u32 heldMask, u32 upMask);

    void reset(u32 downMask, u32 heldMask, u32 upMask);

    bool isDown(const InputBindings &bindings, InputAction action) const;
    bool isHeld(const InputBindings &bindings, InputAction action) const;
    bool isUp(const InputBindings &bindings, InputAction action) const;

    bool consumeDown(const InputBindings &bindings, InputAction action);
    bool consumeHeld(const InputBindings &bindings, InputAction action);
    bool consumeUp(const InputBindings &bindings, InputAction action);
    bool consume(const InputBindings &bindings, InputAction action, InputPhase phase);

    u32 remainingDown() const;
    u32 remainingHeld() const;
    u32 remainingUp() const;

private:
    bool matches(const InputBindings &bindings, InputAction action,
                 u32 rawMask, u32 consumedMask) const;
    bool consumeMask(const InputBindings &bindings, InputAction action,
                     u32 rawMask, u32 &consumedMask);

    u32 down;
    u32 held;
    u32 up;
    u32 consumedDown;
    u32 consumedHeld;
    u32 consumedUp;
};

} // namespace Doodle

#endif

#ifndef DOODLE_UI_ROUTE_H
#define DOODLE_UI_ROUTE_H

#include "renderer.h"

enum UiOverlayKind
{
    UI_OVERLAY_NONE = 0,
    UI_OVERLAY_SYNCING,
    UI_OVERLAY_DISCONNECTED,
    UI_OVERLAY_RESTRICTED,
    UI_OVERLAY_CONFIRMATION,
    UI_OVERLAY_TOAST
};

struct UiRouteEntry
{
    TopScreenMode route;
    int focus;
    int scroll;
    int tab;
};

// A bounded route stack keeps navigation history and per-route selection
// together. Canvas rendering still consumes TopScreenMode during the staged
// migration, but navigation no longer needs parallel back-target booleans.
class UiRouteStack
{
public:
    static const int CAPACITY = 8;

    UiRouteStack();

    void reset(TopScreenMode route);
    bool push(TopScreenMode route);
    bool pop();
    void replace(TopScreenMode route);

    TopScreenMode current() const;
    int depth() const;

    UiRouteEntry &state();
    const UiRouteEntry &state() const;

    void showOverlay(UiOverlayKind overlay);
    void clearOverlay();
    UiOverlayKind overlay() const;

private:
    UiRouteEntry entries_[CAPACITY];
    int depth_;
    UiOverlayKind overlay_;
};

#endif

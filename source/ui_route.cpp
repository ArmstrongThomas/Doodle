#include "ui_route.h"

static UiRouteEntry makeRouteEntry(TopScreenMode route)
{
    UiRouteEntry entry;
    entry.route = route;
    entry.focus = 0;
    entry.scroll = 0;
    entry.tab = 0;
    return entry;
}

UiRouteStack::UiRouteStack()
    : depth_(1), overlay_(UI_OVERLAY_NONE)
{
    entries_[0] = makeRouteEntry(TOP_MODE_CANVAS);
}

void UiRouteStack::reset(TopScreenMode route)
{
    depth_ = 1;
    entries_[0] = makeRouteEntry(route);
    overlay_ = UI_OVERLAY_NONE;
}

bool UiRouteStack::push(TopScreenMode route)
{
    if (current() == route)
        return true;
    if (depth_ >= CAPACITY)
        return false;
    entries_[depth_++] = makeRouteEntry(route);
    return true;
}

bool UiRouteStack::pop()
{
    if (depth_ <= 1)
        return false;
    --depth_;
    overlay_ = UI_OVERLAY_NONE;
    return true;
}

void UiRouteStack::replace(TopScreenMode route)
{
    entries_[depth_ - 1] = makeRouteEntry(route);
}

TopScreenMode UiRouteStack::current() const
{
    return entries_[depth_ - 1].route;
}

int UiRouteStack::depth() const
{
    return depth_;
}

UiRouteEntry &UiRouteStack::state()
{
    return entries_[depth_ - 1];
}

const UiRouteEntry &UiRouteStack::state() const
{
    return entries_[depth_ - 1];
}

void UiRouteStack::showOverlay(UiOverlayKind overlay)
{
    overlay_ = overlay;
}

void UiRouteStack::clearOverlay()
{
    overlay_ = UI_OVERLAY_NONE;
}

UiOverlayKind UiRouteStack::overlay() const
{
    return overlay_;
}

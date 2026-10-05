#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>

namespace H3D::Compat {

// Receives raw relative pointer motion (logical pixels, after libinput
// acceleration and general:sensitivity) while capture is enabled.
// Return true if the motion was consumed; return false to let it through to the
// real cursor (used as a safety valve so the pointer can never get stuck).
using MotionSink = bool (*)(double dx, double dy);

// Hooks Pointer::CPointerManager::move(const Vector2D&).
//
// Why a function hook: the EventBus `input.mouse.move` event delivers an
// absolute, *floored* cursor position - no event carries relative motion. Any
// mouse-look built on it (delta from screen centre + warpTo) loses sub-pixel
// motion and fights with the compositor. `CPointerManager::move` is the point
// where Hyprland applies the relative delta to the cursor: while capture is on
// we hand the delta to `sink` and do NOT call the original, so the real cursor
// never moves, nothing is warped, and no focus/hover logic is triggered.
//
// Returns false if the symbol could not be found or hooked (the plugin then
// falls back to the warp-based path in main.cpp). Symbol lookup shells out to
// `nm`, so `binutils` must be installed.
bool installPointerHook(HANDLE handle, MotionSink sink);
void removePointerHook();

bool pointerHookActive();
void setPointerCapture(bool capture);

// Hides the host cursor while the 3D view owns input (the plugin draws its own
// crosshair). Requires the cursor gates to be hooked; returns false otherwise.
bool setCursorHidden(bool hidden);

// The cursor image most recently requested through the pointer manager --
// recorded even while the gates swallow it, so the 3D view can draw the
// client's cursor itself. Exactly one of buffer / surface is set.
struct SCursorRequest {
    SP<Aquamarine::IBuffer>       buffer;  // theme and cursor-shape cursors
    float                         scale = 1.0f;
    WP<Desktop::View::CWLSurface> surface; // client-drawn cursor surfaces
    Vector2D                      hotspot; // logical px
    uint64_t                      serial = 0;
};

const SCursorRequest& lastCursorRequest();

} // namespace H3D::Compat

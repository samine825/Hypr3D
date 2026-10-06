#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/layout/space/Space.hpp>

#include <cstdint>
#include <vector>

namespace H3D::Compat {

// One participating surface, in monitor-local logical coordinates. Either a
// regular window or a layer-shell surface (panels, bars, quickshell
// PanelWindow), which live outside the window list but are shown in the room
// all the same.
struct SWindowInfo {
    std::uintptr_t id = 0;
    PHLWINDOW      window;
    PHLLS          layer;     // set when isLayer
    bool           isLayer = false;
    CBox           monitorLocalBox; // FULL decorated box (content + border/titlebar), top-left origin
    Vector2D       surfaceOffset;   // top-left of the client surface inside monitorLocalBox (border margin)
    Vector2D       surfaceSize;     // client surface size in logical px
    bool           floating = false;

    // Attached surfaces belong to a parent window and are placed relative
    // to it rather than spawned on their own: a window's xdg popups (menus,
    // tooltips; `popup` set, `window` empty) and X11 override-redirect
    // windows (X11 menus; `window` set). parentId is the parent window's id,
    // 0 when the X server named none.
    bool                          attached = false;
    std::uintptr_t                parentId = 0;
    WP<Desktop::View::CPopup>     popup;
};

// Enumerate mapped, non-hidden windows that live on a visible workspace of
// `monitor`. Windows on invisible workspaces are skipped (the snapshot path
// refuses to render them anyway).
// `underLayers` (world.under_layers): the room is drawn under the top and
// overlay layers, so only bottom-layer surfaces join it; otherwise every
// layer above the background does.
std::vector<SWindowInfo> enumerateEligibleWindows(const PHLMONITOR& monitor,
                                                  bool underLayers = false);

// True if the window still belongs to the eligible set (mapped, not hidden).
bool isWindowEligible(const PHLWINDOW& window, const PHLMONITOR& monitor);

// Full decorated box (client surface + border/titlebar) in monitor-local
// logical px, plus the client surface's offset inside it and its size. Used
// both for the entity/UV geometry and -- crucially -- recorded together with
// every snapshot, so the quad always samples the box the snapshot was
// actually rendered at. Returns false when the geometry is unusable.
bool decoratedSurfaceBox(
    const PHLWINDOW&  window,
    const PHLMONITOR& monitor,
    CBox&             fullBox,
    Vector2D&         surfOffset,
    Vector2D&         surfSize
);

// --- layout ghosting --------------------------------------------------------
//
// While 3D mode is active the tiling layout must not manage the participating
// windows: each window keeps its last 2D geometry but is removed from the
// layout's target list (Layout::ITarget::setSpaceGhost). On exit the target is
// re-assigned to its space and its exact saved box is restored.
//
// IMPORTANT: save the geometry of ALL windows first (saveWindowLayout), then
// ghost them (applyWindowGhost) — ghosting one window triggers a relayout of
// the remaining ones, which would corrupt boxes saved afterwards.
struct SWindowLayoutSave {
    std::uintptr_t id   = 0;
    PHLWINDOWREF   window;
    CBox           box;   // global logical box at save time
    SP<Layout::CSpace> space;
    bool            wasFloating = false;
    // Set by applyWindowGhost: the window actually left the layout (the
    // fullscreen window at entry, windows mapped during the session). Such
    // windows need the full space/algorithm re-adoption on restore; the
    // rest only need their flag and box pinned back (their tree node was
    // never removed).
    bool            ghosted = false;
};

SWindowLayoutSave saveWindowLayout(const PHLWINDOW& window);
void              applyWindowGhost(SWindowLayoutSave& save);
void              restoreWindowLayout(SWindowLayoutSave& save);

// Final pass after ALL restoreWindowLayout calls: one recalculate per
// involved space, then the saved tile boxes are pinned on top (and every
// window damaged). restoreWindowLayout inserts tiled windows into the
// algorithm's tree at their saved box's centre, so the rebuilt tree mirrors
// the saved arrangement instead of following the mouse.
void              restoreWindowLayoutSettle(const std::vector<SWindowLayoutSave>& saves);

// A float/tile toggle aimed at a GHOSTED window (the keybind hits the
// focused window, and ghosts can be aimed): Hyprland's
// toggleTargetFloating unconditionally inserts the target into the layout
// algorithm -- for a ghost that is an INVISIBLE node holding tree space
// (the empty tile that slowly eats the 2D layout). Eject it from the
// algorithm and restore the floating-panel state every ghost carries.
// Returns true when a fixup was applied.
bool              fixupGhostedWindow(const SWindowLayoutSave& save);

CBox currentWindowBox(const PHLWINDOW& window);
bool setWindowBox(const PHLWINDOW& window, const CBox& box);

// Like setWindowBox, but WITHOUT the setFloating(true) flip: the window
// keeps its layout membership and floating flag exactly as they are (a
// tiled window's tree node stays put). This is how the room drives a real
// resize or a morph's box animation for a window that lives inside the
// layout -- the algorithm never hears about it, and the exit's light
// restore has nothing to undo but the box itself.
bool driveWindowBox(const PHLWINDOW& window, const CBox& box);

// --- pointer delivery -------------------------------------------------------
//
// Maps a crosshair hit onto the window's client surface and delivers a pointer
// button event through the seat, so the client actually receives the click.
// `localLogical` is window-local (top-left origin, logical px, decorations
// ignored for the MVP).
void deliverMotion(
    const PHLWINDOW& window,
    const Vector2D& localLogical,
    uint32_t timeMs
);

void deliverClick(
    const PHLWINDOW& window,
    const Vector2D& localLogical,
    uint32_t button,
    bool pressed,
    uint32_t timeMs
);

// Layer-surface delivery: same seat events, pointed at the layer's client
// surface, so panels/launchers receive ordinary pointer input.
void deliverMotion(
    const PHLLS& layer,
    const Vector2D& localLogical,
    uint32_t timeMs
);

void deliverClick(
    const PHLLS& layer,
    const Vector2D& localLogical,
    uint32_t button,
    bool pressed,
    uint32_t timeMs
);

// The window owning a popup entity id (nullptr when the id is not a live
// popup of an eligible window).
PHLWINDOW findPopupParentById(std::uintptr_t id);

// Layer lookup by entity id (nullptr when the id is not a live layer).
PHLLS findLayerById(std::uintptr_t id);

void clearPointerFocus();

// Whether a top or overlay layer surface (bar, sidebar, launcher,
// notification) or a layer popup takes pointer input at a global point on
// this monitor -- Hyprland's own hit test, input regions included. Those
// draw over the room and are used as ordinary 2D surfaces.
bool interactiveLayerAt(const PHLMONITOR& monitor, const Vector2D& global);

// Whether keyboard focus is on a layer surface (a launcher, a sidebar's
// text field): the room then leaves the keyboard and focus alone.
bool layerHasKeyboardFocus();

// Scroll for whatever surface holds pointer focus (set by deliverMotion).
// `value120` is the hi-res wheel value (120 per detent, 0 for smooth).
void deliverAxis(
    uint32_t                           timeMs,
    wl_pointer_axis                    axis,
    double                             value,
    int32_t                            value120,
    wl_pointer_axis_source             source,
    wl_pointer_axis_relative_direction relative
);

} // namespace H3D::Compat

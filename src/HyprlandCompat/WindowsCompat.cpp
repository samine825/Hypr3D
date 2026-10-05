#include "HyprlandCompat/WindowsCompat.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
// CLayerShellResource's m_surface (CWLSurfaceResource) needs the full type
// for .lock()->m_current access in deliver* paths.
#include <hyprland/src/protocols/types/SurfaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <hyprutils/utils/ScopeGuard.hpp>
#include <cmath>

namespace H3D::Compat {

std::vector<SWindowInfo> enumerateEligibleWindows(const PHLMONITOR& monitor) {
    std::vector<SWindowInfo> out;

    if (!monitor || !Desktop::windowState())
        return out;

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window || !window->m_isMapped || window->isHidden())
            continue;

        const auto WORKSPACE = window->m_workspace;

        if (!WORKSPACE)
            continue;

        // The window must live on a workspace attached to this monitor that is
        // actually being rendered (the snapshot path requires it).
        if (WORKSPACE->m_monitor != monitor)
            continue;

        if (!WORKSPACE->isVisible() && !window->m_pinned && !WORKSPACE->m_forceRendering)
            continue;

        SWindowInfo info;
        info.id       = reinterpret_cast<std::uintptr_t>(window.get());
        info.window   = window;
        info.floating = window->m_isFloating;

        if (!decoratedSurfaceBox(
                window,
                monitor,
                info.monitorLocalBox,
                info.surfaceOffset,
                info.surfaceSize))
            continue;

        out.push_back(std::move(info));
    }

    // Layer-shell surfaces (panels, bars, quickshell PanelWindow) live
    // outside the window list; show them in the room as regular entities.
    // The background layer is skipped -- that is where wallpapers live, and a
    // full-screen wallpaper would swallow the whole room. Their boxes are
    // monitor-local and decoration-free.
    constexpr uint32_t LAYER_BACKGROUND = 0;

    for (const auto& LAYERLIST : monitor->m_layerSurfaceLayers) {
        for (const auto& LSREF : LAYERLIST) {
            const auto LS = LSREF.lock();

            if (!LS || !LS->visible() || LS->m_layer == LAYER_BACKGROUND)
                continue;

            const auto BOXOPT = LS->surfaceLogicalBox();

            if (!BOXOPT || BOXOPT->w <= 0 || BOXOPT->h <= 0)
                continue;

            SWindowInfo info;
            info.id      = reinterpret_cast<std::uintptr_t>(LS.get());
            info.layer   = LS;
            info.isLayer = true;

            info.monitorLocalBox = *BOXOPT;
            info.surfaceOffset   = Vector2D{0, 0};
            info.surfaceSize     = Vector2D{BOXOPT->w, BOXOPT->h};

            out.push_back(std::move(info));
        }
    }

    return out;
}

bool decoratedSurfaceBox(
    const PHLWINDOW&  window,
    const PHLMONITOR& monitor,
    CBox&             fullBox,
    Vector2D&         surfOffset,
    Vector2D&         surfSize
) {
    if (!window || !monitor)
        return false;

    // The main surface box is the content only; decorations (the border ring,
    // titlebars) extend beyond it and are part of what a snapshot renders, so
    // the 3D quad must span the unified box to show them.
    const auto SURF = window->getWindowMainSurfaceBox();
    const auto FULL = window->getWindowBoxUnified(Desktop::View::FULL_EXTENTS);

    // A dimAround rule makes the unified box cover the whole monitor; clamp
    // each side margin so a rogue rule cannot blow the quad up.
    constexpr double MAX_MARGIN = 150.0;
    const double LEFT   = std::clamp<double>(SURF.x - FULL.x, 0.0, MAX_MARGIN);
    const double TOP    = std::clamp<double>(SURF.y - FULL.y, 0.0, MAX_MARGIN);
    const double RIGHT  = std::clamp<double>((FULL.x + FULL.w) - (SURF.x + SURF.w), 0.0, MAX_MARGIN);
    const double BOTTOM = std::clamp<double>((FULL.y + FULL.h) - (SURF.y + SURF.h), 0.0, MAX_MARGIN);

    const auto MONPOS = monitor->m_position;

    fullBox = CBox{
        SURF.x - LEFT - MONPOS.x,
        SURF.y - TOP - MONPOS.y,
        SURF.w + LEFT + RIGHT,
        SURF.h + TOP + BOTTOM,
    };

    // Surface-local input coordinates are the hit position minus this offset,
    // so growing the quad to include borders never shifts input.
    surfOffset = Vector2D{LEFT, TOP};
    surfSize   = Vector2D{SURF.w, SURF.h};

    return fullBox.w > 0 && fullBox.h > 0;
}

bool isWindowEligible(const PHLWINDOW& window, const PHLMONITOR& monitor) {
    if (!window || !window->m_isMapped || window->isHidden())
        return false;

    const auto WORKSPACE = window->m_workspace;

    if (!WORKSPACE || !monitor)
        return false;

    if (WORKSPACE->m_monitor != monitor)
        return false;

    if (!WORKSPACE->isVisible() && !window->m_pinned && !WORKSPACE->m_forceRendering)
        return false;

    return true;
}

SWindowLayoutSave saveWindowLayout(const PHLWINDOW& window) {
    SWindowLayoutSave save;

    if (!window || !window->m_target)
        return save;

    save.id          = reinterpret_cast<std::uintptr_t>(window.get());
    save.window      = window;
    save.box         = window->m_target->position();
    save.space       = window->m_target->space();
    save.wasFloating = window->m_target->floating();

    return save;
}

void applyWindowGhost(SWindowLayoutSave& save) {
    if (save.window.expired())
        return;

    const auto WINDOW = save.window.lock();

    if (!WINDOW || !WINDOW->m_target)
        return;

    // The 3D view owns geometry. Force the target floating first, then ghost
    // it out of the layout so a tiled layout cannot overwrite our changes.
    WINDOW->m_target->setFloating(true);
    WINDOW->m_target->setSpaceGhost(save.space);
}

void restoreWindowLayout(SWindowLayoutSave& save) {
    if (save.window.expired())
        return;

    const auto WINDOW = save.window.lock();

    if (!WINDOW || !WINDOW->m_target)
        return;

    // Clear the ghost link FIRST. assignToSpace straight from the ghost
    // state takes the space->move() branch (the ghosted target was never a
    // member of its space, but HAD_SPACE reads true from the ghost
    // pointer), and the layout algorithm learns nothing from a moveTarget()
    // of a window it does not know. With the ghost cleared first, HAD_SPACE
    // is false and the space->add() branch runs, which properly inserts the
    // target into the layout algorithm.
    WINDOW->m_target->setSpaceGhost(nullptr);

    // The exact box the window had when 3D was entered -- for floating
    // windows too: sizes are per-mode, and a 3D resize must not survive
    // into 2D (the exit morph animates the real box back to this one).
    const CBox RESTORE_BOX = save.box;

    if (save.space)
        WINDOW->m_target->assignToSpace(save.space);
    else
        WINDOW->m_target->assignToSpace(nullptr); // force-clear the ghost flag

    if (save.wasFloating) {
        WINDOW->m_target->setPositionGlobal(RESTORE_BOX);
        WINDOW->m_target->rememberFloatingSize(
            Vector2D{RESTORE_BOX.w, RESTORE_BOX.h});
    } else {
        // The canonical float->tile transition. The bare
        // m_target->setFloating(false) only flips the flag and notifies
        // rules -- the layout algorithm's own floating-target bookkeeping
        // never hears about it, and the window ends up positioned but
        // unmanaged: frozen outside the layout until the next interaction
        // adopts it. CSpace::toggleTargetFloating is what Hyprland's own
        // changeFloatingMode calls; it routes through
        // CAlgorithm::setFloating (remove + re-insert by the new state).
        if (save.space)
            save.space->toggleTargetFloating(WINDOW->m_target);
        else
            WINDOW->m_target->setFloating(false);

        // Pin the exact pre-3D tile box on top of the algorithm's
        // arrangement (same insertion order reproduces the same layout;
        // this covers the corner cases).
        WINDOW->m_target->setPositionGlobal(RESTORE_BOX);
        WINDOW->m_target->rememberFloatingSize(
            Vector2D{RESTORE_BOX.w, RESTORE_BOX.h});
    }

    g_pHyprRenderer->damageWindow(WINDOW);
}

CBox currentWindowBox(const PHLWINDOW& window) {
    if (!window || !window->m_target)
        return {};

    return window->m_target->position();
}

bool setWindowBox(const PHLWINDOW& window, const CBox& box) {
    if (!window || !window->m_target)
        return false;

    const CBox CURRENT = window->m_target->position();

    const bool unchanged =
        std::fabs(CURRENT.x - box.x) < 0.01 &&
        std::fabs(CURRENT.y - box.y) < 0.01 &&
        std::fabs(CURRENT.w - box.w) < 0.01 &&
        std::fabs(CURRENT.h - box.h) < 0.01;

    if (unchanged)
        return false;

    window->m_target->setFloating(true);
    window->m_target->setPositionGlobal(box);
    window->m_target->rememberFloatingSize(Vector2D{box.w, box.h});

    if (g_pHyprRenderer)
        g_pHyprRenderer->damageWindow(window);

    return true;
}

PHLLS findLayerById(std::uintptr_t id) {
    if (id == 0)
        return nullptr;

    for (const auto& MONITOR : State::monitorState()->monitors()) {
        for (const auto& LAYERLIST : MONITOR->m_layerSurfaceLayers) {
            for (const auto& LSREF : LAYERLIST) {
                const auto LS = LSREF.lock();

                if (LS && reinterpret_cast<std::uintptr_t>(LS.get()) == id)
                    return LS;
            }
        }
    }

    return nullptr;
}

void clearPointerFocus() {
    if (!g_pSeatManager)
        return;

    g_pSeatManager->setPointerFocus(nullptr, {});
}

void deliverMotion(
    const PHLWINDOW& window,
    const Vector2D& localLogical,
    uint32_t timeMs
) {
    if (!window || !g_pSeatManager)
        return;

    const auto SURFACE = window->resource();
    if (!SURFACE)
        return;

    g_pSeatManager->setPointerFocus(SURFACE, localLogical);
    g_pSeatManager->sendPointerMotion(timeMs, localLogical);
    g_pSeatManager->sendPointerFrame();
}

void deliverClick(
    const PHLWINDOW& window,
    const Vector2D&  localLogical,
    uint32_t         button,
    bool             pressed,
    uint32_t         timeMs
) {
    if (!window)
        return;

    const auto SURFACE = window->resource();

    if (!SURFACE || !g_pSeatManager)
        return;

    // Point the seat at the aimed surface, put the virtual pointer at the hit
    // coordinate, then send the button and a frame. The application therefore
    // sees an ordinary Wayland pointer event even though the physical cursor
    // is captured by the 3D view.
    g_pSeatManager->setPointerFocus(SURFACE, localLogical);
    g_pSeatManager->sendPointerMotion(timeMs, localLogical);
    g_pSeatManager->sendPointerButton(
        timeMs,
        button,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
    );
    g_pSeatManager->sendPointerFrame();
}

void deliverMotion(
    const PHLLS& layer,
    const Vector2D& localLogical,
    uint32_t timeMs
) {
    if (!layer || !g_pSeatManager)
        return;

    const auto SURFACE =
        layer->m_layerSurface ? layer->m_layerSurface->m_surface.lock() : nullptr;

    if (!SURFACE)
        return;

    g_pSeatManager->setPointerFocus(SURFACE, localLogical);
    g_pSeatManager->sendPointerMotion(timeMs, localLogical);
    g_pSeatManager->sendPointerFrame();
}

void deliverClick(
    const PHLLS& layer,
    const Vector2D& localLogical,
    uint32_t button,
    bool pressed,
    uint32_t timeMs
) {
    if (!layer || !g_pSeatManager)
        return;

    const auto SURFACE =
        layer->m_layerSurface ? layer->m_layerSurface->m_surface.lock() : nullptr;

    if (!SURFACE)
        return;

    g_pSeatManager->setPointerFocus(SURFACE, localLogical);
    g_pSeatManager->sendPointerMotion(timeMs, localLogical);
    g_pSeatManager->sendPointerButton(
        timeMs,
        button,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
    );
    g_pSeatManager->sendPointerFrame();
}

} // namespace H3D::Compat

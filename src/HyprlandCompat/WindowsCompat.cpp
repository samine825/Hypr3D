#include "HyprlandCompat/WindowsCompat.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
// CLayerShellResource's m_surface (CWLSurfaceResource) needs the full type
// for .lock()->m_current access in deliver* paths.
#include <hyprland/src/protocols/types/SurfaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <cmath>

namespace H3D::Compat {

// The room holds the monitor's ACTIVE workspace (plus its special workspace
// and pinned windows) -- not every workspace that happens to be visible.
// During a workspace switch the outgoing workspace still renders while it
// slides away; picking its windows up ghosted them out of their tiling for
// a few frames, and the layout re-tiled (resized) them on the way back.
static bool onRoomWorkspace(const PHLWINDOW& window, const PHLMONITOR& monitor) {
    const auto WORKSPACE = window->m_workspace;

    return window->m_pinned || WORKSPACE == monitor->m_activeWorkspace ||
        (WORKSPACE && WORKSPACE == monitor->m_activeSpecialWorkspace);
}

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

        if (!onRoomWorkspace(window, monitor))
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

    if (!onRoomWorkspace(window, monitor))
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
    save.ghosted = true;
}

void restoreWindowLayout(SWindowLayoutSave& save) {
    if (save.window.expired())
        return;

    const auto WINDOW = save.window.lock();

    if (!WINDOW || !WINDOW->m_target)
        return;

    // A window closed while the room was open can outlive its unmap (the
    // close animation keeps the object) with no workspace left; assigning it
    // a space then crashed in moveToWorkspace, which reads the OLD
    // workspace (SIGSEGV in CWorkspace::isVisible). Hyprland is done with it.
    if (!WINDOW->m_isMapped || !WINDOW->m_workspace)
        return;

    if (!save.ghosted) {
        // The window never left the layout: its tree node is alive, its
        // floating flag was never flipped (the room's box drives go through
        // driveWindowBox), and the exit morph already landed the box where
        // the tree expects it. Nothing to re-adopt -- calling the
        // algorithm's remove/re-insert here would DESTROY the live node
        // and a rebuild could only approximate it. Pin the remembered
        // floating size to the current box so a later manual float uses
        // the geometry this window actually has.
        WINDOW->m_target->rememberFloatingSize(
            Vector2D{WINDOW->m_target->position().w,
                     WINDOW->m_target->position().h});

        if (g_pHyprRenderer)
            g_pHyprRenderer->damageWindow(WINDOW);
        return;
    }

    // Clear the ghost link FIRST. assignToSpace straight from the ghost
    // state takes the space->move() branch (the ghosted target was never a
    // member of its space, but HAD_SPACE reads true from the ghost
    // pointer), and the layout algorithm learns nothing from a moveTarget()
    // of a window it does not know. With the ghost cleared first, HAD_SPACE
    // is false and the space->add() branch runs, which properly inserts the
    // target into the layout algorithm.
    WINDOW->m_target->setSpaceGhost(nullptr);

    // The saved space can outlive its workspace: a session lock or a monitor
    // re-plug tears workspaces down and rebuilds them, and assigning into a
    // space whose workspace is gone dereferenced it (SIGSEGV in
    // CWorkspace::isVisible). Fall back to the space of the workspace the
    // window is on now.
    SP<Layout::CSpace> SPACE = save.space;
    if (SPACE) {
        const auto WS = SPACE->workspace();
        if (!WS || WS->inert())
            SPACE = nullptr;
    }
    if (!SPACE && WINDOW->m_workspace && !WINDOW->m_workspace->inert())
        SPACE = WINDOW->m_workspace->m_space;

    if (SPACE)
        WINDOW->m_target->assignToSpace(SPACE);
    else
        WINDOW->m_target->assignToSpace(nullptr); // force-clear the ghost flag

    if (save.wasFloating) {
        // The exact box the window had when 3D was entered: sizes are
        // per-mode, and a 3D resize must not survive into 2D (the exit
        // morph animates the real box back to this one).
        WINDOW->m_target->setPositionGlobal(save.box);
        WINDOW->m_target->rememberFloatingSize(Vector2D{save.box.w, save.box.h});
        g_pHyprRenderer->damageWindow(WINDOW);
        return;
    }

    // Tiled: adopt into the layout algorithm's TREE. The space->add() above
    // routed the target into the algorithm's FLOATING list (addTarget
    // follows the floating flag the ghost left set); moving it to the tiled
    // side needs the algorithm itself -- a bare setFloating(false) only
    // flips a flag and the window ends up positioned but unmanaged.
    //
    // The re-insert carries the saved box's CENTRE as the focal point: the
    // tiled algorithm splits the node closest to it, so the rebuilt tree
    // mirrors the saved arrangement instead of scattering around wherever
    // the mouse happens to be. Geometry is not final here -- the settle
    // pass recalculates once and pins the exact saved boxes after ALL
    // windows are in (an earlier pin would be overridden by the next
    // window's insertion recalcs).
    bool adopted = false;

    if (save.space) {
        if (const auto ALGO = save.space->algorithm()) {
            ALGO->removeTarget(WINDOW->m_target);
            WINDOW->m_target->setFloating(false);
            ALGO->moveTarget(WINDOW->m_target, save.box.middle());
            adopted = true;
        }
    }

    if (!adopted)
        WINDOW->m_target->setFloating(false);
}

bool fixupGhostedWindow(const SWindowLayoutSave& save) {
    if (save.window.expired() || !save.ghosted)
        return false;

    const auto WINDOW = save.window.lock();

    if (!WINDOW || !WINDOW->m_target || WINDOW->m_target->floating())
        return false;

    if (save.space) {
        if (const auto ALGO = save.space->algorithm())
            ALGO->removeTarget(WINDOW->m_target);
    }

    WINDOW->m_target->setFloating(true);

    if (g_pHyprRenderer)
        g_pHyprRenderer->damageWindow(WINDOW);

    return true;
}

void restoreWindowLayoutSettle(const std::vector<SWindowLayoutSave>& saves) {
    // One recalculate per involved space: normalizes gaps/workarea over the
    // rebuilt tree.
    std::vector<SP<Layout::CSpace>> spaces;

    for (const auto& SAVE : saves) {
        if (!SAVE.space || SAVE.window.expired())
            continue;

        if (std::ranges::find(spaces, SAVE.space) == spaces.end())
            spaces.push_back(SAVE.space);
    }

    for (const auto& SPACE : spaces)
        SPACE->recalculate();

    // The tree was never torn down, so the recalculate re-asserts the
    // ORIGINAL arrangement -- no pins on top (they would only fight a
    // legitimate mid-session reflow). Ghosted windows (the fullscreen one,
    // session newcomers) were restored by their own full path.
    for (const auto& SAVE : saves) {
        const auto WINDOW = SAVE.window.lock();

        if (WINDOW && g_pHyprRenderer)
            g_pHyprRenderer->damageWindow(WINDOW);
    }
}

bool driveWindowBox(const PHLWINDOW& window, const CBox& box) {
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

    window->m_target->setPositionGlobal(box);
    window->m_target->rememberFloatingSize(Vector2D{box.w, box.h});
    // The room's box writes must land instantly: the snapshot's box comes
    // from the window's ANIMATED geometry (getWindowMainSurfaceBox reads
    // GEOMETRIC_CURRENT), so an animated drive makes the quad trail the
    // gesture -- a fresh window's vars even carry the slow open-animation
    // config. Same trick the compositor's own drag controller uses.
    window->m_target->warpPositionSize();

    if (g_pHyprRenderer)
        g_pHyprRenderer->damageWindow(window);

    return true;
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
    window->m_target->warpPositionSize(); // instant landing, see driveWindowBox

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

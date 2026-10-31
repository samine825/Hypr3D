#include "HyprlandCompat/FocusCompat.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>

namespace H3D::Compat {

namespace {

// Reason reported to listeners of the focus signal. KEYBIND is used because it
// is known to exist in 0.56.2 (the previous code compiled with it). If you want
// the semantics of "focus follows aim", try Desktop::FOCUS_REASON_FFM instead.
const auto kFocusReason = Desktop::FOCUS_REASON_KEYBIND;

// CFocusState::monitor() is documented but not used anywhere in this project
// yet; degrade to "unknown" (nullptr) instead of failing the build if absent.
template <class FocusState>
PHLMONITOR monitorOf(FocusState&& state) {
    if constexpr (requires { state->monitor(); })
        return state->monitor();
    else
        return nullptr;
}

} // namespace

std::uintptr_t windowId(const PHLWINDOW& window) {
    return window ? reinterpret_cast<std::uintptr_t>(window.get()) : 0;
}

PHLWINDOW findWindowById(std::uintptr_t id) {
    if (id == 0 || !Desktop::windowState())
        return nullptr;

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window || windowId(window) != id)
            continue;

        if (!window->mapped() || window->isHidden())
            return nullptr;

        return window;
    }

    return nullptr;
}

PHLWINDOW focusedWindow() {
    if (!Desktop::focusState())
        return nullptr;

    return Desktop::focusState()->window();
}

PHLMONITOR focusedMonitor() {
    if (!Desktop::focusState())
        return nullptr;

    return monitorOf(Desktop::focusState());
}

void focusWindow(const PHLWINDOW& window) {
    if (!window || !Desktop::focusState())
        return;

    Desktop::focusState()->fullWindowFocus(window, kFocusReason);
}

void clearFocus() {
    if (!Desktop::focusState() || !Desktop::focusState()->window())
        return;

    // rawWindowFocus with an empty window resets the focused window/surface and
    // clears keyboard focus in the seat.
    Desktop::focusState()->rawWindowFocus(PHLWINDOW{}, kFocusReason);
}

} // namespace H3D::Compat

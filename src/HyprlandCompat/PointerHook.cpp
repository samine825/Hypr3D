#include "HyprlandCompat/PointerHook.hpp"

#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>

#include <string>
#include <vector>

namespace H3D::Compat {

namespace {

using MoveFn    = void (*)(void* self, const Vector2D& delta);
using SetSurfFn = void (*)(void* self, SP<Desktop::View::CWLSurface>, const Vector2D&);
using SetBufFn  = void (*)(void* self, SP<Aquamarine::IBuffer>, const Vector2D&, const float&);

CFunctionHook* g_moveHook     = nullptr;
MoveFn         g_moveOriginal = nullptr;

CFunctionHook* g_surfHook     = nullptr;
SetSurfFn      g_surfOriginal = nullptr;

CFunctionHook* g_bufHook      = nullptr;
SetBufFn       g_bufOriginal  = nullptr;

MotionSink     g_sink         = nullptr;
bool           g_capture      = false;
bool           g_cursorHooks  = false;
bool           g_cursorHidden = false;

void hkPointerMove(void* self, const Vector2D& delta) {
    if (g_capture && g_sink && g_sink(delta.x, delta.y))
        return; // swallowed: the real cursor stays where it is

    if (g_moveOriginal)
        ((MoveFn)g_moveOriginal)(self, delta);
}

// Gated at the pointer MANAGER, not the renderer: every cursor update path
// funnels here -- the renderer's setCursorSurface/FromName, the cursor
// manager (theme + shape protocol) and direct callers. Nothing re-applies a
// cursor image behind the gates while the 3D view owns input.
SCursorRequest g_cursorRequest;

void hkPMSetCursorSurface(void* self, SP<Desktop::View::CWLSurface> surf, const Vector2D& hotspot) {
    g_cursorRequest.buffer.reset();
    g_cursorRequest.surface = surf;
    g_cursorRequest.hotspot = hotspot;
    ++g_cursorRequest.serial;

    if (g_cursorHidden)
        return; // swallowed: the cursor image stays hidden while the 3D view owns input

    if (g_surfOriginal)
        ((SetSurfFn)g_surfOriginal)(self, surf, hotspot);
}

void hkPMSetCursorBuffer(void* self, SP<Aquamarine::IBuffer> buf, const Vector2D& hotspot, const float& scale) {
    g_cursorRequest.buffer  = buf;
    g_cursorRequest.scale   = scale > 0.0f ? scale : 1.0f;
    g_cursorRequest.surface.reset();
    g_cursorRequest.hotspot = hotspot;
    ++g_cursorRequest.serial;

    if (g_cursorHidden)
        return;

    if (g_bufOriginal)
        ((SetBufFn)g_bufOriginal)(self, buf, hotspot, scale);
}

// The hook object stores the trampoline under `m_original` in current headers
// and under `m_pOriginal` in older ones; accept either.
template <class Hook>
void* originalOf(Hook* hook) {
    if constexpr (requires { hook->m_original; })
        return (void*)hook->m_original;
    else
        return (void*)hook->m_pOriginal;
}

// SFunctionMatch exposes the demangled name as `demangled`; fall back to the
// raw signature if a future version renames it.
template <class Match>
std::string nameOf(const Match& match) {
    if constexpr (requires { match.demangled; })
        return match.demangled;
    else
        return match.signature;
}

bool isPointerMove(const std::string& name) {
    // e.g. "Pointer::CPointerManager::move(Hyprutils::Math::Vector2D const&)"
    if (name.find("CPointerManager::move(") == std::string::npos)
        return false;
    if (name.find("Vector2D") == std::string::npos)
        return false;
    if (name.find("lambda") != std::string::npos || name.find("::operator") != std::string::npos)
        return false;
    return !name.empty() && name.back() == ')';
}

bool isSetCursorSurface(const std::string& name) {
    return name.find("CPointerManager::setCursorSurface(") != std::string::npos ||
        name.find("15CPointerManager16setCursorSurface") != std::string::npos;
}

bool isSetCursorBuffer(const std::string& name) {
    return name.find("CPointerManager::setCursorBuffer(") != std::string::npos ||
        name.find("15CPointerManager14setCursorBuffer") != std::string::npos;
}

void* findFunction(HANDLE handle, const char* query, bool (*match)(const std::string&)) {
    const auto matches = HyprlandAPI::findFunctionsByName(handle, query);

    for (const auto& M : matches) {
        if (M.address && match(nameOf(M)))
            return M.address;
    }

    return nullptr;
}

CFunctionHook* installOne(HANDLE handle, void* target, void* detour, void** original) {
    auto hook = HyprlandAPI::createFunctionHook(handle, target, detour);

    if (!hook || !hook->hook())
        return nullptr;

    *original = originalOf(hook);

    if (!*original)
        return nullptr;

    return hook;
}

} // namespace

bool installPointerHook(HANDLE handle, MotionSink sink) {
    if (g_moveHook)
        return true;

    void* target = findFunction(handle, "CPointerManager::move", isPointerMove);

    if (!target)
        target = findFunction(handle, "CPointerManager4move", isPointerMove);

    if (!target)
        return false;

    g_moveHook = installOne(handle, target, (void*)&hkPointerMove, (void**)&g_moveOriginal);

    if (!g_moveHook) {
        g_moveHook     = nullptr;
        g_moveOriginal = nullptr;
        return false;
    }

    g_sink    = sink;
    g_capture = false;

    // Optional cursor gates: without them the cursor cannot be hidden
    // reliably (clients would re-apply their images), which is fine -- the
    // view just keeps the frozen cursor.
    void* surfTarget = findFunction(handle, "setCursorSurface", isSetCursorSurface);
    void* bufTarget  = findFunction(handle, "setCursorBuffer", isSetCursorBuffer);

    if (surfTarget && bufTarget) {
        g_surfHook = installOne(handle, surfTarget, (void*)&hkPMSetCursorSurface, (void**)&g_surfOriginal);
        g_bufHook  = installOne(handle, bufTarget, (void*)&hkPMSetCursorBuffer, (void**)&g_bufOriginal);

        g_cursorHooks = g_surfHook && g_bufHook && g_surfOriginal && g_bufOriginal;
    }

    return true;
}

void removePointerHook() {
    g_cursorRequest = {};
    g_capture       = false;
    g_sink          = nullptr;
    g_cursorHidden  = false;
    g_cursorHooks   = false;

    if (g_moveHook) {
        g_moveHook->unhook();
        g_moveHook = nullptr;
    }
    g_moveOriginal = nullptr;

    if (g_surfHook) {
        g_surfHook->unhook();
        g_surfHook = nullptr;
    }
    g_surfOriginal = nullptr;

    if (g_bufHook) {
        g_bufHook->unhook();
        g_bufHook = nullptr;
    }
    g_bufOriginal = nullptr;
}

bool pointerHookActive() {
    return g_moveHook != nullptr;
}

bool setCursorHidden(bool hidden) {
    if (hidden && !g_cursorHooks)
        return false; // cannot guarantee the cursor stays hidden

    g_cursorHidden = hidden;
    return true;
}

const SCursorRequest& lastCursorRequest() {
    return g_cursorRequest;
}

void setPointerCapture(bool capture) {
    g_capture = capture && g_moveHook != nullptr;
}

} // namespace H3D::Compat

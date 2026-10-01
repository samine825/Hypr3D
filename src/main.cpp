#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/memory/UniquePtr.hpp>

// This system's lua headers (5.5) lost the extern "C" guard: including them
// from C++ mangles the API names and the plugin fails to load with
// "undefined symbol: _Z8lua_typeP9lua_Statei". liblua exports plain C
// symbols, so the linkage is forced here.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "Render/GLScene.hpp"
#include "Input/AimFocus.hpp"
#include "Input/InputController.hpp"
#include "World/MapCollision.hpp"
#include "World/World3D.hpp"
#include "HyprlandCompat/WindowsCompat.hpp"
#include "HyprlandCompat/FocusCompat.hpp"
#include "HyprlandCompat/WindowCapture.hpp"
#include "HyprlandCompat/PointerHook.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace H3D {

static HANDLE PHANDLE = nullptr;

static GLScene         g_scene;
static InputController g_input;
static World3D::CWorld g_world;
static AimFocus        g_aim;
static Compat::CWindowCapture g_capture;

static bool g_active = false;

static float g_transition = 0.0f;
static float g_transitionTarget = 0.0f;

static PHLMONITOR g_monitor = nullptr;

static const auto g_inputClockStart = std::chrono::steady_clock::now();
static std::chrono::steady_clock::time_point g_lastTick =
    g_inputClockStart;

static bool g_renderedOnce = false;
static bool g_reportedFramebufferError = false;
static bool g_reportedRenderError = false;
static bool g_reportedPointerHookError = false;

// Layout snapshot taken on entry; empty once the windows are back under the
// layout's control.
static std::vector<Compat::SWindowLayoutSave> g_layoutSaves;
static bool g_ghosted = false;

// What to draw this frame, rebuilt once per frame from the world.
static std::vector<GLScene::WindowRender> g_renderWindows;

// The window that currently owns keyboard focus, as the aim logic last set it.
static std::uintptr_t g_lastFocusId = 0;

// 3D FPS-style mouse gestures. Super is the modifier: LMB moves the aimed
// window in the 3D room, RMB resizes its real Hyprland/Wayland geometry.
enum class EPointerGesture : uint8_t { None, Move3D, ResizeReal, WheelRoll };

static bool            g_pointerDown = false;
static EPointerGesture g_pointerGesture = EPointerGesture::None;
static uint32_t        g_pointerButton = 0;
static bool             g_superHeld = false;

static PHLWINDOW        g_clientButtonWindow;
static PHLLS            g_clientButtonLayer;
static Vector2D          g_clientButtonLocal{};
static uint32_t         g_clientButton = 0;
static bool              g_clientButtonDown = false;

struct SResizeGesture {
    bool          active = false;
    std::uintptr_t id = 0;
    PHLWINDOW     window;
    CBox          startBox{};
    Vec3          startCenter{};
    float         startWorldWidth = 0.0f;
    float         startWorldHeight = 0.0f;
    Vec3          planePoint{};
    Vec3          planeNormal{0.0f, 0.0f, 1.0f};
    // Crosshair position on the real window at grab time, in surface px. The
    // resize is driven by the aim's DELTA from this point, so the grabbed
    // corner never snaps to the crosshair when the gesture starts.
    Vector2D      grabPx{};
    int           edgeX = 0; // -1 left, +1 right
    int           edgeY = 0; // +1 top, -1 bottom
};

// Player spawn point in the room, set via hl.plugin.hypr3d.config().
static SResizeGesture g_resize{};

// Guards against a snapshot triggering another copy of the plugin inside
// Hyprland's nested offscreen render.
static bool g_capturing = false;

// Snapshot scheduling: the snapshot pass is the heaviest per-frame cost, so
// windows whose committed buffer and box are unchanged skip it entirely
// (checked inside the capture layer). `force` refreshes regardless -- for the
// aimed window, any gesture target, and the keyboard-focused window (whose
// border styling changes with focus). This is what keeps the room cheap while
// a screen capture (OBS/pipewire) copies every damaged frame.
static uint64_t g_captureFrames = 0;
static std::uintptr_t g_lastAimedId = 0;

// Super+wheel-PRESS roll: rotating the aimed window around its normal by
// sweeping the crosshair around the window center. The swept angle is
// measured in WORLD space around the normal, which is invariant to the roll
// itself -- only camera rotation drives it, so the gesture never feeds back
// into itself.
static struct SWheelRotate {
    bool           active     = false;
    std::uintptr_t id         = 0;
    Vec3           center     = {};
    Vec3           normal     = {};
    Vec3           reference  = {};  // crosshair point - center at grab
    float          startRoll  = 0.0f;
} s_wheelRot;

// Super+wheel hover zoom: glides the aimed window along the ray from the
// camera through the window toward the target distance (exponential lerp,
// same feel as the drag zoom). Cleared when a drag takes over the window or
// the entity disappears.
static std::uintptr_t s_zoomId     = 0;
static Vec3           s_zoomDir    = {};
static Vec3           s_zoomAnchor = {}; // camera position at the last wheel event
static double         s_zoomCur    = 0.0;
static double         s_zoomTarget = 0.0;

// Fullscreen passthrough: when Hyprland fullscreens a window while the 3D
// view is open, the window's quad animates to face the (frozen) camera and
// stretch exactly over the frustum, then the plugin hands the screen back to
// the 2D compositor: input is released, the cursor reappears, the 3D scene
// stops rendering. Exiting fullscreen reverses it -- the quad is back, input
// captured, cursor hidden. The real window is forced to the monitor box
// during 2D so the compositor shows it truly fullscreened.
static void resetPointerGesture();

enum class EFullscreenPhase : uint8_t { None, To2D, In2D, To3D };
static EFullscreenPhase g_fsPhase = EFullscreenPhase::None;
static PHLWINDOWREF     g_fsWindow;
static CBox             g_fsRestoreBox{}; // floating box before the fullscreen
static CBox             g_fsMonitorBox{}; // the fullscreen target box
static float            g_fsDistance = 8.0f; // fov-derived, set by startTo2D
static Vec3             g_fsStartCenter{}, g_fsEndCenter{};
static float g_fsStartYaw = 0.f, g_fsStartPitch = 0.f;
static float g_fsEndYaw = 0.f, g_fsEndPitch = 0.f;
static float g_fsStartRoll = 0.f;
static std::chrono::steady_clock::time_point g_fsPhaseStart{};
static bool  g_fsWasOn = false; // a fullscreen window existed since the last poll
static PHLWINDOWREF g_fsLastFSWindow; // the most recent fullscreen window
static std::uintptr_t g_fsCurrentId = 0; // the id of the CURRENT fullscreen window, 0 = none
static float g_fsAlpha = 1.0f;  // composite alpha during the transition

// The room's own fade for the NON-fullscreen windows: 1 = visible. Driven by
// the pump (deterministic ticks), sampled into WindowRender::alpha -- the
// snapshot alpha is baked at capture time and never advances (the buffer
// does not change during an alpha fade), so a snapshot-driven fade freezes
// after a couple of frames. Hyprland's own fade animates the real channels;
// ours guarantees the room matches it.
static float g_fsFade = 1.0f;
static std::chrono::steady_clock::time_point g_fsFadeLast{};
static float g_fsRawP   = 0.0f;  // raw (pre-smoothstep) transition progress
static float g_fsSavedRoll   = 0.0f; // the window's roll before the fullscreen
static float g_fsRollAtStart = 0.0f; // roll at the To3D start (aborts may differ)
static int   g_fsAssertFrames = 0;   // re-assert the restored box...
static CBox  g_fsAssertBox{};
static int   g_fsStableCount  = 0;   // consecutive frames the box matched

// Last known real box of every window OUTSIDE any fullscreen transition,
// refreshed each frame. The pre-fullscreen box must come from here: by the
// time the pump detects the fullscreen, Hyprland has already resized the
// window to the monitor, so the live geometry is NOT the pre-FS box.
static std::unordered_map<std::uintptr_t, CBox> g_fsStableBoxes;
static bool  g_captureReleasePending = false;
static constexpr float kFsAnimDuration = 0.6f;

static void pollFullscreen();
static void startTo2D(const PHLWINDOW& window, bool captureRestoreBox = true);

// Windows that appear while the view is open spawn as floating panels of
// this logical size (see ghostWindows), and enter the room this far in front
// of the camera, facing it.
static constexpr float kSpawnWidth    = 960.0f;
static constexpr float kSpawnHeight   = 540.0f;
static constexpr float kSpawnDistance = 10.0f;

// A normal damage cycle stops when nothing else in Hyprland changes. 3D mode is
// itself an animated scene, so keep a small render pump alive while it is open.
// 8 ms targets roughly 120 Hz without making the event loop spin continuously.
static SP<CEventLoopTimer> g_framePump;
static constexpr auto kFramePumpInterval = std::chrono::milliseconds(8);

// Two keyboard modes, toggled with Super + Left Alt:
//   Space  -- WASD/Space/Shift/Ctrl drive the camera;
//   Window -- every key reaches the focused window for typing.
// The mouse behaves identically in both: it always looks around, drags
// windows (Super+LMB), resizes (Super+RMB) and clicks through to clients.
enum class EKeyboardMode : uint8_t { Space, Window };
static EKeyboardMode g_keyboardMode = EKeyboardMode::Space;
static bool          g_altHeld      = false;

// Camera movement keys, set only while the 3D view owns the keyboard.
static bool g_keyFwd = false;
static bool g_keyBack = false;
static bool g_keyLeft = false;
static bool g_keyRight = false;
static bool g_keyUp = false;
static bool g_keyDown = false;
static bool g_keySprint = false;

static bool g_hookInstalled = false;

// Plugin settings, set from lua via hl.plugin.hypr3d.config({...}). Missing
// keys keep their current value, so a partial config only touches what it
// names; wrong-typed keys raise a lua error. Everything is clamped on set.
// --- world ------------------------------------------------------------------
static std::string g_cfgPanorama;                // panorama image path
static std::string g_cfgMonitor;                 // monitor name (e.g. "DP-1"), empty = focused
static bool        g_cfgGrid = true;             // base grid platform on/off

// --- windows ----------------------------------------------------------------
static float       g_cfgWindowScale   = 0.5f;    // room multiplier on window size
static float       g_cfgSpawnDistance = 5.0f;    // units in front of the camera

// --- player -----------------------------------------------------------------
static float       g_cfgLookInertia   = 0.03f;   // seconds, 0 = off
static float       g_cfgMoveInertia   = 0.05f;   // seconds, 0 = off
static float       g_cfgMoveSpeed     = 4.0f;    // world units / second
static float       g_cfgSensitivity   = 0.0025f; // radians per pointer count
static bool        g_playerFlying     = true;    // false = walk / jump / gravity

// Feet position; eyes ride kEyeHeight above (spawn 0,0,0 = standing on
// the grid platform at world zero).
static Vec3 g_playerSpawn{0.0f, 0.0f, 0.0f};

// Walking physics state.
static bool  g_grounded    = false;
static float g_verticalVel = 0.0f;

// --- map --------------------------------------------------------------------
static std::string g_cfgMapPath;
static Vec3        g_mapPosition{};
static Vec3        g_mapRotationDeg{};           // XYZ Euler, degrees
static Vec3        g_mapScale{1.0f, 1.0f, 1.0f}; // per-axis
static float       g_mapEmissiveScale = 1.0f;
static bool        g_mapFlat = true;
static bool        g_mapCollisionOn = true;

// F3 debug HUD: collision wireframe + room info overlay.
static bool        g_debugHud = false;
static float       g_debugFps = 0.0f;

static CMapCollision g_mapCollision;
static uint64_t      g_mapCollisionSetup = 0;

static void notify(const std::string& text, const CHyprColor& color);

// Feeds the requested path to the scene every frame. Tilde is expanded, a
// missing file is reported once per path, and the scene itself reloads the
// texture when the file's mtime changes.
static void updatePanorama() {
    std::string path = g_cfgPanorama;

    if (!path.empty() && path.starts_with('~')) {
        if (const char* HOME = getenv("HOME"))
            path = std::string{HOME} + path.substr(1);
    }

    if (!path.empty()) {
        std::error_code ec;

        if (!std::filesystem::exists(path, ec)) {
            static std::string notified;

            if (notified != path) {
                notified = path;
                notify(
                    "[hypr3d] panorama file not found: " + path,
                    CHyprColor{1.0f, 0.6f, 0.2f, 1.0f}
                );
            }
            return;
        }
    }

    g_scene.setPanoramaPath(path);
}

// --- diagnostics ------------------------------------------------------------
// Written to /tmp/hypr3d-status.txt while the view is live. Both reported
// bugs (mouse not rotating, 2D desktop showing through) admit several
// plausible causes, and guessing between them from the outside cost far more
// time than one dump of the actual numbers.
static uint64_t g_diagMoveEvents = 0;
static uint64_t g_diagSinkCalls   = 0;
static Vector2D g_diagLastPos{};
static double   g_diagLastDx = 0.0;
static double   g_diagLastDy = 0.0;
static double   g_diagLastZoomStep = 0.0; // normalized wheel steps (+ = forward)
static Vector2D g_diagPinned{};
static bool     g_diagPinnedValid = false;
static bool     g_warping         = false;
static uint64_t g_diagWarpCalls   = 0;
static Vector2D g_diagWarpAfter{};
static Vector2D g_diagMgrPos{};

// How far (logical px) the cursor may stray from the crosshair before it is
// warped back. Declared with the other diagnostics so the dump and the code
// that uses it cannot drift apart.
static constexpr double kWarpRadiusPx = 8.0;
static float    g_diagAlpha = 0.0f;
static uint64_t g_diagFrames = 0;
static std::chrono::steady_clock::time_point g_diagLastDump{};

static void notify(
    const std::string& text,
    const CHyprColor& color
) {
    if (!PHANDLE)
        return;

    HyprlandAPI::addNotification(
        PHANDLE,
        text,
        color,
        3000
    );
}

static uint32_t inputTimeMs() {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_inputClockStart
        ).count();

    return static_cast<uint32_t>(std::max<int64_t>(0, elapsed));
}

static void damageCurrentMonitor() {
    if (!g_monitor)
        return;

    if (!g_pHyprRenderer)
        return;

    g_pHyprRenderer->damageMonitor(
        g_monitor
    );
}

static PHLMONITOR g_currentRenderMon = nullptr;

// The monitor the 3D view is built for: configured monitor, or focused monitor,
// falling back to whatever render.pre last reported.
static PHLMONITOR targetMonitor() {
    if (!g_cfgMonitor.empty() && State::monitorState()) {
        for (const auto& mon : State::monitorState()->monitors()) {
            if (mon && mon->m_name == g_cfgMonitor)
                return mon;
        }
    }

    if (const auto FOCUSED = Compat::focusedMonitor())
        return FOCUSED;

    return g_monitor;
}

// Where the crosshair sits in logical coordinates: the centre of the monitor
// the view is built for. The pointer is pinned around this point, and the
// picking ray is independent of the cursor and comes from the camera.
static Vector2D crosshairLogical() {
    const auto MON = targetMonitor();

    if (!MON)
        return Vector2D{0.0, 0.0};

    return MON->m_position +
           Vector2D{MON->m_size.x * 0.5, MON->m_size.y * 0.5};
}

static void resetMovementKeys() {
    g_keyFwd = g_keyBack = g_keyLeft = g_keyRight = false;
    g_keyUp = g_keyDown = g_keySprint = false;
    g_superHeld = false;
}

// Clears only the camera keys, keeping the gesture modifiers honest -- used
// when switching into window mode while keys are physically held.
static void resetCameraKeys() {
    g_keyFwd = g_keyBack = g_keyLeft = g_keyRight = false;
    g_keyUp = g_keyDown = g_keySprint = false;
}

// True while the 3D view owns the pointer and the keyboard.
static bool ownsInput() {
    // In2D is the fullscreen passthrough: Hyprland owns everything.
    return g_active && g_transition >= 0.9f &&
        g_fsPhase != EFullscreenPhase::In2D;
}

static void clearAimFocus() {
    g_aim.reset();
    g_lastFocusId = 0;
    Compat::clearFocus();
    Compat::clearPointerFocus();
}

// --- layout ghosting --------------------------------------------------------
//
// Every window's box is saved first and only then ghosted: ghosting one window
// triggers a relayout of the ones still attached, which would corrupt boxes
// captured afterwards. Ghosting is what stops tiling from managing windows
// while they are living in 3D space.
static void ghostWindows(const PHLMONITOR& mon) {
    if (!mon)
        return;

    const auto INFOS = Compat::enumerateEligibleWindows(mon);

    if (!g_ghosted) {
        // First pass: save EVERY window before ghosting any of them, since
        // ghosting one window triggers a relayout of the ones still attached,
        // which would corrupt boxes captured afterwards.
        g_layoutSaves.clear();
        g_layoutSaves.reserve(INFOS.size());

        for (const auto& info : INFOS) {
            auto SAVE = Compat::saveWindowLayout(info.window);

            if (SAVE.window)
                g_layoutSaves.push_back(std::move(SAVE));
        }

        for (auto& save : g_layoutSaves)
            Compat::applyWindowGhost(save);

        g_ghosted = true;
        return;
    }

    // Steady state: a window that mapped while the view is open must get the
    // same treatment the same frame it becomes eligible, otherwise it stays a
    // live layout target and every later spawn or close re-tiles the space
    // around it. The live weak reference guards against a new window reusing
    // a closed one's address.
    for (const auto& info : INFOS) {
        bool known = false;

        for (const auto& save : g_layoutSaves) {
            if (save.id == info.id && !save.window.expired()) {
                known = true;
                break;
            }
        }

        if (known)
            continue;

        // A window that appears while the view is open becomes a small
        // floating panel instead of a fullscreen tile -- resized BEFORE the
        // layout save, so the box restored on exit is the same 720x480 the
        // user actually worked with.
        if (info.window) {
            const double CX = mon->m_size.x * 0.5 - kSpawnWidth * 0.5;
            const double CY = mon->m_size.y * 0.5 - kSpawnHeight * 0.5;

            Compat::setWindowBox(
                info.window,
                CBox{CX, CY, kSpawnWidth, kSpawnHeight}
            );
        }

        auto SAVE = Compat::saveWindowLayout(info.window);

        if (!SAVE.window)
            continue;

        Compat::applyWindowGhost(SAVE);
        g_layoutSaves.push_back(std::move(SAVE));
    }
}

static void unghostWindows() {
    if (!g_ghosted)
        return;

    // Force the exact saved geometry back, so leaving 3D never disturbs the
    // user's 2D arrangement. The 3D arrangement is a view, not an edit.
    for (auto& save : g_layoutSaves)
        Compat::restoreWindowLayout(save);

    g_layoutSaves.clear();
    g_ghosted = false;
}

// --- continuous 3D frame pump ----------------------------------------------

static void stopFramePump() {
    if (!g_framePump)
        return;

    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(g_framePump);

    g_framePump.reset();
}

static void startFramePump() {
    if (!g_pEventLoopManager)
        return;

    if (!g_framePump) {
        g_framePump = makeShared<CEventLoopTimer>(
            std::nullopt,
            [](SP<CEventLoopTimer> self, void*) {
                if (!g_active) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                // 2D passthrough: no damage (Hyprland renders by its own
                // damage); the tick still polls the fullscreen state so the
                // exit transition can fire.
                pollFullscreen();

                // Snapshot release deferred from deactivate3D: safe to
                // destroy GL objects here, outside any render pass.
                if (g_captureReleasePending && !g_active) {
                    g_capture.releaseAll();
                    g_captureReleasePending = false;
                }

                if (g_fsPhase != EFullscreenPhase::In2D)
                    damageCurrentMonitor();

                self->updateTimeout(kFramePumpInterval);
            },
            nullptr
        );

        g_pEventLoopManager->addTimer(g_framePump);
    }

    g_framePump->updateTimeout(kFramePumpInterval);
}

// --- live window capture -----------------------------------------------------

static void refreshCaptures(
    const std::vector<Compat::SWindowInfo>& infos,
    const PHLMONITOR& mon
) {
    if (infos.empty())
        return;

    std::vector<std::uintptr_t> keep;
    keep.reserve(infos.size());

    for (const auto& info : infos)
        keep.push_back(info.id);

    g_capture.retainOnly(keep);

    const auto FOCUSED = Compat::focusedWindow();
    const std::uintptr_t FOCUSED_ID = FOCUSED ? Compat::windowId(FOCUSED) : 0;

    for (const auto& info : infos) {
        // Focus-change feedback (the active/inactive opacity fade and the
        // border color tween) is compositor-side -- no client commit happens,
        // so the buffer-change check would freeze both animations mid-way
        // and make the window snap to its final look. While any of them
        // runs, the snapshot refreshes at full rate. The FADE and FULLSCREEN
        // channels belong to the same family: the fullscreen handoff fades
        // every non-FS window through them, and without this the last
        // snapshot keeps the MID-FADE alpha baked in -- static windows
        // stayed semi-transparent until a hover forced a repaint.
        // Alpha channels in flight force full-rate retakes -- but the LAST
        // retake must happen AFTER the animation finishes: while
        // isBeingAnimated() is still true the value can be 0.99, the
        // animation then completes and no further retake runs, leaving the
        // snapshot baked one tick short of the goal (the lingering
        // semi-transparency on static windows). So once a channel was seen
        // in flight, the window keeps forcing retakes for a grace period
        // past the end -- the final retake captures the goal state exactly.
        static std::unordered_map<std::uintptr_t,
            std::chrono::steady_clock::time_point> alphaGrace;

        const auto NOW = std::chrono::steady_clock::now();

        const bool FADING = info.window &&
            (info.window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)
                 ->isBeingAnimated() ||
                info.window->alpha(Desktop::View::WINDOW_ALPHA_FADE)
                 ->isBeingAnimated() ||
                info.window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)
                 ->isBeingAnimated() ||
                info.window->m_borderFadeAnimationProgress->isBeingAnimated());

        if (FADING)
            alphaGrace[info.id] = NOW + std::chrono::milliseconds(200);
        else if (auto IT = alphaGrace.find(info.id);
                 IT != alphaGrace.end() && NOW > IT->second)
            alphaGrace.erase(IT); // expired: keep the map from growing

        const bool ALPHA_GRACE = info.window &&
            alphaGrace.count(info.id) > 0;

        const bool FORCE =
            FADING ||
            ALPHA_GRACE ||
            info.id == g_lastAimedId ||
            (g_world.dragActive() && g_world.draggedId() == info.id) ||
            (g_resize.active && g_resize.id == info.id) ||
            info.id == FOCUSED_ID;

        if (info.isLayer)
            g_capture.makeSnapshotLayer(info.layer, mon, FORCE);
        else
            g_capture.makeSnapshot(info.window, mon, FORCE);
    }

    ++g_captureFrames;
}

// Called from render.pre. Hyprland emits render.pre before beginRender() and
// before the compositor's main render pass, so makeSnapshotFB can safely open
// its own offscreen render here. Nested snapshot renders set g_capturing and
// therefore skip all plugin rendering callbacks.
static void serviceCapture() {
    if (!g_active || g_capturing || !g_pHyprRenderer)
        return;

    // 2D passthrough: the room is dormant, no captures needed.
    if (g_fsPhase == EFullscreenPhase::In2D)
        return;

    const auto MON = targetMonitor();
    if (!MON)
        return;

    g_capturing = true;

    ghostWindows(MON);
    refreshCaptures(Compat::enumerateEligibleWindows(MON), MON);

    g_capturing = false;
}

// Handoff the keyboard to whatever the crosshair is on -- and to nothing else.
// Aiming at empty space drops focus entirely rather than leaving the last
// window typed into.
static void updateAimFocus(float dt) {
    if (g_pointerDown || g_clientButtonDown)
        return;

    const auto& cam = g_scene.camera();

    const World3D::SHit HIT =
        g_world.pick(cam.position, cam.centerRay());

    const std::uintptr_t AIMED = HIT.hit ? HIT.id : 0;
    g_lastAimedId = AIMED;

    const std::uintptr_t FOCUS = g_aim.update(AIMED, dt);

    if (FOCUS == g_lastFocusId)
        return;

    const auto WINDOW =
        FOCUS != 0 ? Compat::findWindowById(FOCUS) : nullptr;

    if (WINDOW) {
        Compat::focusWindow(WINDOW);
        g_lastFocusId = FOCUS;
    }
    else {
        Compat::clearFocus();
        g_lastFocusId = 0;
    }
}

// Raw (pre-smoothstep) fullscreen transition progress. Shared so syncWorld
// and applyFullscreenAnimation agree on the same instant.
static float fsRawProgress() {
    return std::clamp(
        std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                     g_fsPhaseStart).count() /
            kFsAnimDuration,
        0.0f, 1.0f);
}

// The room's uniform window scale, clamped. Shared by syncWorld (quad size)
// and the fullscreen animation (its endpoints must match what the room
// renders, or the transition visibly jumps).
static float configWindowScale() {
    return std::clamp(g_cfgWindowScale, 0.1f, 8.0f);
}

static void syncWorld(const PHLMONITOR& mon, float dt) {
    // Deliberately free of side effects on the layout and the renderer. This
    // runs from render.stage, i.e. between the frame's startRenderPass() and
    // endRender(). Ghosting would trigger a relayout while the pass is half
    // built, and makeSnapshotFB opens and closes its OWN nested pass
    // (beginFullFakeRender -> startRenderPass -> endRender), which rebinds the
    // monitor blur FBs; the compositor's outer endRender() then aborted in
    // CMonitor::useFP16(). Capture and ghosting therefore happen outside the
    // frame -- see serviceCapture().
    const auto INFOS = Compat::enumerateEligibleWindows(mon);

    const float MONW = mon->m_size.x;
    const float MONH = mon->m_size.y;

    const float WIN_SCALE = configWindowScale();

    std::vector<World3D::SEntity> ENTITIES;
    ENTITIES.reserve(INFOS.size());

    for (const auto& info : INFOS) {
        // Track each window's last stable (non-transition) box -- see
        // g_fsStableBoxes. Skipped while the exit re-assert is running: a
        // late Hyprland-side restore could pollute the memory with the
        // monitor-sized box, and the NEXT fullscreen cycle would then
        // restore the giant size (the intermittent bug).
        if (!info.isLayer && info.window &&
            g_fsPhase == EFullscreenPhase::None && g_fsAssertFrames == 0 &&
            info.id != g_fsCurrentId) // a fullscreened window's box is the
                                      // monitor -- transient, never "stable"
            g_fsStableBoxes[info.id] = Compat::currentWindowBox(info.window);

        // Not captured yet means not on screen, and something that is not on
        // screen must not be aimable -- otherwise focus could land on an
        // invisible window.
        if (!g_capture.has(info.id))
            continue;

        const auto* SNAPSHOT = g_capture.get(info.id);

        if (!SNAPSHOT)
            continue;

        // Everything below uses the geometry captured WITH the snapshot, not
        // the live box: updateRealResize writes the real window after
        // render.pre, so the live box can be a frame ahead of the captured
        // pixels. Quad, UV subrect and picking all follow the snapshot box,
        // which keeps the drawn content and the crosshair mapping aligned
        // during resizes -- otherwise the edges smear across the frame delta.
        const CBox& BOX = SNAPSHOT->sampledBox;

        World3D::SEntity entity;
        entity.id = info.id;

        // BOX is where the content sits INSIDE the captured texture (it is
        // the real box unless the window was pulled on-screen for the
        // snapshot), so the UV subrect must follow it.
        entity.logicalLeft   = BOX.x;
        entity.logicalTop    = BOX.y;
        entity.logicalWidth  = BOX.w;
        entity.logicalHeight = BOX.h;

        // Where the client surface sits inside the full decorated box. The
        // quad spans content + borders, so surface-local input is the hit
        // position minus this offset -- borders render but never shift input.
        entity.surfaceOffsetX = SNAPSHOT->surfaceOffset.x;
        entity.surfaceOffsetY = SNAPSHOT->surfaceOffset.y;
        entity.surfaceWidth   = SNAPSHOT->surfaceSize.x;
        entity.surfaceHeight  = SNAPSHOT->surfaceSize.y;

        // Uniform window scale from config, applied to every entity every
        // frame so a runtime change resizes the whole room. The scale is
        // uniform so the captured content never distorts.
        entity.spawnScale = WIN_SCALE;

        // Seed pose: existing entities own their world position and rotation.
        // NEW windows spawn straight in front of the camera at a fixed read
        // distance, facing it.
        if (const auto* EXISTING = g_world.find(info.id)) {
            entity.center = EXISTING->center;
            entity.yaw = EXISTING->yaw;
            entity.pitch = EXISTING->pitch;
            entity.roll = EXISTING->roll;
        }
        else {
            const auto& CAM = g_scene.camera();
            const Vec3 FWD = CAM.forward();

            entity.center = CAM.position + FWD * g_cfgSpawnDistance;

            // Face the camera: with this model's convention (the normal's Y
            // component is -sin(pitch)) the target is the camera's own yaw
            // and pitch.
            entity.yaw   = std::atan2(-FWD.x, -FWD.z);
            entity.pitch = std::asin(std::clamp(FWD.y, -1.0f, 1.0f));
        }

        // The quad size ALWAYS follows the snapshot box (times the spawn
        // scale): the UV subrect and the input mapping are box-relative, so a
        // stale world size would squish the content and shrink the input zone
        // on every resize.
        entity.width  = World3D::toWorld(BOX.w) * entity.spawnScale;
        entity.height = World3D::toWorld(BOX.h) * entity.spawnScale;

        // Fullscreen transition: the animation owns this quad's size, and it
        // MUST be applied here -- the draw list below is built from these
        // values, so overriding later in applyFullscreenAnimation only
        // reaches the render a frame late. That late frame is what leaked
        // the config scale into the monitor-facing handoff and left a seam.
        // The scale glides between the room's config scale and the screen's
        // exact 1:1, following the same eased progress as the box lerp.
        if (g_fsPhase != EFullscreenPhase::None) {
            if (auto FSW = g_fsWindow.lock();
                FSW && Compat::windowId(FSW) == info.id) {
                const float RAWP = fsRawProgress();
                const float p = RAWP * RAWP * (3.0f - 2.0f * RAWP);
                const float ROOM_S = configWindowScale();
                const float S0 =
                    g_fsPhase == EFullscreenPhase::To2D ? ROOM_S : 1.0f;
                const float S1 =
                    g_fsPhase == EFullscreenPhase::To2D ? 1.0f : ROOM_S;
                const float S = S0 + (S1 - S0) * p;

                entity.width  = World3D::toWorld(BOX.w) * S;
                entity.height = World3D::toWorld(BOX.h) * S;
            }
        }

        if (g_resize.active && g_resize.id == info.id) {
            const float DW = entity.width - g_resize.startWorldWidth;
            const float DH = entity.height - g_resize.startWorldHeight;

            entity.center = g_resize.startCenter;
            entity.center += g_world.rightOf(info.id) *
                (static_cast<float>(g_resize.edgeX) * DW * 0.5f);
            entity.center += g_world.upOf(info.id) *
                (static_cast<float>(g_resize.edgeY) * DH * 0.5f);
        }

        ENTITIES.push_back(entity);
    }

    g_world.setEntities(std::move(ENTITIES), false);

    // --- build the draw list from world + snapshot ---
    g_renderWindows.clear();
    g_renderWindows.reserve(g_world.entities().size());

    for (const auto& entity : g_world.entities()) {
        const auto* SNAPSHOT = g_capture.get(entity.id);

        if (!SNAPSHOT || (SNAPSHOT->texID == 0 && !SNAPSHOT->bigTex))
            continue;

        // The captured texture spans texSpan -- the monitor size, or the
        // enlarged virtual monitor when the window did not fit. Dividing by
        // the live monitor size would rescale an oversized window's content
        // and distort it.
        const float SPANW = SNAPSHOT->texSpan.x;
        const float SPANH = SNAPSHOT->texSpan.y;

        if (SPANW <= 0.0f || SPANH <= 0.0f)
            continue;

        GLScene::WindowRender render;
        render.id      = entity.id;
        render.texture = SNAPSHOT->bigTex ? SNAPSHOT->bigTex : SNAPSHOT->texID;

        render.x = entity.center.x;
        render.y = entity.center.y;
        render.z = entity.center.z;
        render.yaw = entity.yaw;
        render.pitch = entity.pitch;
        render.roll = entity.roll;

        render.width  = entity.width;
        render.height = entity.height;

        // The room fade (see g_fsFade): every non-FS window rides it. The
        // fullscreen window must NEVER ride it, in any of its three
        // identities: the one fullscreened right now (g_fsCurrentId), the
        // one being animated back to the room (To3D -- FSW is already null
        // there, but g_fsWindow still holds it), and the one that JUST
        // exited (g_fsLastFSWindow persists past the exit -- without this
        // the ex-FS panel fades in together with everyone else even though
        // it was never hidden and must sit at 100% immediately).
        const auto FS_WIN      = g_fsWindow.lock();
        const auto LAST_FS_WIN = g_fsLastFSWindow.lock();

        const bool IS_FS_WINDOW =
            entity.id == g_fsCurrentId ||
            (FS_WIN && entity.id == Compat::windowId(FS_WIN)) ||
            (LAST_FS_WIN && entity.id == Compat::windowId(LAST_FS_WIN));

        render.alpha = IS_FS_WINDOW ? 1.0f : g_fsFade;

        // The snapshot framebuffer covers the whole monitor, so the window is
        // a subrect of it. Hyprland renders its framebuffers with logical Y
        // (top-left origin, growing down) mapped straight into NDC, so the
        // resulting texture is TOP-DOWN: v = 0 is the monitor's top row and
        // v = logicalY / monH. The quad convention here is v0 at the world
        // bottom of the window, so the content's bottom edge (logicalY =
        // top + height) must feed v0. Flipping with 1 - (...) instead sampled
        // a mirrored band and rendered windows upside down relative to the
        // picked coordinates.
        render.u0 = std::clamp(entity.logicalLeft / SPANW, 0.0f, 1.0f);
        render.u1 = std::clamp(
            (entity.logicalLeft + entity.logicalWidth) / SPANW, 0.0f, 1.0f);
        render.v0 = std::clamp(
            (entity.logicalTop + entity.logicalHeight) / SPANH, 0.0f, 1.0f);
        render.v1 = std::clamp(entity.logicalTop / SPANH, 0.0f, 1.0f);

        g_renderWindows.push_back(render);
    }

    // Aim focus updates freeze during the fullscreen transition: the flying
    // quad sweeps the crosshair across other windows and would thrash focus.
    if (g_fsPhase == EFullscreenPhase::None)
        updateAimFocus(dt);
}

// Window-local (top-left origin, logical px) point for a crosshair hit.
static Vector2D localFromHit(const World3D::SHit& hit) {
    const auto* ENTITY = g_world.find(hit.id);
    if (!ENTITY)
        return {};

    // RayHit::u/v span the full decorated box (content + border) with a
    // top-left origin growing downwards, matching Wayland surface-local
    // coordinates. Do not invert them a second time. The client surface sits
    // inside that box at surfaceOffset, so subtracting it converts to
    // surface-local px: content maps 1:1 and the border ring clamps to the
    // surface edge, so drawing borders never shifts input.
    const float X = std::clamp(hit.u, 0.0f, 1.0f) * ENTITY->logicalWidth -
        ENTITY->surfaceOffsetX;
    const float Y = std::clamp(hit.v, 0.0f, 1.0f) * ENTITY->logicalHeight -
        ENTITY->surfaceOffsetY;

    return {
        std::clamp(X, 0.0f, ENTITY->surfaceWidth),
        std::clamp(Y, 0.0f, ENTITY->surfaceHeight),
    };
}

static World3D::SHit aimHit() {
    const auto& cam = g_scene.camera();

    // Nearest hit whose pixels are actually visible: an invisible layer
    // overlay (transparent quickshell PanelWindow) must not swallow the
    // crosshair -- empty pixels fall through to the surface underneath. This
    // is also what keeps aim/focus from flapping between an overlay and the
    // window it covers.
    for (const auto& HIT : g_world.pickAll(cam.position, cam.centerRay())) {
        const auto* SNAPSHOT = g_capture.get(HIT.id);

        if (SNAPSHOT && SNAPSHOT->alphaValid && !SNAPSHOT->alphaMask.empty()) {
            const int MX = std::min(SNAPSHOT->alphaW - 1,
                static_cast<int>(HIT.u * SNAPSHOT->alphaW));
            const int MY = std::min(SNAPSHOT->alphaH - 1,
                static_cast<int>(HIT.v * SNAPSHOT->alphaH));

            const int A = SNAPSHOT->alphaMask[static_cast<size_t>(MY) * SNAPSHOT->alphaW + MX];

            if (A < 8)
                continue; // transparent pixel of an overlay: next surface
        }

        return HIT;
    }

    return {};
}

// Resolves a hit id into either a window or a layer surface.
struct SHitTarget {
    PHLWINDOW window;
    PHLLS     layer;
};

static SHitTarget targetFromHit(std::uintptr_t id) {
    SHitTarget target;

    if (id == 0)
        return target;

    target.layer = Compat::findLayerById(id);

    if (!target.layer)
        target.window = Compat::findWindowById(id);

    return target;
}

static bool rayPlanePoint(
    const Vec3& origin,
    const Vec3& dir,
    const Vec3& planePoint,
    const Vec3& planeNormal,
    Vec3& out
) {
    const float denom = dot(dir, planeNormal);
    if (!std::isfinite(denom) || std::fabs(denom) < 0.000001f)
        return false;

    const float t = dot(planePoint - origin, planeNormal) / denom;
    if (!std::isfinite(t) || t <= 0.0f)
        return false;

    out = origin + dir * t;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

static Vector2D worldPointToGlobalPx(
    const PHLMONITOR& mon,
    const Vec3& point
) {
    (void)mon;

    const Vec3 LOCAL = g_world.localPoint(g_resize.id, point);
    const CBox CURRENT = Compat::currentWindowBox(g_resize.window);

    // The quad spans toWorld(box) * spawnScale world units while carrying
    // box pixels of content, so the px density on the quad is the room's
    // base density divided by the scale.
    const auto* RESIZE_ENTITY = g_world.find(g_resize.id);
    const double scale =
        RESIZE_ENTITY ? RESIZE_ENTITY->spawnScale : 1.0f;

    const double currentCenterX = CURRENT.x + CURRENT.w * 0.5;
    const double currentCenterY = CURRENT.y + CURRENT.h * 0.5;

    return {
        currentCenterX + static_cast<double>(LOCAL.x) *
            World3D::LOGICAL_PX_PER_UNIT / scale,
        currentCenterY - static_cast<double>(LOCAL.y) *
            World3D::LOGICAL_PX_PER_UNIT / scale,
    };
}

static CBox resizeBoxFromAim(const Vec3& point, const PHLMONITOR& mon) {
    // The grabbed edge moves by the crosshair's travel since the grab, not
    // to its absolute position: the box starts identical to the window, so
    // starting a resize never snaps the nearest corner under the crosshair.
    const Vector2D PX = worldPointToGlobalPx(mon, point);
    const double DX = PX.x - g_resize.grabPx.x;
    const double DY = PX.y - g_resize.grabPx.y;

    const CBox& start = g_resize.startBox;
    const double RIGHT = start.x + start.w;
    const double BOTTOM = start.y + start.h;

    CBox out = start;

    if (g_resize.edgeX > 0) {
        out.x = start.x;
        out.w = start.w + DX;
    } else {
        out.x = start.x + DX;
        out.w = start.w - DX;
    }

    if (g_resize.edgeY > 0) {
        out.y = start.y + DY;
        out.h = start.h - DY;
    } else {
        out.y = start.y;
        out.h = start.h + DY;
    }

    const auto MIN = g_resize.window->minSize().value_or(Vector2D{1.0, 1.0});
    const auto MAX = g_resize.window->maxSize().value_or(Vector2D{INFINITY, INFINITY});

    out.w = std::clamp(out.w, MIN.x, MAX.x);
    out.h = std::clamp(out.h, MIN.y, MAX.y);

    if (g_resize.edgeX > 0)
        out.x = start.x;
    else
        out.x = RIGHT - out.w;

    if (g_resize.edgeY > 0)
        out.y = BOTTOM - out.h;
    else
        out.y = start.y;

    return out;
}

// --- fullscreen passthrough -------------------------------------------------

static float wrapPi(float a) {
    while (a > 3.14159265f)
        a -= 6.28318530f;
    while (a < -3.14159265f)
        a += 6.28318530f;
    return a;
}

// To2D: capture the room pose, force the real window to the monitor box and
// aim the animation at a quad that exactly covers the frozen camera frustum.
static void startTo2D(const PHLWINDOW& window, bool captureRestoreBox) {
    const auto MON = targetMonitor();

    if (!MON)
        return;

    const auto ID = Compat::windowId(window);

    if (!g_world.find(ID))
        return; // not in the room: nothing to animate

    g_fsWindow = window;

    // The floating box: the POSITION from the stable memory (where the
    // window lived), the SIZE from Hyprland's own remembered floating size.
    // When the FS was engaged, Hyprland remembered the pre-fullscreen size
    // there (setTargetFullscreenModeInternal does it BEFORE stretching the
    // window to the monitor) -- exactly the size the 2D FS-exit restores.
    // The stable memory is the fallback (e.g. a tiled window has no
    // floating history: its tile size is the honest original).
    if (captureRestoreBox) {
        const auto IT = g_fsStableBoxes.find(ID);
        CBox RESTORE = IT != g_fsStableBoxes.end() ?
            IT->second : Compat::currentWindowBox(window);

        if (window->m_target) {
            const auto LF = window->m_target->lastFloatingSize();

            if (LF.x > 5.0 && LF.y > 5.0)
                RESTORE = CBox{RESTORE.x, RESTORE.y, LF.x, LF.y};
        }

        g_fsRestoreBox = RESTORE;
    }

    // The fullscreen TARGET box (the animation moves the real box there
    // gradually -- see applyFullscreenAnimation).
    g_fsMonitorBox = CBox{
        MON->m_position.x, MON->m_position.y, MON->m_size.x, MON->m_size.y};

    // Pin Hyprland's remembered floating size to the captured PRE-FS size:
    // the transition's per-frame setWindowBox calls would otherwise
    // re-member intermediate (up to monitor-sized) values, and Hyprland's
    // own FS-exit applies that memory.
    if (window->m_target)
        window->m_target->rememberFloatingSize(
            {g_fsRestoreBox.w, g_fsRestoreBox.h});

    // The screen-covering distance derives from the vertical FOV and the
    // monitor's NATURAL world size (logical px / 100):
    // d = (worldHeight / 2) / tan(vfov / 2). At that distance a
    // monitor-sized quad subtends exactly the full frustum.
    const float MON_H_WORLD =
        static_cast<float>(MON->m_size.y) / World3D::LOGICAL_PX_PER_UNIT;
    g_fsDistance = (MON_H_WORLD * 0.5f) /
        std::tan(kFovDeg * 3.14159265f / 360.0f);

    if (const auto* E = g_world.find(ID)) {
        g_fsStartCenter = E->center;
        g_fsStartYaw    = E->yaw;
        g_fsStartPitch  = E->pitch;
        g_fsStartRoll   = wrapPi(E->roll);
        g_fsSavedRoll   = wrapPi(E->roll); // restored when the FS exits

    }

    const auto& CAM = g_scene.camera();
    const Vec3 FWD = CAM.forward();

    g_fsEndCenter = CAM.position + FWD * g_fsDistance;

    // Face the FROZEN camera: with the model convention (normal Y is
    // -sin(pitch)) facing the camera means yaw = -camYaw, pitch = +camPitch.
    // The content lands unmirrored: the quad's local +X maps exactly onto
    // the camera's right vector.
    g_fsEndYaw    = -CAM.yaw;
    g_fsEndPitch  = CAM.pitch;

    resetPointerGesture(); // no gestures on the animating window
    g_input.reset();       // no look jump when look resumes

    g_fsPhase        = EFullscreenPhase::To2D;
    g_fsPhaseStart   = std::chrono::steady_clock::now();
    g_fsWasOn        = true;
    g_fsAssertFrames = 0;
    damageCurrentMonitor();
}

// In2D -> To3D: restore the real floating box, capture input, hide the
// cursor; the animation runs the quad back to its room pose.
static void startTo3D() {
    if (auto W = g_fsWindow.lock()) {
        // The To3D start is the window's CURRENT pose: the tracked fullscreen
        // pose after a normal exit, or wherever the aborted To2D got to. The
        // real floating box is restored when the animation COMPLETES (restoring
        // it here would make the quad show magnified small-window content).
        if (auto* E = g_world.find(Compat::windowId(W))) {
            g_fsEndCenter = E->center;
            g_fsEndYaw    = E->yaw;
            g_fsEndPitch  = E->pitch;
            g_fsRollAtStart = wrapPi(E->roll);
        }

        if (Compat::setCursorHidden(true))
            Pointer::mgr()->resetCursorImage();

        Compat::setPointerCapture(g_hookInstalled);

        g_fsPhase      = EFullscreenPhase::To3D;
        g_fsPhaseStart = std::chrono::steady_clock::now();
        g_input.reset();
        damageCurrentMonitor();
    }
}

// None -> To2D when a fullscreen window appears; In2D -> To3D when it goes.
// Hyprland fades windows with per-window alpha animations that only make
// progress while the window is being damaged: during the fullscreen
// handoff the non-FS windows fade out (their FADE/FULLSCREEN alpha channels
// animate toward 0), and on the way back they fade in; focus changes fade
// the ACTIVE channel the same way. Mid-fade the compositor stops damaging
// them -- the animation freezes a couple of frames in, and the room shows
// windows stuck at partial alpha until an aim hover forces a frame (the
// semi-transparent-after-FS bug). Damage every window whose alpha channels
// are still in flight; the damage drives the fade to completion and the
// loop stops on its own.
static void damageWindowsWithLiveAlpha(const PHLMONITOR& mon) {
    if (!Desktop::windowState() || !mon || !mon->m_activeWorkspace)
        return;

    for (const auto& W : Desktop::windowState()->windows()) {
        if (!W || W->m_workspace != mon->m_activeWorkspace)
            continue;

        const auto IN_FLIGHT = [&](Desktop::View::eWindowAlpha channel) {
            return W->alphaValue(channel) != W->alphaGoal(channel);
        };

        if (IN_FLIGHT(Desktop::View::WINDOW_ALPHA_FADE) ||
            IN_FLIGHT(Desktop::View::WINDOW_ALPHA_ACTIVE) ||
            IN_FLIGHT(Desktop::View::WINDOW_ALPHA_FULLSCREEN))
            g_pHyprRenderer->damageWindow(W);
    }
}

static void pollFullscreen() {
    const auto MON = targetMonitor();

    if (!MON)
        return;

    // The alpha keepalive rides the pump (8 ms) rather than the render pass,
    // so the fade keeps ticking even when a frame is slow.
    damageWindowsWithLiveAlpha(MON);

    const auto FSW = Fullscreen::controller()->getFullscreenWindow(MON);

    g_fsCurrentId = FSW ? Compat::windowId(FSW) : 0;

    // Our own room fade: while a fullscreen exists (or a transition runs)
    // the other windows glide to invisible; when it is gone they glide
    // back. Deterministic wall-clock ticks -- never dependent on Hyprland's
    // animation engine or on snapshot retakes.
    constexpr float FADE_DURATION = 0.25f;

    const auto FADE_NOW = std::chrono::steady_clock::now();
    const float FADE_DT = g_fsFadeLast.time_since_epoch().count() == 0 ?
        0.0f :
        std::chrono::duration<float>(FADE_NOW - g_fsFadeLast).count();
    g_fsFadeLast = FADE_NOW;

    const float FADE_TARGET =
        (FSW || g_fsPhase != EFullscreenPhase::None) ? 0.0f : 1.0f;

    if (FADE_DT > 0.0f && g_fsFade != FADE_TARGET) {
        const float STEP = std::min(FADE_DT / FADE_DURATION, 1.0f);
        g_fsFade = FADE_TARGET > g_fsFade ?
            std::min(g_fsFade + STEP, FADE_TARGET) :
            std::max(g_fsFade - STEP, FADE_TARGET);
    }

    if (g_fsPhase == EFullscreenPhase::None) {
        if (FSW && !g_fsWasOn && ownsInput())
            startTo2D(FSW);
        else {
            g_fsWasOn = FSW != nullptr;

            // A fullscreen EXITED while the room is plain 3D (no transition
            // was running -- e.g. the FS predated the 3D entry): make the
            // window a spawn-sized floating panel AND hold it with the same
            // condition-driven assert the To3D completion uses -- Hyprland's
            // floating layout re-applies ITS remembered size (possibly
            // monitor-sized from an old cycle), and a one-shot set loses to
            // it.
            if (!FSW && g_fsWasOn) {
                if (auto OLDW = g_fsLastFSWindow.lock()) {
                    const auto CUR = Compat::currentWindowBox(OLDW);

                    g_fsAssertBox    = CBox{CUR.x, CUR.y, kSpawnWidth,
                                            kSpawnHeight};
                    g_fsAssertFrames = 1;
                    g_fsStableCount  = 0;

                    Compat::setWindowBox(OLDW, g_fsAssertBox);
                }
            }
        }

        g_fsLastFSWindow = FSW;
    } else if (g_fsPhase == EFullscreenPhase::In2D) {
        // Exit passthrough when OUR window is no longer the fullscreen one
        // (toggled off, or fullscreen moved to another window entirely).
        const auto W = g_fsWindow.lock();

        if (!FSW || (W && FSW != W))
            startTo3D();
        else
            g_fsWasOn = true;
    } else if (g_fsPhase == EFullscreenPhase::To2D) {
        // Fullscreen was toggled off MID-ANIMATION: hand back to 3D from the
        // current animated pose instead of finishing onto a stale screen.
        if (!FSW)
            startTo3D();
    } else if (g_fsPhase == EFullscreenPhase::To3D) {
        g_fsWasOn = FSW != nullptr; // remembered for after the animation
    }
}

// To2D/To3D: pose the fullscreening window between its room pose and the
// screen-covering pose (smoothstep). Runs AFTER syncWorld, overriding the
// per-frame reseed.
static void applyFullscreenAnimation() {
    if (g_fsPhase != EFullscreenPhase::To2D && g_fsPhase != EFullscreenPhase::To3D)
        return;

    const auto W = g_fsWindow.lock();
    auto* E = W ? g_world.find(Compat::windowId(W)) : nullptr;

    if (!E) {
        // the window closed mid-transition: land the phase
        g_fsPhase  = EFullscreenPhase::None;
        g_fsWindow = {};
        return;
    }

    const float RAWP = fsRawProgress();
    g_fsRawP = RAWP; // feeds the composite alpha below
    float p = RAWP * RAWP * (3.0f - 2.0f * RAWP); // smoothstep

    // To2D: the SCREEN pose tracks the LIVE camera -- looking around during
    // the transition keeps the quad converging onto the current view, so the
    // handoff never pops.
    if (g_fsPhase == EFullscreenPhase::To2D) {
        const auto& CAM = g_scene.camera();
        const Vec3 FWD = CAM.forward();

        g_fsEndCenter = CAM.position + FWD * g_fsDistance;
        g_fsEndYaw    = -CAM.yaw;
        g_fsEndPitch  = CAM.pitch;
    }

    // To2D: room -> screen; To3D: screen -> room
    const auto  A = g_fsPhase == EFullscreenPhase::To2D;
    const Vec3  C0 = A ? g_fsStartCenter : g_fsEndCenter;
    const Vec3  C1 = A ? g_fsEndCenter : g_fsStartCenter;
    const float Y0 = A ? g_fsStartYaw : g_fsEndYaw;
    const float Y1 = A ? g_fsEndYaw : g_fsStartYaw;
    const float P0 = A ? g_fsStartPitch : g_fsEndPitch;
    const float P1 = A ? g_fsEndPitch : g_fsStartPitch;

    E->center = C0 + (C1 - C0) * p;
    E->yaw    = Y0 + wrapPi(Y1 - Y0) * p;
    E->pitch  = P0 + (P1 - P0) * p;

    // The roll fades out on the way to 2D (the compositor renders windows
    // unrolled) and fades back to the saved value on the way to the room.
    if (A)
        E->roll = g_fsSavedRoll * (1.0f - p);
    else
        E->roll = g_fsRollAtStart + (g_fsSavedRoll - g_fsRollAtStart) * p;

    // The REAL box animates between the floating box and the fullscreen box.
    // The client's buffer is stretched to this box by the compositor, so the
    // content scale inside the quad stays constant through the whole
    // transition. On the way back the box lands at the SPAWN size (960x540):
    // the post-FS size is deterministic and never inherits garbage from
    // earlier broken cycles.
    const CBox B0 = A ? g_fsRestoreBox : g_fsMonitorBox;
    const CBox B1 = A ? g_fsMonitorBox : g_fsRestoreBox;

    const CBox BOX{
        B0.x + (B1.x - B0.x) * p,
        B0.y + (B1.y - B0.y) * p,
        B0.w + (B1.w - B0.w) * p,
        B0.h + (B1.h - B0.h) * p,
    };

    if (auto W = g_fsWindow.lock())
        Compat::setWindowBox(W, BOX);

    // The quad follows the animated box at the room's pixel density. The
    // fullscreen quad maps 1:1 onto the monitor by definition, so the room's
    // window scale is suspended for its duration (syncWorld re-applies it
    // once the window lands back in the room). The scale itself is lerped
    // across the animation so the endpoints match their neighbours: the room
    // side is the window at config scale, the monitor side is exact 1:1 --
    // otherwise the quad visibly jumps at the handoff frames.
    E->spawnScale = 1.0f;

    const float ROOM_SCALE = configWindowScale();
    const float S0 = A ? ROOM_SCALE : 1.0f;
    const float S1 = A ? 1.0f : ROOM_SCALE;
    const float S = S0 + (S1 - S0) * p; // p is the eased progress

    E->width  = World3D::toWorld(BOX.w) * S;
    E->height = World3D::toWorld(BOX.h) * S;

    // The roll decays to zero: the 2D fullscreen view has no roll, and the
    // quad must land matching what the compositor will render.

    // Composite alpha: minimal fades at the handoffs (a couple of frames).
    // The snapshot and the live 2D render can be a frame apart, and a long
    // fade makes that desync visible; a 2-frame crossfade hides it.
    g_fsAlpha = A ?
        std::clamp(1.0f - (g_fsRawP - 0.96f) / 0.04f, 0.0f, 1.0f) :
        std::clamp(g_fsRawP / 0.04f, 0.0f, 1.0f);

    // Phase completion:
    //   To2D done -> hand the screen to the 2D compositor (In2D): input is
    //   released, the cursor reappears, the 3D scene stops rendering (the
    //   onRenderStage gate) and the pump polls for the fullscreen exit.
    //   To3D done -> restore the floating box (the client re-renders at its
    //   room size) and return to the plain 3D room.
    if (p >= 1.0f) {
        if (g_fsPhase == EFullscreenPhase::To2D) {
            g_fsPhase = EFullscreenPhase::In2D;

            Compat::setPointerCapture(false);

            // Our transition's per-frame setWindowBox calls polluted
            // Hyprland's remembered floating size with intermediate (up to
            // monitor-sized) values. Re-pin the captured PRE-FS size.
            if (auto W2 = g_fsWindow.lock())
                W2->m_target->rememberFloatingSize(
                    {g_fsRestoreBox.w, g_fsRestoreBox.h});

            if (Compat::setCursorHidden(false) && g_pHyprRenderer)
                g_pHyprRenderer->setCursorFromName("default", true);
        } else {
            // Rapid exit+reenter: the FS state may already be ON again for
            // this window -- fly it back to the screen (real 2D) instead of
            // dropping it into the room small while fullscreened.
            const auto MON = targetMonitor();
            const auto W2 = g_fsWindow.lock();

            if (W2 && MON &&
                Fullscreen::controller()->getFullscreenWindow(MON) == W2) {
                g_fsPhaseStart = std::chrono::steady_clock::now();
                startTo2D(W2, /*captureRestoreBox*/ false);
            } else {
                if (W2) {
                    // The original position AND the pre-fullscreen size:
                    // Hyprland remembered the size when the FS was engaged,
                    // the box lerp above already animated the real box there.
                    Compat::setWindowBox(W2, g_fsRestoreBox);

                    // Hyprland's own fullscreen-exit restore animates the
                    // window toward ITS remembered floating size -- which our
                    // transition kept updating -- and can retarget the box
                    // AFTER this point. Re-assert the restore box for a few
                    // frames so the final size is ours.
                    // The SAME box the quad just landed at (the spawn
                    // size) -- asserting the raw g_fsRestoreBox here made
                    // our own guard resize the window back to the polluted
                    // monitor size right after the landing.
                    g_fsAssertBox    = g_fsRestoreBox;
                    g_fsAssertFrames = 1;    // condition-driven: runs until stable
                    g_fsStableCount  = 0;

                    // The FS fade dimmed EVERY other workspace window to
                    // alpha 0 when the fullscreen started; our transition can
                    // leave the OUT fade mid-flight. Reset the channel
                    // explicitly, or windows stay barely visible.
                    if (MON && MON->m_activeWorkspace) {
                        for (auto const& W3 :
                             Desktop::windowState()->windows()) {
                            if (W3 && W3->m_workspace ==
                                          MON->m_activeWorkspace &&
                                !W3->m_pinned)
                                *W3->alpha(
                                     Desktop::View::WINDOW_ALPHA_FULLSCREEN) =
                                    1.F;
                        }
                    }
                }

                g_fsPhase = EFullscreenPhase::None;
            }
        }
    }
}

// Per-frame roll gesture: rotate the window around its normal by the signed
// angle the crosshair swept around the window center (measured in world
// space around the normal -- invariant to the roll itself, so the gesture
// never feeds back into its own measurement).
static void updateWheelRoll() {
    if (!s_wheelRot.active)
        return;

    const auto HIT = aimHit();

    if (!HIT.hit || HIT.id != s_wheelRot.id) {
        s_wheelRot.active = false; // crosshair left the window: end it
        return;
    }

    auto* ENTITY = g_world.find(s_wheelRot.id);

    if (!ENTITY) {
        s_wheelRot.active = false;
        return;
    }

    const Vec3 V = HIT.point - s_wheelRot.center;
    const float ANGLE = std::atan2(
        dot(cross(s_wheelRot.reference, V), s_wheelRot.normal),
        dot(s_wheelRot.reference, V));

    // Wrapped to [-pi, pi]: the angle is 2pi-periodic, so the visual is
    // identical, but the fullscreen transition interpolates this value
    // linearly -- an unwrapped 700-degree roll would unwind like a top.
    ENTITY->roll = wrapPi(s_wheelRot.startRoll + ANGLE);
}

static void updateRealResize() {
    if (!g_resize.active || !g_resize.window)
        return;

    const auto MON = targetMonitor();
    if (!MON)
        return;

    const auto* ENTITY = g_world.find(g_resize.id);
    if (!ENTITY)
        return;

    // The edge being dragged is part of the real, resized window, so the
    // interaction plane follows the window's current centre every frame.
    g_resize.planePoint = ENTITY->center;
    g_resize.planeNormal = g_world.normalOf(g_resize.id);

    Vec3 point;
    if (!rayPlanePoint(
            g_scene.camera().position,
            g_scene.camera().centerRay(),
            g_resize.planePoint,
            g_resize.planeNormal,
            point))
        return;

    const CBox BOX = resizeBoxFromAim(point, MON);
    Compat::setWindowBox(g_resize.window, BOX);
}

static void resetPointerGesture() {
    g_pointerDown = false;
    g_pointerGesture = EPointerGesture::None;
    g_pointerButton = 0;
    g_resize = {};
    s_wheelRot.active = false;
}

static void finishClientButton(uint32_t timeMs) {
    if (!g_clientButtonDown)
        return;

    if (g_clientButtonLayer) {
        Compat::deliverClick(
            g_clientButtonLayer,
            g_clientButtonLocal,
            g_clientButton,
            false,
            timeMs
        );
    } else if (g_clientButtonWindow) {
        Compat::deliverClick(
            g_clientButtonWindow,
            g_clientButtonLocal,
            g_clientButton,
            false,
            timeMs
        );
    }

    g_clientButtonWindow = nullptr;
    g_clientButtonLayer  = nullptr;
    g_clientButton = 0;
    g_clientButtonDown = false;
}

static void forwardPointerToAim(uint32_t timeMs) {
    if (!ownsInput() || g_pointerDown)
        return;

    const auto HIT = aimHit();
    const auto TARGET = HIT.hit ? targetFromHit(HIT.id) : SHitTarget{};

    if (g_clientButtonDown && (g_clientButtonWindow || g_clientButtonLayer)) {
        // A pressed client keeps pointer ownership while the camera turns. If
        // the crosshair leaves that surface, hold the last local point instead
        // of teleporting pointer focus into a different client mid-drag.
        const bool SAME =
            (g_clientButtonWindow && TARGET.window == g_clientButtonWindow) ||
            (g_clientButtonLayer && TARGET.layer == g_clientButtonLayer);

        if (SAME && HIT.hit)
            g_clientButtonLocal = localFromHit(HIT);

        if (g_clientButtonLayer)
            Compat::deliverMotion(g_clientButtonLayer, g_clientButtonLocal, timeMs);
        else if (g_clientButtonWindow)
            Compat::deliverMotion(g_clientButtonWindow, g_clientButtonLocal, timeMs);
        return;
    }

    if (TARGET.layer) {
        Compat::deliverMotion(TARGET.layer, localFromHit(HIT), timeMs);
        return;
    }

    if (!TARGET.window) {
        Compat::clearPointerFocus();
        return;
    }

    Compat::deliverMotion(TARGET.window, localFromHit(HIT), timeMs);
}

// Absolute-position fallback for installations where the PointerManager hook
// is unavailable. When the hook works, it feeds onPointerMotion directly and
// this event only keeps the baseline warm.
static bool onPointerMotion(double dx, double dy) {
    if (!ownsInput())
        return false;

    g_input.addMotion(dx, dy);
    damageCurrentMonitor();
    return true;
}

static void onRenderPre(PHLMONITOR mon) {
    if (g_capturing)
        return;

    const auto TARGET = targetMonitor();
    if (TARGET && mon != TARGET) {
        g_currentRenderMon = nullptr;
        return;
    }

    g_currentRenderMon = mon;

    if (!g_active) {
        g_monitor = mon;
        return;
    }

    g_monitor = mon;
    serviceCapture();
}


// --- lifecycle --------------------------------------------------------------

static void deactivate3D() {
    finishClientButton(0);
    resetPointerGesture();

    g_active = false;
    stopFramePump();

    // Fullscreen passthrough state: back to plain 3D-off. Restore the real
    // box if a transition was mid-flight (the window would otherwise stay
    // monitor-sized). The SIZE is the spawn size -- never the possibly
    // polluted restore box.
    if (auto W = g_fsWindow.lock())
        Compat::setWindowBox(
            W,
            CBox{g_fsRestoreBox.x, g_fsRestoreBox.y, kSpawnWidth,
                 kSpawnHeight});

    g_fsPhase        = EFullscreenPhase::None;
    g_fsWindow       = {};
    g_fsWasOn        = false;
    g_fsAlpha        = 1.0f;
    g_fsFade         = 1.0f;
    g_fsAssertFrames = 0;

    // Restore the host cursor before anything else touches focus: removing
    // pointer focus makes the client re-apply its own cursor image on the
    // next pointer enter, and the default shape shows immediately.
    if (Compat::setCursorHidden(false) && g_pHyprRenderer)
        g_pHyprRenderer->setCursorFromName("default", true);

    clearAimFocus();
    g_world.clear();
    g_renderWindows.clear();
    g_input.reset();
    resetMovementKeys();

    g_capture.releaseAll();
    Compat::setPointerCapture(false);

    unghostWindows();
    g_renderedOnce = false;
}

static void enter3D() {
    // Without this the render stage bails out immediately and the toggle does
    // nothing at all.
    g_active = true;
    startFramePump();

    // Hide the host cursor: the 3D view aims with its own crosshair. Client
    // cursor updates are gated by the hooks; the client re-applies its own
    // image automatically when pointer focus re-enters on exit.
    if (!Compat::setCursorHidden(true))
        notify(
            "[hypr3d] cursor hooks unavailable: the frozen cursor will stay visible",
            CHyprColor{1.0f, 0.6f, 0.2f, 1.0f}
        );
    else
        Pointer::mgr()->resetCursorImage();

    // Focus is absent by default: it only ever appears when the crosshair is
    // on a window.
    clearAimFocus();

    finishClientButton(0);
    resetPointerGesture();

    g_scene.reset();
    g_input.reset();
    g_world.clear();
    g_renderWindows.clear();

    // Player spawn point (config player_spawn): the coordinates are the
    // player's FEET, so spawning at 0,0,0 stands on the grid platform at
    // world zero instead of falling through it. Eyes ride kEyeHeight above.
    {
        auto& CAM = g_scene.camera();
        CAM.position = Vec3{
            g_playerSpawn.x,
            g_playerSpawn.y + Camera::kEyeHeight,
            g_playerSpawn.z,
        };

        // Camera forward is {sin yaw, ., -cos yaw}: looking at the origin
        // from (x, z) means yaw = atan2(-x, z).
        CAM.yaw = std::atan2(-g_playerSpawn.x, g_playerSpawn.z);
    }

    g_capture.releaseAll();

    resetMovementKeys();

    g_grounded    = false;
    g_verticalVel = 0.0f;

    g_keyboardMode = EKeyboardMode::Space;
    g_altHeld      = false;
    s_zoomId       = 0;

    // A fullscreen window that already exists: it joins the room as a
    // standard spawn-sized floating panel (a fullscreened/tiled box would
    // otherwise enter the room monitor-sized AND pollute the stable-box
    // memory). The passthrough still triggers on the fullscreen EVENT.
    if (const auto MON = targetMonitor()) {
        if (const auto FSW =
                Fullscreen::controller()->getFullscreenWindow(MON)) {
            Compat::setWindowBox(
                FSW,
                CBox{MON->m_size.x * 0.5 - kSpawnWidth * 0.5,
                     MON->m_size.y * 0.5 - kSpawnHeight * 0.5, kSpawnWidth,
                     kSpawnHeight});

            g_fsLastFSWindow = FSW;
        }

        g_fsWasOn =
            Fullscreen::controller()->getFullscreenWindow(MON) != nullptr;
    } else
        g_fsWasOn = false;

    g_fsStableBoxes.clear();

    g_renderedOnce = false;
    g_captureFrames = 0;
    g_reportedFramebufferError = false;
    g_reportedRenderError = false;

    // The first full live capture is performed from render.pre, where
    // Hyprland has not entered its main render pass yet.
}

static void toggle3D() {
    g_transitionTarget =
        g_transitionTarget > 0.5f ? 0.0f : 1.0f;

    if (g_transitionTarget > 0.5f)
        enter3D();

    damageCurrentMonitor();

    notify(
        "[hypr3d] toggle",
        CHyprColor{0.2f, 0.8f, 0.4f, 1.0f}
    );
}

static void open3D() {
    g_transitionTarget = 1.0f;
    enter3D();
    damageCurrentMonitor();

    notify(
        "[hypr3d] open",
        CHyprColor{0.2f, 0.8f, 0.4f, 1.0f}
    );
}

static void close3D() {
    g_transitionTarget = 0.0f;
    damageCurrentMonitor();

    notify(
        "[hypr3d] close",
        CHyprColor{0.2f, 0.8f, 0.4f, 1.0f}
    );
}

static float updateTransition() {
    const auto now = std::chrono::steady_clock::now();

    const float dt =
        std::chrono::duration<float>(now - g_lastTick).count();

    g_lastTick = now;

    constexpr float duration = 0.55f;
    constexpr float speed = 1.0f / duration;

    if (g_transition < g_transitionTarget)
        g_transition = std::min(g_transition + dt * speed, 1.0f);
    else if (g_transition > g_transitionTarget)
        g_transition = std::max(g_transition - dt * speed, 0.0f);

    return std::clamp(dt, 0.0f, 0.1f);
}

// --- frame ------------------------------------------------------------------

// World-space glide velocity for the WASD inertia (units/second). Lives
// across frames so releasing the keys coasts down instead of cutting dead.
static Vec3 s_moveVel{};

static void applyCameraMovement(float dt) {
    float FORWARD  = (g_keyFwd ? 1.f : 0.f) - (g_keyBack ? 1.f : 0.f);
    float STRAFE   = (g_keyRight ? 1.f : 0.f) - (g_keyLeft ? 1.f : 0.f);
    float VERTICAL = (g_keyUp ? 1.f : 0.f) - (g_keyDown ? 1.f : 0.f);

    auto& CAM = g_scene.camera();

    // Walking mode: Shift does nothing (gravity owns vertical), Space is a
    // jump impulse handled in the key handler.
    if (!g_playerFlying)
        VERTICAL = 0.f;

    // Diagonals must not be faster than a straight run: W+D used to add the
    // two key speeds into a sqrt(2)x diagonal.
    {
        const float LEN = std::sqrt(FORWARD * FORWARD + STRAFE * STRAFE);
        if (LEN > 1.0f) {
            FORWARD /= LEN;
            STRAFE /= LEN;
        }
    }

    const bool MOVING = FORWARD != 0.f || STRAFE != 0.f || VERTICAL != 0.f;

    if (!g_playerFlying) {
        // --- walking: gravity owns the vertical axis, jump rides it ---
        constexpr float GRAVITY  = 14.0f;
        constexpr float TERMINAL = 40.0f;

        g_verticalVel -= GRAVITY * dt;
        if (g_verticalVel < -TERMINAL)
            g_verticalVel = -TERMINAL;

        const float    SPEED = CAM.moveSpeed * (g_keySprint ? 2.5f : 1.0f);
        const Vec3     TARGET =
            CAM.flatForward() * (FORWARD * SPEED) +
            CAM.right() * (STRAFE * SPEED);

        const float tau = g_cfgMoveInertia;
        if (tau > 0.0f && dt > 0.0f)
            s_moveVel += (TARGET - s_moveVel) * (1.0f - std::exp(-dt / tau));
        else
            s_moveVel = TARGET;

        const bool MOVING_H = FORWARD != 0.f || STRAFE != 0.f;
        if (!MOVING_H && std::fabs(s_moveVel.x) + std::fabs(s_moveVel.z) < 0.01f)
            s_moveVel = Vec3{0.f, 0.f, s_moveVel.y};

        const Vec3 STEP = s_moveVel + Vec3{0.f, g_verticalVel, 0.f};
        CAM.displace(STEP, dt);
        damageCurrentMonitor();
        return;
    }

    const float tau = g_cfgMoveInertia;

    if (tau <= 0.0f || dt <= 0.0f) {
        s_moveVel = {};

        if (!MOVING)
            return;

        const float BASE = CAM.moveSpeed;
        CAM.moveSpeed = BASE * (g_keySprint ? 2.5f : 1.0f);
        CAM.move(FORWARD, STRAFE, VERTICAL, dt);
        CAM.moveSpeed = BASE;

        damageCurrentMonitor();
        return;
    }

    // Glide toward the key-driven velocity with tau as the time constant;
    // with the keys released the target is zero, so motion decays smoothly.
    const float SPEED =
        CAM.moveSpeed * (g_keySprint ? 2.5f : 1.0f) * (MOVING ? 1.0f : 0.0f);
    const Vec3 TARGET =
        CAM.flatForward() * (FORWARD * SPEED) +
        CAM.right() * (STRAFE * SPEED) +
        Vec3{0.f, 1.f, 0.f} * (VERTICAL * SPEED);

    s_moveVel += (TARGET - s_moveVel) *
        (1.0f - std::exp(-dt / tau));

    // Snap the decay tail off once it is far below a frame of movement, so
    // the glide always terminates.
    if (!MOVING &&
        std::fabs(s_moveVel.x) + std::fabs(s_moveVel.y) +
            std::fabs(s_moveVel.z) < 0.01f)
        s_moveVel = {};

    if (s_moveVel.x != 0.f || s_moveVel.y != 0.f || s_moveVel.z != 0.f) {
        CAM.displace(s_moveVel, dt);
        damageCurrentMonitor();
    }
}

static void update3D(float dt) {
    // Release capture unconditionally when the view no longer owns input, so
    // a disappearing monitor or a closing transition can never strand the
    // pointer in captured mode.
    Compat::setPointerCapture(g_hookInstalled && ownsInput());

    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;

    // Look/movement stay live during the fullscreen transition: the To2D end
    // pose tracks the camera, so the quad keeps converging onto the view.
    // Plugin settings come from lua (hl.plugin.hypr3d.config) and are
    // already clamped at set time.
    g_input.setLookSmoothing(g_cfgLookInertia);
    g_scene.camera().moveSpeed = g_cfgMoveSpeed;
    g_input.setSensitivity(g_cfgSensitivity);

    // Map: the scene owns the file/GL side; collision rebuilds its BVH
    // whenever the map (re)loaded.
    g_scene.setMapPath(g_cfgMapPath);
    g_scene.setMapTransform(g_mapPosition, g_mapRotationDeg, g_mapScale);
    g_scene.setMapEmissiveScale(g_mapEmissiveScale);
    g_scene.setMapFlat(g_mapFlat);
    g_scene.setGridVisible(g_cfgGrid);

    // Frame rate for the F3 HUD: slow exponential average over dt.
    if (dt > 0.0f)
        g_debugFps = g_debugFps * 0.9f + (1.0f / dt) * 0.1f;
    g_scene.setDebugFps(g_debugFps);

    // Window alpha keepalive lives on the FS pump (damageWindowsWithLiveAlpha)
    // -- it must tick even when a frame is slow.

    // The collision set = the grid platform (when world.grid) + the map
    // triangles (when map.collision). Rebuild when any of those changed.
    const uint64_t SETUP = (g_cfgGrid ? 1ull : 0ull) |
        ((g_mapCollisionOn ? 1ull : 0ull) << 1) |
        (static_cast<uint64_t>(g_scene.mapGeneration()) << 2);

    if (SETUP != g_mapCollisionSetup) {
        std::vector<CMapCollision::STL> TRIS;

        if (g_cfgGrid) {
            // The visible grid platform as a real slab: top at world zero,
            // half extent matching the drawn grid lines (20), 0.5 thick.
            // Past its edge there is no collision -- the world has no floor
            // clamp, falling off the platform is falling.
            constexpr float H = 20.0f, T = 0.5f;
            const Vec3 A{-H, 0, -H}, B{H, 0, -H}, C{H, 0, H}, D{-H, 0, H};
            const Vec3 A2{-H, -T, -H}, B2{H, -T, -H}, C2{H, -T, H},
                D2{-H, -T, H};

            const auto QUAD = [&](const Vec3& p1, const Vec3& p2,
                                  const Vec3& p3, const Vec3& p4) {
                TRIS.push_back({p1, p2, p3});
                TRIS.push_back({p1, p3, p4});
            };

            QUAD(A, D, C, B);    // top
            QUAD(A2, B2, C2, D2); // bottom
            QUAD(A, B, B2, A2);  // -z side
            QUAD(B, C, C2, B2);  // +x side
            QUAD(C, D, D2, C2);  // +z side
            QUAD(D, A, A2, D2);  // -x side
        }

        if (g_mapCollisionOn)
            TRIS.insert(TRIS.end(), g_scene.mapTriangles().begin(),
                        g_scene.mapTriangles().end());

        g_mapCollision.build(TRIS);
        g_mapCollisionSetup = SETUP;
    }

    if (g_input.consumeLook(yawDelta, pitchDelta, dt)) {
        g_scene.rotateView(yawDelta, pitchDelta);
        damageCurrentMonitor();
    }

    const Vec3 PREMOVE = g_scene.camera().position;
    applyCameraMovement(dt);

    // Map collision: a vertical capsule (rounded hull) instead of a box --
    // walls and seams slide past instead of snagging corners. The flat-floor
    // clamp in Camera::move stays as the fallback.
    if (!g_mapCollision.empty()) {
        auto& CAM = g_scene.camera();
        const Vec3 DELTA = CAM.position - PREMOVE;

        bool grounded    = false;
        bool hitCeiling  = false;

        if (DELTA.x != 0.f || DELTA.y != 0.f || DELTA.z != 0.f) {
            Vec3 feet{PREMOVE.x, PREMOVE.y - Camera::kEyeHeight, PREMOVE.z};
            grounded = g_mapCollision.moveCapsule(
                feet, DELTA, Camera::kBodyHalfWidth, Camera::kBodyHeight,
                &hitCeiling);
            CAM.position = Vec3{feet.x, feet.y + Camera::kEyeHeight, feet.z};
        }

        g_grounded = grounded;
        if (grounded && g_verticalVel < 0.f)
            g_verticalVel = 0.f; // the floor owns the fall

        // Head bump: a jump into a ceiling must die right there, or the
        // leftover upward velocity keeps pressing the capsule into it and
        // the player hangs there while gravity slowly wins.
        if (hitCeiling && g_verticalVel > 0.f)
            g_verticalVel = 0.f;
    } else {
        g_grounded = false;
    }

    // decoration:blur feeds the frost overlay on transparent windows
    // (CConfigValue binds by name and works with both hyprland.conf and
    // hyprland.lua -- the same accessor Hyprland's own renderer uses).
    {
        static const CConfigValue<Config::INTEGER> PBLURENABLED("decoration:blur:enabled");
        static const CConfigValue<Config::INTEGER> PBLURSIZE("decoration:blur:size");
        static const CConfigValue<Config::INTEGER> PBLURPASSES("decoration:blur:passes");
        static const CConfigValue<Config::FLOAT>   PBLURVIBRANCY("decoration:blur:vibrancy");

        g_scene.setBlurConfig(
            *PBLURENABLED != 0,
            static_cast<int>(std::clamp(*PBLURSIZE, int64_t{1}, int64_t{32})),
            static_cast<int>(std::clamp(*PBLURPASSES, int64_t{1}, int64_t{8})),
            *PBLURVIBRANCY);
    }

    // Super+wheel hover zoom: glide the window along its ray toward the
    // target distance. Paused while a fullscreen transition animates (its
    // own glide is one-shot and completes separately).
    if (s_zoomId != 0 && g_fsPhase == EFullscreenPhase::None) {
        if (auto* ZOOMED = g_world.find(s_zoomId)) {
            s_zoomCur += (s_zoomTarget - s_zoomCur) *
                (1.0 - std::exp(-8.0 * dt));

            // Glide along the ray ANCHORED at the last wheel event: the
            // window stays world-fixed instead of riding the camera.
            ZOOMED->center = s_zoomAnchor +
                s_zoomDir * static_cast<float>(s_zoomCur);

            // One-shot glide: once the target is reached the state is
            // dropped and the window is world-fixed again -- a live zoom
            // state would keep the window glued to a moving camera.
            if (std::fabs(s_zoomTarget - s_zoomCur) < 0.02)
                s_zoomId = 0;
        } else {
            s_zoomId = 0;
        }
    }

    if (g_transition <= 0.0f)
        return;

    // Fullscreen-exit box re-assertion: CONDITION-driven, not frame-count --
    // keep re-asserting until the window's box matches ours for 15
    // consecutive frames, outlasting ANY late Hyprland-side restore.
    if (g_fsAssertFrames > 0) {
        if (auto W = g_fsWindow.lock()) {
            Compat::setWindowBox(W, g_fsAssertBox);

            const auto CUR = Compat::currentWindowBox(W);

            if (CUR.x == g_fsAssertBox.x && CUR.y == g_fsAssertBox.y &&
                CUR.w == g_fsAssertBox.w && CUR.h == g_fsAssertBox.h) {
                if (++g_fsStableCount >= 15) {
                    g_fsAssertFrames = 0;
                    g_fsWindow       = {};
                }
            } else {
                g_fsStableCount = 0;
            }
        } else {
            g_fsAssertFrames = 0;
        }
    }

    const auto MON = targetMonitor();

    if (!MON)
        return;

    updatePanorama();

    // The camera pose has changed for this frame. Use that same new centre ray
    // for the active gesture, so movement and resize track exactly what the
    // user is looking at rather than the previous frame's ray.
    if (g_pointerGesture == EPointerGesture::Move3D && g_pointerDown) {
        g_world.updateDrag(
            g_scene.camera().position,
            g_scene.camera().centerRay(),
            dt
        );
    } else if (g_pointerGesture == EPointerGesture::ResizeReal && g_pointerDown) {
        updateRealResize();
    } else if (g_pointerGesture == EPointerGesture::WheelRoll && g_pointerDown) {
        updateWheelRoll();
    }

    syncWorld(MON, dt);

    applyFullscreenAnimation();

    // Normal client interaction is a virtual pointer located exactly at the
    // crosshair. It is updated every frame after camera motion, so buttons,
    // text fields, scrollbars, etc. receive ordinary Wayland pointer motion.
    if (!g_pointerDown && g_fsPhase == EFullscreenPhase::None)
        forwardPointerToAim(inputTimeMs());
}

class CHypr3DPassElement final : public IPassElement {
  public:
    CHypr3DPassElement(float alpha, float dt) :
        m_alpha(alpha), m_dt(dt) {}

    std::vector<UP<IPassElement>> draw() override {
        if (!g_pHyprRenderer)
            return {};

        if (g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL) {
            reportFramebufferErrorOnce("[hypr3d] renderer is not OpenGL");
            return {};
        }

        auto& renderData = g_pHyprRenderer->m_renderData;

        if (!renderData.currentFB) {
            reportFramebufferErrorOnce("[hypr3d] current framebuffer is null");
            return {};
        }

        auto* framebuffer =
            dynamic_cast<Render::GL::CGLFramebuffer*>(
                renderData.currentFB.get()
            );

        if (!framebuffer) {
            reportFramebufferErrorOnce(
                "[hypr3d] current framebuffer is not CGLFramebuffer"
            );
            return {};
        }

        const int width = std::max(
            1,
            static_cast<int>(std::round(renderData.currentFB->m_size.x))
        );

        const int height = std::max(
            1,
            static_cast<int>(std::round(renderData.currentFB->m_size.y))
        );

        const GLuint framebufferID = framebuffer->getFBID();

        if (framebufferID == 0) {
            reportFramebufferErrorOnce(
                "[hypr3d] current framebuffer has ID 0"
            );
            return {};
        }

        if (!Render::GL::g_pHyprOpenGL) {
            reportFramebufferErrorOnce("[hypr3d] g_pHyprOpenGL is null");
            return {};
        }

        Render::GL::g_pHyprOpenGL->makeEGLCurrent();

        const bool result = g_scene.render(
            framebufferID,
            width,
            height,
            m_alpha,
            m_dt,
            g_renderWindows
        );

        if (!result) {
            if (!g_reportedRenderError) {
                g_reportedRenderError = true;
                notify(
                    "[hypr3d] GLScene::render failed",
                    CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}
                );
            }

            return {};
        }

        if (!g_renderedOnce) {
            g_renderedOnce = true;

            notify(
                "[hypr3d] 3D pass executed " +
                    std::to_string(width) + "x" + std::to_string(height),
                CHyprColor{0.2f, 1.0f, 0.5f, 1.0f}
            );
        }

        return {};
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    const char* passName() override {
        return "Hypr3D";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

    bool undiscardable() override {
        return true;
    }

    bool disableSimplification() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        return std::nullopt;
    }

    CRegion opaqueRegion() override {
        return {};
    }

  private:
    static void reportFramebufferErrorOnce(const std::string& text) {
        if (g_reportedFramebufferError)
            return;

        g_reportedFramebufferError = true;

        notify(text, CHyprColor{1.0f, 0.2f, 0.2f, 1.0f});
    }

    float m_alpha;
    float m_dt;
};

// Throttled snapshot of the live state, written where I can read it directly
// instead of asking the user to describe what they see.
static void dumpStatus() {
    const auto NOW = std::chrono::steady_clock::now();

    if (std::chrono::duration<float>(NOW - g_diagLastDump).count() < 0.25f)
        return;

    g_diagLastDump = NOW;
    ++g_diagFrames;

    // Requested here, filled in later this frame by the pass element, read by
    // the next dump -- one frame of lag on a value that is not changing.
    g_scene.requestProbe();

    // History log: APPEND with a timestamp. The one-shot snapshot kept
    // missing the bug (it self-healed before the read); the timeline catches
    // the transition frame by frame. Bounded at ~128 KB, trimmed from the
    // head.
    std::ofstream out("/tmp/hypr3d-status.txt",
                      std::ios::app | std::ios::in);

    {
        std::error_code              ec;
        const auto                   SZ = std::filesystem::file_size(
            "/tmp/hypr3d-status.txt", ec);
        if (!ec && SZ > 128 * 1024) {
            std::ifstream  in("/tmp/hypr3d-status.txt");
            std::string    data((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            in.close();

            const auto CUT = data.find('\n', data.size() / 2);
            if (CUT != std::string::npos) {
                std::ofstream trim("/tmp/hypr3d-status.txt",
                                   std::ios::trunc);
                trim << data.substr(CUT + 1);
            }
        }
    }

    out << "--- frame " << g_diagFrames << " t="
        << std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                        g_inputClockStart)
               .count()
        << "s\n";

    if (!out)
        return;

    const auto MON = targetMonitor();

    out << "active=" << (g_active ? 1 : 0) << " frames=" << g_diagFrames
        << " transition=" << g_transition
        << " target=" << g_transitionTarget
        << " alphaSent=" << g_diagAlpha
        << " renderedOnce=" << (g_renderedOnce ? 1 : 0) << "\n";

    out << "hookInstalled=" << (g_hookInstalled ? 1 : 0)
        << " hookActive=" << (Compat::pointerHookActive() ? 1 : 0)
        << " sinkCalls=" << g_diagSinkCalls
        << " moveEvents=" << g_diagMoveEvents << "\n";

    out << "lastPos=" << g_diagLastPos.x << "," << g_diagLastPos.y
        << " pinned=" << g_diagPinned.x << "," << g_diagPinned.y
        << " lastDelta=" << g_diagLastDx << "," << g_diagLastDy << "\n";

    out << "lastZoomStep=" << g_diagLastZoomStep
        << " (positive = wheel forward = push away)\n";

    // warpAfter is what the compositor reported immediately after the last
    // warp: equal to `center` means the pin sticks, equal to the pre-warp
    // position means warpTo is a no-op here.
    out << "warpCalls=" << g_diagWarpCalls
        << " warpAfter=" << g_diagWarpAfter.x << "," << g_diagWarpAfter.y
        << " warpRadius=" << kWarpRadiusPx << "\n";

    // mgrPos vs lastPos: if these differ persistently, the event's position
    // and Pointer::mgr()'s position are different quantities and only one of
    // them can be used to derive a delta.
    out << "mgrPos=" << g_diagMgrPos.x << "," << g_diagMgrPos.y << "\n";

    const Vector2D CENTER = crosshairLogical();

    if (MON)
        out << "monitor=" << MON->m_name
            << " pos=" << MON->m_position.x << "," << MON->m_position.y
            << " size=" << MON->m_size.x << "x" << MON->m_size.y
            << " pixel=" << MON->m_pixelSize.x << "x" << MON->m_pixelSize.y
            << " center=" << CENTER.x << "," << CENTER.y << "\n";
    else
        out << "monitor=none\n";

    // The decisive number for the "2D desktop showing through" report: the
    // alpha actually sitting in the offscreen scene buffer at screen centre.
    if (g_scene.probeValid()) {
        const unsigned char* P = g_scene.probeRGBA();
        out << "scenePixel=" << int(P[0]) << "," << int(P[1]) << ","
            << int(P[2]) << "," << int(P[3]) << "\n";
    }
    else
        out << "scenePixel=unavailable\n";

    out << "renderWindows=" << g_renderWindows.size() << "\n";

    out << "fsPhase=" << static_cast<int>(g_fsPhase)
        << " fsWasOn=" << (g_fsWasOn ? 1 : 0)
        << " fsAlpha=" << g_fsAlpha << "\n";

    if (const auto FSW = Fullscreen::controller()->getFullscreenWindow(
            targetMonitor())) {
        const auto BOX = Compat::currentWindowBox(FSW);
        float fsRoll = 0.0f;
        if (auto* E = g_world.find(Compat::windowId(FSW)))
            fsRoll = E->roll;

        out << "fsWindow=" << Compat::windowId(FSW)
            << " box=" << BOX.x << "," << BOX.y << "," << BOX.w << "," << BOX.h
            << " restore=" << g_fsRestoreBox.w << "x" << g_fsRestoreBox.h
            << " roll=" << fsRoll << "\n";

    } else {
        out << "fsWindow=none\n";
    }
    // Per-window channel alphas + Hyprland's remembered floating size:
    // the semi-transparency and the giant-size bugs live in THESE state
    // channels; the dump pins which one is stuck and for whom.
    if (Desktop::windowState() && MON && MON->m_activeWorkspace) {
        for (auto const& W3 : Desktop::windowState()->windows()) {
            if (!W3 || W3->m_workspace != MON->m_activeWorkspace)
                continue;

            const auto WB = W3->m_target ? W3->m_target->position() :
                                           CBox{};
            out << "  win=" << Compat::windowId(W3)
                << " float=" << (W3->m_isFloating ? 1 : 0)
                << " aFull="
                << W3->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN)
                << " aActive="
                << W3->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE)
                << " aFade="
                << W3->alphaValue(Desktop::View::WINDOW_ALPHA_FADE)
                << " box=" << WB.x << "," << WB.y << "," << WB.w << ","
                << WB.h << " lastFloat="
                << (W3->m_target ? W3->m_target->lastFloatingSize().x : -1.f)
                << "x"
                << (W3->m_target ? W3->m_target->lastFloatingSize().y : -1.f)
                << "\n";
        }
    }

    out << "captureFrames=" << g_captureFrames << "\n";
}

static void onRenderStage(eRenderStage stage) {
    if (g_capturing)
        return;

    if (!g_active)
        return;

    if (!g_monitor)
        return;

    if (stage != RENDER_LAST_MOMENT)
        return;

    if (!g_currentRenderMon || g_currentRenderMon != g_monitor)
        return;

    dumpStatus();

    const float dt = updateTransition();

    if (g_transition <= 0.0f && g_transitionTarget <= 0.0f) {
        deactivate3D();
        return;
    }

    if (!g_pHyprRenderer)
        return;

    if (g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL)
        return;

    // 2D passthrough: Hyprland renders the fullscreen window; the room and
    // its input are dormant. The frame pump polls for the fullscreen exit.
    if (g_fsPhase == EFullscreenPhase::In2D)
        return;

    update3D(dt);

    g_diagAlpha = std::clamp(g_transition, 0.0f, 1.0f);

    if (g_fsPhase == EFullscreenPhase::To2D || g_fsPhase == EFullscreenPhase::To3D)
        g_diagAlpha = g_fsAlpha;

    g_pHyprRenderer->addPassElement(
        makeUnique<CHypr3DPassElement>(
            g_diagAlpha,
            dt
        )
    );

    damageCurrentMonitor();
}

// --- event handlers ---------------------------------------------------------

// The pointer hook gives us true relative motion and consumes it, so the OS
// cursor remains parked and cannot hit a screen edge. If the hook is missing,
// the absolute event path derives a delta and keeps the camera usable.
static bool hookSink(double dx, double dy) {
    ++g_diagSinkCalls;
    return onPointerMotion(dx, dy);
}

static void onMouseMove(Vector2D pos, Event::SCallbackInfo& info) {
    ++g_diagMoveEvents;
    g_diagLastPos = pos;

    if (!ownsInput())
        return;

    if (g_hookInstalled && g_diagSinkCalls > 0) {
        // The relative hook already owns the real delta. The absolute event is
        // intentionally ignored for rotation so it cannot double-count it.
        info.cancelled = true;
        return;
    }

    if (!g_diagPinnedValid) {
        g_diagPinned = pos;
        g_diagPinnedValid = true;
        info.cancelled = true;
        return;
    }

    const double dx = pos.x - g_diagPinned.x;
    const double dy = pos.y - g_diagPinned.y;
    g_diagPinned = pos;

    onPointerMotion(dx, dy);
    info.cancelled = true;
}

static void onMouseAxis(
    IPointer::SAxisEvent event,
    Event::SCallbackInfo& info
) {
    if (!ownsInput())
        return;

    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    // Physical wheels report whole detents in deltaDiscrete; touchpads send
    // a smooth delta instead. Hi-res wheels report many detents per physical
    // click -- clamped, or a light scroll flings the window across the room.
    //
    // Sign, per the Wayland axis spec: a vertical delta is negative when the
    // top of the wheel rolls AWAY from the user ("forward") with
    // IDENTICAL direction, and positive when the client is told INVERTED
    // (natural scroll). Normalize to +1 step = wheel forward in both cases,
    // so the zoom below can be written against the physical wheel motion
    // instead of guessing the compositor's sign.
    const double RAW = event.deltaDiscrete != 0 ?
        static_cast<double>(event.deltaDiscrete) :
        event.delta * 0.05;

    const bool INVERTED = event.relativeDirection ==
        WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED;

    double STEPS = std::clamp(INVERTED ? RAW : -RAW, -2.0, 2.0);

    if (STEPS == 0.0)
        return;

    g_diagLastZoomStep = STEPS;

    // During an LMB drag the wheel zooms the dragged window.
    if (g_pointerGesture == EPointerGesture::Move3D && g_pointerDown) {
        g_world.dragZoom(STEPS);
        info.cancelled = true;
        damageCurrentMonitor();
        return;
    }

    // Any other time: Super + wheel over a window zooms it along the ray
    // from the camera through the window. Without Super the wheel reaches
    // the focused client unchanged (scroll).
    if (!g_superHeld)
        return;

    const auto HIT = aimHit();

    if (!HIT.hit)
        return;

    const auto* ENTITY = g_world.find(HIT.id);

    if (!ENTITY)
        return;

    const auto& CAM = g_scene.camera();
    const Vec3 OFFSET = ENTITY->center - CAM.position;
    const float DIST = std::sqrt(
        OFFSET.x * OFFSET.x + OFFSET.y * OFFSET.y + OFFSET.z * OFFSET.z);

    if (!std::isfinite(DIST) || DIST < 0.05f)
        return;

    const bool NEW = s_zoomId != HIT.id;
    s_zoomId     = HIT.id;
    s_zoomDir    = OFFSET * (1.0f / DIST);
    s_zoomAnchor = CAM.position;

    if (NEW) {
        s_zoomCur    = DIST;
        s_zoomTarget = DIST;
    }

    // Wheel forward (+) pushes the window away, wheel back pulls it closer;
    // the drag zoom receives the same normalized sign, so both wheel paths
    // agree. No distance limits -- the target stays positive, so scrolling
    // just keeps multiplying; the window can come arbitrarily close or far.
    s_zoomTarget *= std::pow(1.06, STEPS);

    info.cancelled = true;
    damageCurrentMonitor();
}

static void onMouseButton(
    IPointer::SButtonEvent event,
    Event::SCallbackInfo& info
) {
    if (!ownsInput())
        return;

    const bool PRESSED =
        event.state == WL_POINTER_BUTTON_STATE_PRESSED;

    // Release the plugin gesture that owns this physical button.
    if (!PRESSED && g_pointerDown && event.button == g_pointerButton) {
        resetPointerGesture();
        info.cancelled = true;
        damageCurrentMonitor();
        return;
    }

    // Super + wheel-press: grab the aimed window's roll around its normal.
    // Sweeping the crosshair around the window center then rotates the
    // window by the swept angle; release ends it. Never reaches the client.
    if (PRESSED && g_superHeld && g_fsPhase == EFullscreenPhase::None &&
        event.button == BTN_MIDDLE) {
        const World3D::SHit HIT = aimHit();
        const auto* ENTITY = HIT.hit ? g_world.find(HIT.id) : nullptr;

        if (ENTITY) {
            const Vec3 REFERENCE = HIT.point - ENTITY->center;

            // The sweep angle is undefined when the crosshair sits exactly
            // on the center -- refuse the grab there.
            if (dot(REFERENCE, REFERENCE) > 0.0004f) {
                s_wheelRot.active     = true;
                s_wheelRot.id         = HIT.id;
                s_wheelRot.center     = ENTITY->center;
                s_wheelRot.normal     = g_world.normalOf(HIT.id);
                s_wheelRot.reference  = REFERENCE;
                s_wheelRot.startRoll  = ENTITY->roll;

                g_pointerGesture = EPointerGesture::WheelRoll;
                g_pointerButton  = BTN_MIDDLE;
                g_pointerDown    = true;
                g_resize         = {};

                info.cancelled = true;
                damageCurrentMonitor();
                return;
            }
        }

        info.cancelled = true;
        return;
    }

    // Super+LMB / Super+RMB are exclusively plugin gestures. Nothing is sent
    // to the client for these physical button events. Layer surfaces can be
    // dragged like windows, but not resized -- their real geometry is owned
    // by the shell that anchored them.
    if (PRESSED && g_superHeld && g_fsPhase == EFullscreenPhase::None &&
        (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
        const World3D::SHit HIT = aimHit();
        const auto TARGET = HIT.hit ? targetFromHit(HIT.id) : SHitTarget{};

        if (!TARGET.window && !TARGET.layer) {
            info.cancelled = true;
            return;
        }

        const auto& CAM = g_scene.camera();

        if (TARGET.window)
            Compat::focusWindow(TARGET.window);

        if (event.button == BTN_RIGHT && !TARGET.window) {
            info.cancelled = true;
            return;
        }

        if (event.button == BTN_LEFT) {
            if (!g_world.startDrag(
                    HIT.id, HIT, CAM.position, CAM.forward())) {
                info.cancelled = true;
                return;
            }
            s_zoomId = 0; // the drag owns this window's distance now

            // A drag on the assert window: the drag wins, stop fighting.
            if (g_fsAssertFrames > 0 && HIT.id == Compat::windowId(TARGET.window)) {
                g_fsAssertFrames = 0;
                g_fsWindow = {};
            }

            g_pointerGesture = EPointerGesture::Move3D;
            g_pointerButton = BTN_LEFT;
            g_pointerDown = true;
            g_resize = {};
        }
        else {
            const auto ENTITY = g_world.find(HIT.id);
            if (!ENTITY) {
                info.cancelled = true;
                return;
            }

            g_resize = {};
            g_resize.active = true;
            g_resize.id = HIT.id;
            g_resize.window = TARGET.window;
            g_resize.startBox = Compat::currentWindowBox(TARGET.window);
            g_resize.startCenter = ENTITY->center;
            g_resize.startWorldWidth = ENTITY->width;
            g_resize.startWorldHeight = ENTITY->height;
            g_resize.planePoint = ENTITY->center;
            g_resize.planeNormal = g_world.normalOf(HIT.id);

            // The grab point only seeds which edge follows the crosshair;
            // updateRealResize re-evaluates that every frame from the aim's
            // side relative to the window centre, so the grab lands anywhere
            // on the window and the pull direction decides the rest. As the
            // camera turns, the current centre ray is intersected with this
            // same window plane, so the real window stretches exactly toward
            // the point being aimed at.
            // RayHit::v is already top-to-bottom. Top half follows +1,
            // bottom half follows -1 in the CBox edge convention below.
            g_resize.edgeX = HIT.u < 0.5f ? -1 : 1;
            g_resize.edgeY = HIT.v < 0.5f ? 1 : -1;
            g_resize.grabPx = worldPointToGlobalPx(targetMonitor(), HIT.point);

            g_pointerGesture = EPointerGesture::ResizeReal;
            g_pointerButton = BTN_RIGHT;
            g_pointerDown = true;
        }

        info.cancelled = true;
        damageCurrentMonitor();
        return;
    }

    // Regular buttons are virtual-pointer buttons at the crosshair. Track the
    // pressed surface so release goes back to the same client even if the
    // camera turns away before release.
    const World3D::SHit HIT = aimHit();
    const auto TARGET = HIT.hit ? targetFromHit(HIT.id) : SHitTarget{};

    if (!TARGET.window && !TARGET.layer) {
        info.cancelled = true;
        return;
    }

    const Vector2D LOCAL = localFromHit(HIT);

    if (PRESSED) {
        if (TARGET.window)
            Compat::focusWindow(TARGET.window);

        if (TARGET.layer)
            Compat::deliverClick(TARGET.layer, LOCAL, event.button, true, event.timeMs);
        else
            Compat::deliverClick(TARGET.window, LOCAL, event.button, true, event.timeMs);

        g_clientButtonWindow = TARGET.window;
        g_clientButtonLayer  = TARGET.layer;
        g_clientButton = event.button;
        g_clientButtonLocal = LOCAL;
        g_clientButtonDown = true;
    }
    else {
        if (g_clientButtonDown && (g_clientButtonWindow || g_clientButtonLayer)) {
            if (g_clientButtonLayer)
                Compat::deliverClick(
                    g_clientButtonLayer,
                    g_clientButtonLocal,
                    g_clientButton,
                    false,
                    event.timeMs
                );
            else
                Compat::deliverClick(
                    g_clientButtonWindow,
                    g_clientButtonLocal,
                    g_clientButton,
                    false,
                    event.timeMs
                );
            g_clientButtonWindow = nullptr;
            g_clientButtonLayer  = nullptr;
            g_clientButton = 0;
            g_clientButtonDown = false;
        }
    }

    info.cancelled = true;
    damageCurrentMonitor();
}

// Space mode: only the camera movement keys are swallowed -- everything else
// still reaches the window the crosshair is aiming at. Minecraft creative-
// flight scheme: Space ascends, Shift descends, Ctrl sprints.
static bool isMovementSym(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_w:
        case XKB_KEY_a:
        case XKB_KEY_s:
        case XKB_KEY_d:
        case XKB_KEY_space:
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R: return true;
        default: return false;
    }
}

static void setMovementSym(xkb_keysym_t sym, bool down) {
    switch (sym) {
        case XKB_KEY_w: g_keyFwd = down; break;
        case XKB_KEY_s: g_keyBack = down; break;
        case XKB_KEY_a: g_keyLeft = down; break;
        case XKB_KEY_d: g_keyRight = down; break;
        case XKB_KEY_space: g_keyUp = down; break;
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R: g_keyDown = down; break;
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R: g_keySprint = down; break;
        default: break;
    }
}

static void onKeyboardKey(
    IKeyboard::SKeyEvent event,
    Event::SCallbackInfo& info
) {
    if (!ownsInput())
        return;

    if (!g_pSeatManager || g_pSeatManager->m_keyboard.expired())
        return;

    const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (!KEYBOARD || !KEYBOARD->m_xkbSymState)
        return;

    // libinput reports evdev codes; xkb wants them offset by 8.
    const auto SYM = xkb_state_key_get_one_sym(
        KEYBOARD->m_xkbSymState,
        event.keycode + 8
    );

    const bool PRESSED = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;

    // Modifier state is tracked in both modes: Super drives the mouse
    // gestures, and Super + Left Alt toggles the keyboard mode.
    if (SYM == XKB_KEY_Super_L || SYM == XKB_KEY_Super_R)
        g_superHeld = PRESSED;
    else if (SYM == XKB_KEY_Alt_L)
        g_altHeld = PRESSED;

    // Walking mode: Space jumps off whatever the capsule stands on. The
    // held state still reaches setMovementSym, but the walking movement
    // path zeroes the vertical input, so holding Space does not fly.
    if (PRESSED && SYM == XKB_KEY_space && !g_playerFlying && g_grounded) {
        g_verticalVel = 5.5f;
        g_grounded    = false;
    }

    // F3 toggles the debug HUD (collision wireframe + info overlay) in both
    // keyboard modes: it never belongs to the focused window.
    if (PRESSED && SYM == XKB_KEY_F3) {
        g_debugHud = !g_debugHud;
        g_scene.setMapDebugCollisions(g_debugHud);
        g_scene.setDebugOverlay(g_debugHud);

        info.cancelled = true;
        damageCurrentMonitor();
        return;
    }

    if (PRESSED && g_superHeld && g_altHeld &&
        (SYM == XKB_KEY_Alt_L || SYM == XKB_KEY_Super_L)) {
        g_keyboardMode =
            g_keyboardMode == EKeyboardMode::Space ?
                EKeyboardMode::Window :
                EKeyboardMode::Space;

        if (g_keyboardMode == EKeyboardMode::Window)
            resetCameraKeys(); // held camera keys must not keep walking

        notify(
            g_keyboardMode == EKeyboardMode::Space ?
                "[hypr3d] keyboard: space (wasd / space / shift / ctrl)" :
                "[hypr3d] keyboard: window (typing reaches the focused window)",
            CHyprColor{0.2f, 0.8f, 0.4f, 1.0f}
        );

        info.cancelled = true;
        return;
    }

    // Window mode: every key reaches the focused window untouched.
    if (g_keyboardMode == EKeyboardMode::Window)
        return;

    if (!isMovementSym(SYM))
        return;

    setMovementSym(SYM, PRESSED);

    info.cancelled = true;
}

// --- plugin entry -----------------------------------------------------------

static int luaConfig(lua_State* L) {
    // hl.plugin.hypr3d.config({
    //     world = {
    //         panorama = "~/picture.png",   -- 360-degree room background
    //         grid = true,                  -- base grid platform (visible +
    //                                       -- collidable, at world zero)
    //     },
    //     windows = {
    //         window_scale = 0.5,           -- room multiplier on window size
    //         spawn_distance = 5.0,         -- units in front of the camera
    //     },
    //     player = {
    //         look_sensitivity = 0.0025,    -- radians per pointer count
    //         look_inertia = 0.03,          -- look glide, seconds (0 = off)
    //         move_inertia = 0.05,          -- walk glide, seconds (0 = off)
    //         move_speed = 4.0,             -- world units / second
    //         spawn = { x = 0, y = 0, z = 0 }, -- FEET position
    //         flying = true,                -- false: gravity, Space jumps,
    //                                       -- Shift does nothing
    //     },
    //     map = {
    //         path = "~/map.glb",
    //         transform = {
    //             position = { x = 0, y = 0, z = 0 },
    //             rotation = { x = 0, y = 0, z = 0 }, -- degrees, XYZ
    //             scale = { x = 1, y = 1, z = 1 },    -- per-axis
    //         },
    //         emissive_scale = 1.0,
    //         flat = true,                  -- baked-map look (no dynamic light)
    //         collision = true,
    //     },
    // })
    //
    // Missing keys keep their current value; wrong-typed keys raise a lua
    // error. Numbers are clamped on set.
    if (!lua_istable(L, 1))
        return luaL_error(L, "hypr3d.config expects a single table");

    // Field accessors. TIDX = stack index of the section table (0 = absent).
    const auto SET_NUM = [&](int tidx, const char* key, float& out,
                             float lo, float hi, const char* path) -> bool {
        lua_getfield(L, tidx, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        out = std::clamp(static_cast<float>(lua_tonumber(L, -1)), lo, hi);
        lua_pop(L, 1);
        return true;
    };

    const auto SET_BOOL = [&](int tidx, const char* key, bool& out,
                              const char* path) -> bool {
        lua_getfield(L, tidx, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }
        if (!lua_isboolean(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        out = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return true;
    };

    const auto SET_STRING = [&](int tidx, const char* key, std::string& out,
                                const char* path) -> bool {
        lua_getfield(L, tidx, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        size_t LEN = 0;
        const char* STR = lua_tolstring(L, -1, &LEN);
        out.assign(STR, LEN);
        lua_pop(L, 1);
        return true;
    };

    // Missing axes keep their current values.
    const auto SET_VEC3 = [&](int tidx, const char* key, Vec3& out,
                              const char* path) -> bool {
        lua_getfield(L, tidx, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return false;
        }

        const auto AXIS = [&](const char* name, float& v) {
            lua_getfield(L, -1, name);
            if (lua_isnumber(L, -1))
                v = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        };

        AXIS("x", out.x);
        AXIS("y", out.y);
        AXIS("z", out.z);
        lua_pop(L, 1);
        return true;
    };

    // A sub-table section: returns its stack index, or 0 when absent/wrong.
    // A wrong-typed section is an error, a missing one is skipped.
    const auto SECTION = [&](const char* key, const char* path) -> int {
        lua_getfield(L, 1, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return 0;
        }
        if (!lua_istable(L, -1))
            return -1; // caller reports
        return lua_gettop(L);
    };

    int idx = SECTION("world", "world");
    if (idx == -1)
        return luaL_error(L, "hypr3d.config: world must be a table");
    if (idx > 0) {
        if (!SET_STRING(idx, "panorama", g_cfgPanorama, "world.panorama"))
            return luaL_error(L, "hypr3d.config: world.panorama must be a string");
        if (!SET_STRING(idx, "monitor", g_cfgMonitor, "world.monitor"))
            return luaL_error(L, "hypr3d.config: world.monitor must be a string");
        if (!SET_BOOL(idx, "grid", g_cfgGrid, "world.grid"))
            return luaL_error(L, "hypr3d.config: world.grid must be a boolean");
        lua_pop(L, 1);
    }

    idx = SECTION("windows", "windows");
    if (idx == -1)
        return luaL_error(L, "hypr3d.config: windows must be a table");
    if (idx > 0) {
        if (!SET_NUM(idx, "window_scale", g_cfgWindowScale, 0.1f, 8.0f,
                     "windows.window_scale"))
            return luaL_error(L, "hypr3d.config: windows.window_scale must be a number");
        if (!SET_NUM(idx, "spawn_distance", g_cfgSpawnDistance, 1.0f, 100.0f,
                     "windows.spawn_distance"))
            return luaL_error(L, "hypr3d.config: windows.spawn_distance must be a number");
        lua_pop(L, 1);
    }

    idx = SECTION("player", "player");
    if (idx == -1)
        return luaL_error(L, "hypr3d.config: player must be a table");
    if (idx > 0) {
        if (!SET_NUM(idx, "look_sensitivity", g_cfgSensitivity, 0.0001f, 0.05f,
                     "player.look_sensitivity"))
            return luaL_error(L, "hypr3d.config: player.look_sensitivity must be a number");
        if (!SET_NUM(idx, "look_inertia", g_cfgLookInertia, 0.0f, 1.0f,
                     "player.look_inertia"))
            return luaL_error(L, "hypr3d.config: player.look_inertia must be a number");
        if (!SET_NUM(idx, "move_inertia", g_cfgMoveInertia, 0.0f, 1.0f,
                     "player.move_inertia"))
            return luaL_error(L, "hypr3d.config: player.move_inertia must be a number");
        if (!SET_NUM(idx, "move_speed", g_cfgMoveSpeed, 0.5f, 50.0f,
                     "player.move_speed"))
            return luaL_error(L, "hypr3d.config: player.move_speed must be a number");
        if (!SET_BOOL(idx, "flying", g_playerFlying, "player.flying"))
            return luaL_error(L, "hypr3d.config: player.flying must be a boolean");
        if (!SET_VEC3(idx, "spawn", g_playerSpawn, "player.spawn"))
            return luaL_error(L, "hypr3d.config: player.spawn must be a table { x = .., y = .., z = .. }");
        // y is the FEET height; only guard against absurd values.
        g_playerSpawn.y = std::clamp(g_playerSpawn.y, -1000.0f, 1000.0f);
        lua_pop(L, 1);
    }

    idx = SECTION("map", "map");
    if (idx == -1)
        return luaL_error(L, "hypr3d.config: map must be a table");
    if (idx > 0) {
        if (!SET_STRING(idx, "path", g_cfgMapPath, "map.path"))
            return luaL_error(L, "hypr3d.config: map.path must be a string");
        if (!SET_BOOL(idx, "flat", g_mapFlat, "map.flat"))
            return luaL_error(L, "hypr3d.config: map.flat must be a boolean");
        if (!SET_BOOL(idx, "collision", g_mapCollisionOn, "map.collision"))
            return luaL_error(L, "hypr3d.config: map.collision must be a boolean");
        if (!SET_NUM(idx, "emissive_scale", g_mapEmissiveScale, 0.0f, 20.0f,
                     "map.emissive_scale"))
            return luaL_error(L, "hypr3d.config: map.emissive_scale must be a number");

        lua_getfield(L, idx, "transform");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
        } else if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(
                L, "hypr3d.config: map.transform must be a table with position/rotation/scale");
        } else {
            const int TIDX = lua_gettop(L);

            if (!SET_VEC3(TIDX, "position", g_mapPosition, "map.transform.position"))
                return luaL_error(L, "hypr3d.config: map.transform.position must be a table { x = .., y = .., z = .. }");
            if (!SET_VEC3(TIDX, "rotation", g_mapRotationDeg, "map.transform.rotation"))
                return luaL_error(L, "hypr3d.config: map.transform.rotation must be a table { x = .., y = .., z = .. } (degrees)");
            if (!SET_VEC3(TIDX, "scale", g_mapScale, "map.transform.scale"))
                return luaL_error(L, "hypr3d.config: map.transform.scale must be a table { x = .., y = .., z = .. }");

            // Zero axes would collapse the map to a plane hair.
            g_mapScale.x = std::clamp(g_mapScale.x, 0.05f, 10.0f);
            g_mapScale.y = std::clamp(g_mapScale.y, 0.05f, 10.0f);
            g_mapScale.z = std::clamp(g_mapScale.z, 0.05f, 10.0f);

            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    return 0;
}

static int luaToggle(lua_State*) {
    toggle3D();
    return 0;
}

static int luaOpen(lua_State*) {
    open3D();
    return 0;
}

static int luaClose(lua_State*) {
    close3D();
    return 0;
}

static SDispatchResult dispatchToggle(std::string) {
    toggle3D();
    return {};
}

static SDispatchResult dispatchOpen(std::string) {
    open3D();
    return {};
}

static SDispatchResult dispatchClose(std::string) {
    close3D();
    return {};
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string serverHash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();

    if (serverHash != clientHash)
        throw std::runtime_error("[hypr3d] Hyprland API hash mismatch");

    // Relative pointer capture is what lets the mouse look around freely
    // instead of hitting the edge of whatever screen it started on.
    g_hookInstalled =
        Compat::installPointerHook(PHANDLE, &hookSink);

    if (!g_hookInstalled) {
        g_reportedPointerHookError = true;

        notify(
            "[hypr3d] pointer hook unavailable: mouse-look falls back to "
            "cursor-relative motion and will stop at screen edges",
            CHyprColor{1.0f, 0.6f, 0.2f, 1.0f}
        );
    }

    HyprlandAPI::addNotification(
        PHANDLE,
        "[hypr3d] renderer loaded",
        CHyprColor{0.2f, 0.8f, 0.4f, 1.0f},
        3000
    );

    if (!HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:toggle", dispatchToggle))
        throw std::runtime_error("[hypr3d] failed to register toggle dispatcher");

    if (!HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:open", dispatchOpen))
        throw std::runtime_error("[hypr3d] failed to register open dispatcher");

    if (!HyprlandAPI::addDispatcherV2(PHANDLE, "hypr3d:close", dispatchClose))
        throw std::runtime_error("[hypr3d] failed to register close dispatcher");

    if (!HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "toggle", luaToggle))
        throw std::runtime_error("[hypr3d] failed to register Lua toggle");

    if (!HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "open", luaOpen))
        throw std::runtime_error("[hypr3d] failed to register Lua open");

    if (!HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "close", luaClose))
        throw std::runtime_error("[hypr3d] failed to register Lua close");

    if (!HyprlandAPI::addLuaFunction(PHANDLE, "hypr3d", "config", luaConfig))
        throw std::runtime_error("[hypr3d] failed to register Lua config");

    static auto renderPre =
        Event::bus()->m_events.render.pre.listen(
            [](PHLMONITOR mon) { onRenderPre(mon); }
        );

    static auto renderStage =
        Event::bus()->m_events.render.stage.listen(
            [](eRenderStage stage) { onRenderStage(stage); }
        );

    static auto mouseMove =
        Event::bus()->m_events.input.mouse.move.listen(
            [](Vector2D pos, Event::SCallbackInfo& info) {
                onMouseMove(pos, info);
            }
        );

    static auto mouseButton =
        Event::bus()->m_events.input.mouse.button.listen(
            [](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
                onMouseButton(event, info);
            }
        );

    static auto mouseAxis =
        Event::bus()->m_events.input.mouse.axis.listen(
            [](IPointer::SAxisEvent event, Event::SCallbackInfo& info) {
                onMouseAxis(event, info);
            }
        );

    static auto keyboardKey =
        Event::bus()->m_events.input.keyboard.key.listen(
            [](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
                onKeyboardKey(event, info);
            }
        );

    (void)renderPre;
    (void)renderStage;
    (void)mouseMove;
    (void)mouseButton;
    (void)mouseAxis;
    (void)keyboardKey;

    if (!HyprlandAPI::reloadConfig()) {
        notify(
            "[hypr3d] reloadConfig failed",
            CHyprColor{1.0f, 0.6f, 0.2f, 1.0f}
        );
    }

    return {
        "hypr3d",
        "A new perspective on window management",
        "Samine825",
        "0.5.0"
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_active = false;
    stopFramePump();
    g_transition = 0.0f;
    g_transitionTarget = 0.0f;

    if (Compat::setCursorHidden(false) && g_pHyprRenderer)
        g_pHyprRenderer->setCursorFromName("default", true);

    clearAimFocus();

    g_world.clear();
    g_renderWindows.clear();

    // Defer the snapshot release to the event loop: this runs INSIDE
    // Hyprland's render pass, and destroying framebuffers/textures here
    // breaks the frame that is still being composed (windows left
    // transparent until their next repaint).
    g_captureReleasePending = true;

    // Put the windows back under the layout before the plugin goes away.
    unghostWindows();

    // The FS fade channel: on FS-on Hyprland dims EVERY other workspace
    // window to alpha 0 (WINDOW_ALPHA_FULLSCREEN). If our transition raced
    // the fade, windows can be left semi-transparent after the 3D exit.
    // Reset the channel for the active workspace unconditionally.
    if (const auto MON = targetMonitor(); MON && MON->m_activeWorkspace) {
        for (auto const& W3 : Desktop::windowState()->windows()) {
            if (W3 && W3->m_workspace == MON->m_activeWorkspace &&
                !W3->m_pinned)
                *W3->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN) = 1.F;
        }
    }

    // The ghosted windows were skipped by Hyprland's main render during 3D;
    // force a full monitor repaint so every window is re-composited from its
    // own buffer immediately (static clients would otherwise stay invisible
    // until their first repaint).
    if (g_pHyprRenderer && g_monitor)
        g_pHyprRenderer->damageMonitor(g_monitor);

    Compat::setPointerCapture(false);
    Compat::removePointerHook();

    g_monitor = nullptr;

    if (Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        g_scene.shutdown();
    }
}

} // namespace H3D

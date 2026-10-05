#pragma once

#include "Render/MapModel.hpp"
#include "Render/PlayerModel.hpp"
#include "World/Camera.hpp"
#include "World/Outline.hpp"
#include "World/Picking.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace H3D {

// The vertical field of view of the 3D view, in degrees. Shared with
// main.cpp (fullscreen transition computes the screen-filling quad from it).
static constexpr float kFovDeg = 65.0f;

// The 3D view: a first-person room containing the windows of the active
// workspace. It deliberately knows nothing about Hyprland -- main.cpp feeds it
// WindowRender entries and reads back what the crosshair hit.
class GLScene {
  public:
    // One window as it should appear in the 3D world. Carries exactly what a
    // draw and a pick both need, so the renderer cannot reach into compositor
    // state and the two can never disagree about what is on screen.
    struct WindowRender {
        std::uintptr_t id = 0;

        // 0 = not captured yet. Such a window is neither drawn nor pickable,
        // which is what keeps aim focus from handing keyboard input to
        // something invisible.
        unsigned int texture = 0;

        float x = 0.f, y = 0.f, z = 0.f; // world centre
        float width = 0.f, height = 0.f; // world size, independent per window
        float yaw = 0.f, pitch = 0.f;   // world orientation, radians
        float roll = 0.f;                // in-plane rotation around the normal

        // Subrect of `texture` to sample: (u0,v0) at the quad's bottom-left in
        // GL convention, (u1,v1) at its top-right. Defaults cover the whole
        // texture so a caller with a tightly-fit texture needs no bookkeeping.
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;

        float alpha = 1.0f;

        // Depth slab: thickness in world units, extruded BACKWARDS along the
        // window's normal (0 = the plain flat quad). When `outlines` carries
        // the captured alpha's silhouette, the slab's walls hug that shape --
        // rounded corners stay rounded -- and each wall samples its own
        // silhouette texel, so the window texture's edge colors paint the
        // sides. Without an outline the slab is a plain box.
        float depth = 0.0f;
        std::shared_ptr<const std::vector<SOutlineLoop>> outlines;
    };

    GLScene();

    // Draws into `targetFBO` and composites the result over whatever is
    // already there with `alpha`, so the 2D desktop underneath fades out
    // instead of being captured and re-projected -- the workspace itself is
    // never rendered as an object in the scene.
    bool render(unsigned int targetFBO, int width, int height, float alpha,
                float dt, const std::vector<WindowRender>& windows);

    // Raw pointer counts -> camera look. Prefer driving this through
    // InputController so sensitivity policy stays in one testable place.
    void mouseMove(float dx, float dy);

    // Already-converted rotation in radians.
    void rotateView(float yawDelta, float pitchDelta);

    // Background panorama: set a path ("" restores the default void). The
    // image is (re)loaded lazily inside render() when the file changes.
    // Equirectangular mapping around the camera.
    void setPanoramaPath(const std::string& path);

    // What the crosshair currently points at, or 0 for "nothing". Ray is cast
    // through the centre of the screen from the camera, never from the OS
    // cursor, so aiming is independent of where the pointer happens to sit.
    std::uintptr_t pick(const std::vector<WindowRender>& windows) const;

    Camera& camera() {
        return m_camera;
    }
    const Camera& camera() const {
        return m_camera;
    }

    void reset();
    void shutdown();

    // F3 debug HUD: collision wireframe + info text (coords/yaw/pitch/fps).
    void setDebugOverlay(bool on) {
        m_debugOverlay = on;
    }

    // Hidden while the typing mode frees the real cursor.
    void setCrosshairVisible(bool on) {
        m_crosshairVisible = on;
    }

    void setDebugFps(float fps) {
        m_debugFps = fps;
    }

    // The base grid platform (world zero): visible + collidable.
    // F3 debug: the player's collision capsule outline (world space).
    void setPlayerDebugCapsule(const Vec3& center, bool on) {
        m_pDbgCenter = center;
        m_pDbgOn     = on;
    }

    // View zoom (the C key): magnification narrows the render fov
    // symmetrically around the crosshair, so aiming stays exact.
    void setZoom(float magnification) {
        m_zoom = magnification > 0.01f ? magnification : 0.01f;
    }

    float zoom() const {
        return m_zoom;
    }

    // Environment visibility for the 2D<->3D view morph: 1 = the room fully
    // drawn (steady state), 0 = windows only over a transparent scene
    // buffer (the 2D desktop beneath shows through everywhere the window
    // quads do not cover). While below 1 the scene pass switches to a
    // premultiplied per-pixel composite: the environment fades as one, the
    // window quads stay fully opaque from the first morph frame.
    void setEnvAlpha(float a) {
        m_envAlpha = std::clamp(a, 0.0f, 1.0f);
    }

    // The player's own character (player.mesh). The SAME mesh description
    // the scene objects use: path, transform, material overrides. Config
    // and pose come from main; the animation clock runs inside render().
    struct SPlayerCfg {
        std::string path;
        Vec3        posOffset{};   // model anchor offset from the feet
        Vec3        rotDeg{};      // authored facing correction, XYZ degrees
        Vec3        scale{1.f, 1.f, 1.f};
        float       emissiveScale = 1.0f;
        bool        flat = false;  // true: raw texture, no headlight shading
        std::string center;       // parsed for format parity; the player
        Vec3        centerOffset{}; // rotates around its anchor, so only
                                  // center_offset (the anchor shift) applies
        // idle, walk, run, jump: animation index or name (name wins),
        // plus the playback speed multiplier (1 = as authored).
        int         animIdx[CPlayerModel::kStateCount] = {-1, -1, -1, -1};
        std::string animName[CPlayerModel::kStateCount];
        float       animSpeed[CPlayerModel::kStateCount] = {1.f, 1.f, 1.f, 1.f};
    };

    void setPlayerConfig(const SPlayerCfg& cfg) {
        m_playerCfg = cfg;
        m_playerPath.clear(); // forces a (re)load attempt
    }

    void setPlayerPose(const Vec3& feet, float yawRad) {
        m_playerFeet = feet;
        m_playerYaw  = yawRad;
    }

    void setPlayerVisible(bool on) {
        m_playerVisible = on;
    }

    CPlayerModel* player() {
        return &m_player;
    }

    void setGridVisible(bool on) {
        m_gridVisible = on;
    }

    // --- scene: an arbitrary number of glTF objects -------------------------

    // One object's render-relevant config (index-aligned with the slots).
    struct SSceneSpec {
        std::string path;
        Vec3 position{}, rotationDeg{}, scale{1.0f, 1.0f, 1.0f};
        float emissiveScale = 1.0f;
        bool flat = false; // false = headlight half-lambert shading
        CMapModel::ECenter center = CMapModel::ECenter::Logical;
        Vec3 centerOffset{};
    };

    // Diff-apply the object list: new slots are created (files load lazily
    // in refreshScene, EGL current there), changed specs update transforms
    // in place, removed slots free their GL objects. The scene fingerprint
    // folds every model's generation, so collision can rebuild when any
    // object (re)loads or moves.
    void setSceneObjects(const std::vector<SSceneSpec>& specs);

    // Per-model access for collision and the grab interaction.
    size_t sceneModelCount() const {
        return m_slots.size();
    }

    CMapModel* sceneModel(size_t index) {
        return index < m_slots.size() ? m_slots[index].model.get() : nullptr;
    }

    // Move/rotate a (grabbed) object: updates the slot spec and the model
    // transform in one place so mtime reloads keep the new placement.
    void setSceneObjectTransform(size_t index, const Vec3& position,
                                 const Vec3& rotationDeg);

    uint64_t sceneFingerprint() const;

    void setMapDebugCollisions(bool on) {
        for (auto& S : m_slots)
            S.model->setDebugCollisions(on);
    }

    // Diagnostics: requests a single pixel readback from the offscreen scene
    // on the next frame. If it comes back with alpha below 255 the composite
    // is see-through by construction, not because of the entry fade.
    void requestProbe();
    bool probeValid() const;
    const unsigned char* probeRGBA() const;

  private:
    bool initialize();
    bool createPrograms();
    bool createMeshes();
    bool ensureSceneFramebuffer(int width, int height);

    void refreshScene();
    void refreshPlayer();

    void drawDebugOverlay(int width, int height);

    void destroyGLObjects();

    // texture == 0 draws `color` instead of sampling. uvRect is
    // {u0, v0, u1, v1} in texture space.
    void drawQuad(
        unsigned int vao,
        int vertexCount,
        const Mat4& mvp,
        unsigned int texture,
        const float uvRect[4],
        float r, float g, float b, float a
    );

    void drawFloor(const Mat4& vp);
    void drawGrid(const Mat4& vp);
    void drawPanorama(float aspect);
    void refreshPanorama();

    void drawWindows(const Mat4& vp, const std::vector<WindowRender>& windows);
    void drawFullscreen(float alpha, bool perPixel);
    void drawCrosshair(int width, int height);

    // Fullscreen NDC quad in the SCENE program's layout (pos3+uv2): the
    // environment fade multiplies the scene buffer in place.
    void drawEnvFade(float a);

  private:
    bool m_initialized = false;

    int m_sceneWidth = 0;
    int m_sceneHeight = 0;

    unsigned int m_sceneFBO = 0;
    unsigned int m_sceneColor = 0;
    unsigned int m_sceneDepth = 0;

    unsigned int m_sceneProgram = 0;
    unsigned int m_blitProgram = 0;
    unsigned int m_panoramaProgram = 0;

    // Unit quad in the XY plane, centred on the origin, facing +Z. Windows.
    unsigned int m_quadVAO = 0;
    unsigned int m_quadVBO = 0;
    int          m_quadVertexCount = 0;

    // Dynamic mesh for BSP-ordered window pieces (pos3+uv2, like the scene
    // program's layout).
    unsigned int m_polyVAO = 0;
    unsigned int m_polyVBO = 0;

    // Unit quad in the XZ plane. The ground.
    unsigned int m_floorVAO = 0;
    unsigned int m_floorVBO = 0;
    int          m_floorVertexCount = 0;

    unsigned int m_gridVAO = 0;
    unsigned int m_gridVBO = 0;
    int          m_gridVertexCount = 0;

    unsigned int m_fullscreenVAO = 0;
    unsigned int m_fullscreenVBO = 0;

    unsigned int m_fadeVAO = 0;
    unsigned int m_fadeVBO = 0;

    unsigned int m_crosshairVAO = 0;
    unsigned int m_crosshairVBO = 0;

    int m_sceneMVP = -1;
    int m_sceneTexture = -1;
    int m_sceneTextured = -1;
    int m_sceneColorUniform = -1;
    int m_sceneUVRect = -1;
    int m_scenePremult = -1;

    int m_blitTexture = -1;
    int m_blitAlpha = -1;
    int m_blitPerPx = -1;

    int m_panoramaFwd = -1;
    int m_panoramaRight = -1;
    int m_panoramaUp = -1;
    int m_panoramaTanX = -1;
    int m_panoramaTanY = -1;
    int m_panoramaLod = -1;
    int m_panoramaSampler = -1;

    std::string m_panoramaPath;   // requested (resolved) path
    std::string m_panoramaLoaded; // path actually loaded into m_panoramaTex
    std::filesystem::file_time_type m_panoramaMtime{};
    bool m_panoramaMtimeValid = false;
    unsigned int m_panoramaTex = 0;
    int m_panoramaW = 0;

    int  m_width = 0, m_height = 0;

    // Single source of truth for the view pose: mouse-look, movement and the
    // crosshair pick all read these same yaw/pitch values.
    Camera m_camera;

    float m_time = 0.0f;

    // 2D<->3D view morph: environment visibility (1 = steady room) and the
    // window pipeline mode it selects. See setEnvAlpha.
    float m_envAlpha = 1.0f;
    bool  m_windowsPremultiplied = false;

    // F3 debug HUD state.
    bool                            m_debugOverlay = false;
    bool                            m_crosshairVisible = true;
    bool                            m_gridVisible  = true;
    float                           m_zoom         = 1.0f;
    CPlayerModel                    m_player;
    // Player debug capsule (F3).
    unsigned int                    m_pDbgProgram = 0, m_pDbgVAO = 0, m_pDbgVBO = 0;
    int                             m_pDbgMVP = -1;
    int                             m_pDbgVerts = 0;
    bool                            m_pDbgOn = false;
    Vec3                            m_pDbgCenter{};
    void drawPlayerDebugCapsule(const Mat4& vp);
    SPlayerCfg                      m_playerCfg;
    std::string                     m_playerPath;      // tilde-expanded
    std::filesystem::file_time_type m_playerMtime{};
    bool                            m_playerMtimeValid = false;
    bool                            m_playerVisible    = false;
    Vec3                            m_playerFeet{};
    float                           m_playerYaw        = 0.f;
    float                           m_debugFps     = 0.f;

    unsigned int                    m_textVAO = 0, m_textVBO = 0;

    // Scene slots: one per config object, index-aligned with the specs.
    struct SSlot {
        std::unique_ptr<CMapModel>      model;
        SSceneSpec                      spec{};
        std::string                     loadedPath; // expanded
        std::filesystem::file_time_type mtime{};
        bool                            mtimeValid = false;
    };
    std::vector<SSlot>              m_slots;

    bool          m_probeRequested = false;
    bool          m_probeValid     = false;
    unsigned char m_probe[4]       = {0, 0, 0, 0};
};

} // namespace H3D

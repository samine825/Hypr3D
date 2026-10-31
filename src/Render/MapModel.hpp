#pragma once

#include "World/Camera.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace H3D {

// A glTF 2.0 map (.glb or .gltf) placed in the room: GL meshes, decoded
// textures and world-space collision triangles. Owns its GL objects; poll()
// and destroy() must run with the EGL context current (they are called from
// GLScene::render, like the panorama refresh). The file is read and decoded
// on a worker thread.
class CMapModel {
  public:
    CMapModel();
    ~CMapModel();

    // World-space triangle (the map transform is already applied). This is
    // what collision consumes.
    struct STL {
        Vec3 a, b, c;
    };

    enum class ECenter : uint8_t { Logical, Origin };

    // rotationDeg is XYZ Euler angles in degrees. Starts reading and
    // decoding the file on a worker thread; poll() makes the GL objects once
    // that is done. mtime is NOT checked here -- the caller decides when the
    // file changed.
    bool load(const std::string& path, const Vec3& position,
              const Vec3& rotationDeg, const Vec3& scale);
    // Finishes a load whose worker is done. Every frame, context current.
    void poll();
    void destroy();

    // A load is still being read and decoded, or its textures uploaded.
    bool pending() const {
        return m_worker.joinable() || m_uploading;
    }

    // The last load could not read the file; it is not retried until the
    // caller loads again.
    bool failed() const {
        return m_failed;
    }

    bool loaded() const {
        return m_loaded;
    }

    // The map's own transform, remembered for the draw pass.
    void setTransform(const Vec3& position, const Vec3& rotationDeg,
                      const Vec3& scale);

    // The rotation pivot: Logical = the mesh's local AABB center, Origin =
    // the mesh's own origin. centerOffset adds on top in local units.
    void setCenter(ECenter mode, const Vec3& offset);

    // Room light level shared by every scene model (1 = as authored, 0 =
    // black); self-lit models (screens, neon, glow quads) ignore it.
    static inline float s_lightLevel = 1.0f;
    static inline float s_darkLevel  = 0.15f; // the level "lights off" fades to
    void setSelfLit(bool on) {
        m_selfLit = on;
    }
    // This model's own lights-off level (< 0 = follow the room): a neon tube
    // can stay visibly coloured while the room goes black, a glow quad can
    // vanish (blended materials fade their alpha with the level too).
    void setDarkLevel(float d) {
        m_darkLevel = d;
    }
    // Power groups (hypr3d.power): an off group blacks out the emissive
    // output (screens, lamps) and fades the blended haze of this model's
    // primitives -- all of them, or only those whose material is listed.
    // Lit surfaces keep following the room light.
    static inline std::unordered_map<std::string, float> s_power; // group -> 0..1
    // Screens: emissive primitives (only power_materials ones when listed)
    // ignore the room light -- they go dark only through their power group.
    void setScreens(bool on, std::vector<std::string> materials = {}) {
        m_screens         = on;
        m_screenMaterials = std::move(materials);
    }
    // groups: each group with the materials it covers (empty = all, or the
    // shared power_materials list when set).
    using PowerList = std::vector<std::pair<std::string, std::vector<std::string>>>;
    void setPower(PowerList groups, std::vector<std::string> materials) {
        m_powerGroups    = std::move(groups);
        m_powerMaterials = std::move(materials);
    }
    // 1 = on; the lowest level of the groups covering this material, or -1
    // when no group covers it.
    float powerLevel(const std::string& material) const {
        float p = -1.0f;
        for (const auto& [G, MATS] : m_powerGroups) {
            const auto& LIST = MATS.empty() ? m_powerMaterials : MATS;
            if (!LIST.empty() && std::find(LIST.begin(), LIST.end(), material) == LIST.end())
                continue;
            const auto IT = s_power.find(G);
            const float L = IT != s_power.end() ? IT->second : 1.0f;
            p = p < 0.0f ? L : std::min(p, L);
        }
        return p;
    }

    float lightLevel() const {
        if (m_selfLit)
            return 1.0f;
        if (m_darkLevel < 0.0f)
            return s_lightLevel;
        const float SPAN = 1.0f - s_darkLevel;
        const float T = SPAN > 1e-4f ? (s_lightLevel - s_darkLevel) / SPAN
                                     : (s_lightLevel >= 1.0f ? 1.0f : 0.0f);
        const float C = T < 0.0f ? 0.0f : (T > 1.0f ? 1.0f : T);
        return m_darkLevel + (1.0f - m_darkLevel) * C;
    }

    // Global emissive multiplier (config); 0 = pure lambert, 1 = as authored.
    void setEmissiveScale(float s) {
        m_emissiveScale = s > 0.f ? s : 0.f;
    }

    // Baked-map mode (default): textures carry all lighting, so the shader
    // shows albedo/emission as-is with no dynamic light -- a lambert term
    // re-shades baked shadows and greys out down-facing light fixtures.
    void setFlat(bool flat) {
        m_flat = flat;
    }

    // cameraPos orders the blend pass far-to-near (translucent surfaces
    // need back-to-front to composite correctly against each other).
    void draw(const Mat4& vp, const Vec3& cameraPos) const;

    // Closest world-space hit of a ray against this model's triangles, or
    // -1. Used to grab dynamic (static = false) scene objects.
    float rayCast(const Vec3& origin, const Vec3& dir) const;

    const Vec3& position() const {
        return m_position;
    }

    const Vec3& rotationDeg() const {
        return m_rotationDeg;
    }

    bool hasPath() const {
        return !m_path.empty();
    }

    // Wireframe of the world-space collision triangles (red, x-ray). The
    // line buffer is (re)built on the next draw whenever the triangles were
    // recomputed (m_debugPending) -- scale/rotation changes included.
    void setDebugCollisions(bool on);
    void drawDebug(const Mat4& vp);

    const std::vector<STL>& triangles() const {
        return m_triangles;
    }

    // Node-space triangles (no transform). Physics bodies map these into
    // BODY space (scale + pivot shift), so the body transform alone places
    // them in the world exactly like the render matrix does.
    const std::vector<STL>& localTriangles() const {
        return m_localTriangles;
    }

    // The resolved local rotation pivot (center mode + offset applied) and
    // the current scale -- both feed the body-space mapping above.
    const Vec3& pivot() const {
        return m_pivot;
    }

    const Vec3& scale() const {
        return m_scale;
    }

    // Bump this after every (re)load so collision can rebuild its BVH.
    uint32_t generation() const {
        return m_generation;
    }

    // Bumps only when the MESH CONTENT changes (load/destroy) -- generation
    // also bumps on transform moves, which must not recreate physics bodies.
    uint32_t meshVersion() const {
        return m_meshVersion;
    }

  private:
    // Node-space collision triangles; the world-space set (m_triangles) is
    // recomputed from these whenever the config transform changes.
    void recomputeTriangles();

    // Resolves m_pivot from the center mode + offset (local units).
    void resolvePivot();

    struct SPrimitive {
        unsigned int vao = 0, vbo = 0, ebo = 0;
        int          count = 0;   // index count (or vertex count unindexed)
        bool         indexed = false;
        unsigned int texture = 0; // 0 = untextured (white fallback)
        float        color[4] = {1.f, 1.f, 1.f, 1.f};
        unsigned int emissiveTex = 0;
        float        emissiveFactor[3] = {1.f, 1.f, 1.f};
        float        emissiveStrength = 1.0f;

        // glTF alphaMode: 0 opaque, 1 mask (uAlphaCutoff), 2 blend.
        int          alphaMode = 0;
        float        alphaCutoff = 0.5f;
        Vec3         centroid{}; // node-space AABB center, for blend sorting
        std::string  material;   // glTF material name (power_materials)
    };

    // The file's meshes and images, read and decoded without GL.
    struct SDecoded;
    static std::unique_ptr<SDecoded> decode(const std::string& path,
                                            std::stop_token stop);
    void finishUpload();
    void dropUpload();
    void releaseMesh();
    void stopWorker();

    std::vector<SPrimitive> m_primitives;
    std::vector<unsigned>   m_ownedTextures; // unique image textures
    std::vector<STL>        m_localTriangles; // node space
    std::vector<STL>        m_triangles;      // world space (recomputed)
    std::string             m_path;
    Vec3                    m_position{};
    Vec3                    m_rotationDeg{};
    Vec3                    m_scale{1.0f, 1.0f, 1.0f};
    ECenter                 m_center = ECenter::Logical;
    Vec3                    m_centerOffset{};
    Vec3                    m_pivot{}; // resolved local pivot
    float                   m_emissiveScale = 1.0f;
    bool                    m_flat = false; // false = headlight shading
    bool                    m_loaded = false;
    uint32_t                m_generation = 0;
    uint32_t                m_meshVersion = 0;

    // Collision wireframe.
    unsigned int            m_debugVAO = 0, m_debugVBO = 0;
    int                     m_debugVerts = 0;
    bool                    m_debugOn = false;
    bool                    m_debugPending = false; // triangles changed since build
    unsigned int            m_debugBuiltGen = 0;

    unsigned int            m_debugProgram = 0;
    int                     m_debugMVP = -1;

    unsigned int            m_program = 0;
    int                     m_uMVP = -1;
    int                     m_uModel = -1;
    int                     m_uColor = -1;
    int                     m_uTex = -1;
    int                     m_uEmissive = -1;
    int                     m_uHasEmissive = -1;
    int                     m_uEmissiveFactor = -1;
    int                     m_uEmissiveStrength = -1;
    int                     m_uFlat = -1;
    int                     m_uLight = -1;
    bool                    m_selfLit = false;
    float                   m_darkLevel = -1.0f;
    PowerList               m_powerGroups;
    std::vector<std::string> m_powerMaterials, m_screenMaterials;
    bool                    m_screens = false;
    int                     m_uPower = -1;
    int                     m_uAlphaMode = -1;
    int                     m_uAlphaCutoff = -1;

    std::string               m_meshPath; // the file the GL mesh came from
    bool                      m_failed = false;
    std::unique_ptr<SDecoded> m_decoded;  // the worker's result
    // A decoded file whose textures go up one per frame, then its buffers.
    std::unique_ptr<SDecoded> m_uploading;
    std::vector<unsigned>     m_uploadTextures; // by image index, 0 = none
    size_t                    m_uploadNext = 0;
    std::atomic<bool>         m_decodedReady{false};
    // Last, so it is destroyed -- joined -- before what its thread writes.
    std::jthread              m_worker;
};

} // namespace H3D

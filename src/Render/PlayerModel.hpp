#pragma once

#include "World/Camera.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace H3D {

// The player's own glTF character. Node animation is evaluated on the CPU
// every frame (channels -> node TRS -> world matrices -> vertices -> upload):
// plain rigid per-node meshes, no skinning/joints -- what simple character
// exports use. load()/destroy()/update()/draw() need the EGL context current
// (they run from GLScene::render, like the map models).
class CPlayerModel {
  public:
    enum class EState : uint8_t { Idle, Walk, Run, Jump };
    static constexpr int kStateCount = 4;
    static constexpr float kBlendDuration = 0.25f; // state crossfade, s

    // Quaternion (glTF rotation channels); minimal, test-visible.
    struct SQuat {
        float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
        static SQuat slerp(const SQuat& a, const SQuat& b, float u);
    };

    // One animation sampler: keyframe times + values (3 = vec3, 4 = quat).
    // Handles linear, step and cubicspline (cubicspline reads the key VALUE
    // component and lerps linearly -- tangents are ignored).
    struct SSampler {
        int                interp = 0; // 0 linear, 1 step, 2 cubicspline
        int                comps = 3;  // components per key value
        float              duration = 0.f;
        std::vector<float> times;
        std::vector<float> values;

        void sample(float t, float* out) const; // out = comps floats
    };

    bool load(const std::string& path);
    void destroy();

    bool loaded() const {
        return m_loaded;
    }

    int animationCount() const {
        return static_cast<int>(m_anims.size());
    }

    // Shader link status (test/diagnostic hook).
    bool programValid() const {
        return m_program != 0;
    }

    unsigned int programId() const {
        return m_program;
    }

    // The node index of a skin's joint (diagnostic hook).
    int skinJointNode(int skin, int joint) const {
        if (skin < 0 || skin >= static_cast<int>(m_skins.size()))
            return -1;
        const auto& SK = m_skins[skin];
        return joint >= 0 && joint < static_cast<int>(SK.joints.size())
                   ? SK.joints[joint]
                   : -1;
    }

    int primCount() const {
        return static_cast<int>(m_prims.size());
    }

    int primNode(int prim) const {
        return prim >= 0 && prim < static_cast<int>(m_prims.size())
                   ? m_prims[prim].node
                   : -1;
    }

    unsigned int primVBO(int prim) const {
        return prim >= 0 && prim < static_cast<int>(m_prims.size())
                   ? m_prims[prim].vbo
                   : 0;
    }

    unsigned int primVAO(int prim) const {
        return prim >= 0 && prim < static_cast<int>(m_prims.size())
                   ? m_prims[prim].vao
                   : 0;
    }

    unsigned int primTexture(int prim) const {
        return prim >= 0 && prim < static_cast<int>(m_prims.size())
                   ? m_prims[prim].texture
                   : 0;
    }

    float animDuration(int index) const {
        return index >= 0 && index < static_cast<int>(m_anims.size())
                   ? m_anims[index].duration
                   : 0.f;
    }

    // Per-state animation assignment: -1 = none (the state keeps the last
    // pose). By index, or by animation name (false when there is no such
    // animation).
    void setAnim(EState state, int index);
    bool setAnim(EState state, const std::string& name);

    // World placement of the model's ORIGIN (feet for character exports):
    // anchor offset (rotates with the model), facing (yaw radians),
    // per-axis scale. Also the material overrides from the shared mesh
    // description.
    void setPose(const Vec3& feet, float yawRad, const Vec3& scale,
                 const Vec3& offset, const Vec3& rotDeg);
    void setFlat(bool flat) {
        m_flat = flat;
    }

    // Playback speed multiplier for a state's clip (1 = as authored).
    void setAnimSpeed(EState state, float speed) {
        if (state >= EState::Idle && state <= EState::Jump &&
            speed > 0.01f && std::isfinite(speed))
            m_animSpeed[static_cast<int>(state)] = speed;
    }
    void setEmissiveScale(float s) {
        m_emissiveScale = s > 0.f ? s : 0.f;
    }

    // Switch the animated state; Jump restarts and HOLDS its last frame,
    // the looped states wrap. The previous state keeps playing and the two
    // poses crossfade over kBlendDuration, so state changes never pop.
    void setState(EState state) {
        if (m_state == state)
            return;
        m_prevState = m_state;
        m_prevTime  = m_time;
        m_state     = state;
        m_time      = 0.f;
        m_blend     = 0.f;
    }

    float blend() const {
        return m_blend;
    }

    // Advance the clock and evaluate the animation into the vertex buffers.
    void update(float dt);

    void draw(const Mat4& vp, const Vec3& cameraPos) const;

    // --- test hooks --------------------------------------------------------
    int nodeCount() const {
        return static_cast<int>(m_nodes.size());
    }

    // World (model-space) transform of a node as evaluated by the last
    // update() call.
    Mat4 nodeWorld(int index) const;

    // Diagnostic hooks.
    int evalOrderSize() const {
        return static_cast<int>(m_order.size());
    }

    Vec3 nodeRestTranslation(int index) const {
        return index >= 0 && index < static_cast<int>(m_nodes.size())
                   ? m_nodes[index].t
                   : Vec3{};
    }

    // The interleaved buffer a primitive last uploaded (test hook).
    const std::vector<float>& lastUploaded(int prim) const;

    // Joint matrix (jointWorld * inverseBind) of a skin's joint, as
    // evaluated by the last update() (test hook).
    Mat4 jointMat(int skin, int joint) const;

    // CPU skinning of a primitive at the CURRENT pose, regardless of the
    // GPU path -- the test hook that verifies the skinning math.
    std::vector<float> cpuSkinnedVerts(int prim) const;

  private:
    struct SNode {
        int   parent = -1;
        Vec3  t{};
        float r[4] = {0.f, 0.f, 0.f, 1.f}; // rest local rotation
        Vec3  s{1.f, 1.f, 1.f};
    };

    struct SChannel {
        int node   = -1;
        int path   = 0; // 0 translation, 1 rotation, 2 scale
        int sampler = -1;
    };

    struct SAnim {
        std::string              name;
        float                    duration = 0.f;
        std::vector<SChannel>    channels;
    };

    struct SPrim {
        unsigned int vao = 0, vbo = 0, ebo = 0;
        int          count = 0;
        bool         indexed = false;
        unsigned int texture = 0;
        float        color[4] = {1.f, 1.f, 1.f, 1.f};
        int          node = -1;
        // Base (node-space) vertices: positions + normals are transformed
        // and re-uploaded every update(); the uvs are static.
        std::vector<float> basePos;
        std::vector<float> baseNrm;
        std::vector<float> baseUv;
        // Skeletal skinning (JOINTS_0 / WEIGHTS_0): empty for rigid meshes.
        // gpuSkinned: the bone palette rides a uniform array and the vertex
        // data is static; otherwise the CPU transforms vertices per frame.
        int                   skin = -1;
        bool                  gpuSkinned = false;
        std::vector<uint16_t> joints;  // 4 per vertex, node indices
        std::vector<float>    weights; // 4 per vertex
        Mat4                  lastWorld{};
        bool                  uploadedOnce = false;
        // The last interleaved buffer uploaded (test hook).
        std::vector<float> uploaded;
    };

    struct SSkin {
        std::vector<int>  joints;   // node indices
        std::vector<Mat4> invBind;  // per joint (as authored)
    };

    // Rest TRS + one state's animation channels -> local transforms.
    void evalLocals(EState state, float time, std::vector<Vec3>& T,
                    std::vector<SQuat>& R, std::vector<Vec3>& S) const;

    // Local TRS -> world matrix pass, parents first (m_order).
    void evaluateNodes();

    // The interleaved vertex buffer for a primitive at the CURRENT pose
    // (rigid follow or CPU skinning).
    std::vector<float> buildVerts(const SPrim& P) const;

    std::vector<SNode>     m_nodes;
    std::vector<int>       m_order;   // depth-sorted node indices
    std::vector<Mat4>      m_world;   // evaluated node worlds
    std::vector<SAnim>     m_anims;
    std::vector<SSampler>  m_samplers;
    std::vector<SSkin>     m_skins;
    std::vector<std::vector<Mat4>> m_skinMats; // jointWorld * invBind, per skin
    // Per skin: true = the mesh node's world goes before the palette (the
    // bind bakes it in), false = glTF spec (see load()).
    std::vector<bool>      m_skinMeshWorld;
    std::vector<SPrim>     m_prims;

    EState m_state      = EState::Idle;
    EState m_prevState  = EState::Idle;
    float  m_prevTime   = 0.f;
    float  m_blend      = 1.f; // 0 = the previous pose, 1 = the current
    int    m_animFor[kStateCount] = {-1, -1, -1, -1};
    float  m_animSpeed[kStateCount] = {1.f, 1.f, 1.f, 1.f};
    float  m_time = 0.f;
    Vec3   m_feet{};
    float  m_yaw = 0.f;
    Vec3   m_offset{};
    Vec3   m_scale{1.f, 1.f, 1.f};
    Vec3   m_rotDeg{}; // authored-facing correction, XYZ degrees
    bool   m_flat = false;
    float  m_emissiveScale = 1.0f;

    bool   m_loaded = false;
    std::string m_path;

    unsigned int m_program = 0;
    int          m_uMVP = -1, m_uModel = -1, m_uColor = -1, m_uTex = -1,
        m_uHasTex = -1, m_uCamPos = -1, m_uJoints = -1, m_uSkinned = -1,
        m_uMeshWorld = -1, m_uFlat = -1;
};

} // namespace H3D

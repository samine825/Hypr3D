#include "Render/PlayerModel.hpp"

#include "../../third_party/cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb_image.h"

#include <GLES3/gl32.h>

// The test stubs carry only a subset of the GL constants.
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif

#include <hyprgraphics/image/Image.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace H3D {

namespace {

constexpr float PI = 3.14159265358979f;

constexpr int MAX_JOINTS = 128;

constexpr const char* PLAYER_VERTEX = R"GLSL(
#version 320 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aJoint;
layout(location = 4) in vec4 aWeight;

#define MAX_JOINTS 128
uniform mat4 uJoints[MAX_JOINTS];
uniform bool uSkinned;
uniform mat4 uMeshWorld;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec3 vNrm;
out vec2 vUv;
out vec3 vWorld;

void main() {
    // GPU linear blend skinning: the joint palette (jointWorld * invBind)
    // arrives as a uniform; vertex data is static. The mesh node's world
    // takes the vertex into the bind space BEFORE the palette (three.js /
    // Godot convention) -- exporters put scales/rotations there.
    mat4 skin = mat4(1.0);
    vec3 P = aPos;
    vec3 N = aNrm;
    if (uSkinned) {
        P = (uMeshWorld * vec4(aPos, 1.0)).xyz;
        N = mat3(uMeshWorld) * aNrm;
        skin = aWeight.x * uJoints[int(aJoint.x)] +
               aWeight.y * uJoints[int(aJoint.y)] +
               aWeight.z * uJoints[int(aJoint.z)] +
               aWeight.w * uJoints[int(aJoint.w)];
        P = (skin * vec4(P, 1.0)).xyz;
        N = mat3(skin) * N;
    }

    vec4 w = uModel * vec4(P, 1.0);
    vWorld = w.xyz;
    vNrm = mat3(uModel) * N;
    vUv = aUv;
    gl_Position = uMVP * vec4(P, 1.0);
}
)GLSL";

// Headlight half-lambert: a character needs SOME shading to read in 3D,
// but baked/flat lighting keeps the look predictable.
constexpr const char* PLAYER_FRAGMENT = R"GLSL(
#version 320 es
precision mediump float;

in vec3 vNrm;
in vec2 vUv;
in vec3 vWorld;

uniform sampler2D uTex;
uniform bool uHasTex;
uniform bool uFlat;
uniform vec4 uColor;
uniform vec3 uCamPos;

out vec4 fragColor;

void main() {
    vec3 N = normalize(vNrm);
    vec3 L = normalize(uCamPos - vWorld);
    // flat: the texture carries all lighting (baked/toon characters)
    float diff = uFlat ? 1.0 : dot(N, L) * 0.5 + 0.5; // half-lambert
    vec4 base = uHasTex ? texture(uTex, vUv) : vec4(1.0);
    fragColor = vec4(base.rgb * uColor.rgb * diff, base.a * uColor.a);
}
)GLSL";

GLuint compileShader(GLenum type, const char* src) {
    const GLuint SH = glCreateShader(type);
    glShaderSource(SH, 1, &src, nullptr);
    glCompileShader(SH);

    GLint ok = GL_FALSE;
    glGetShaderiv(SH, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(SH);
        return 0;
    }

    return SH;
}

Mat4 quatToMat4(const CPlayerModel::SQuat& q) {
    const float X = q.x, Y = q.y, Z = q.z, W = q.w;
    // Column-major, same convention as the rest of H3D math.
    Mat4 m{};
    m.m[0]  = 1.f - 2.f * (Y * Y + Z * Z);
    m.m[1]  = 2.f * (X * Y + W * Z);
    m.m[2]  = 2.f * (X * Z - W * Y);
    m.m[4]  = 2.f * (X * Y - W * Z);
    m.m[5]  = 1.f - 2.f * (X * X + Z * Z);
    m.m[6]  = 2.f * (Y * Z + W * X);
    m.m[8]  = 2.f * (X * Z + W * Y);
    m.m[9]  = 2.f * (Y * Z - W * X);
    m.m[10] = 1.f - 2.f * (X * X + Y * Y);
    m.m[15] = 1.f; // closes the matrix: without it, T * (R * S) drops the
                   // node's translation (t * R.m[15] = 0) and the whole
                   // skeleton collapses onto the origin
    return m;
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
    };
}

Vec3 transformDir(const Mat4& m, const Vec3& p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z,
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z,
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z,
    };
}

unsigned int uploadGLTex(const std::vector<unsigned char>& pixels, int W,
                         int H) {
    if (W <= 0 || H <= 0 || pixels.empty())
        return 0;

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

unsigned int uploadTexture(const std::string& modelDir, const cgltf_image* image) {
    // Primary path: stb_image. It decodes JPEG/PNG/BMP/TGA/... straight
    // into RGBA -- glTF models in the wild embed JPEG far more often than
    // PNG, and straight alpha avoids cairo's premultiplied-ARGB traps.
    if (image) {
        std::vector<unsigned char> PIX;
        int W = 0, H = 0;

        if (image->buffer_view && image->buffer_view->buffer &&
            image->buffer_view->buffer->data) {
            // Embedded texture (.glb): the image bytes live in the buffer
            // view.
            const auto* BV = image->buffer_view;
            const auto* DATA =
                static_cast<const uint8_t*>(BV->buffer->data) + BV->offset;
            int COMP = 0;
            unsigned char* DECODED = stbi_load_from_memory(
                DATA, static_cast<int>(BV->size), &W, &H, &COMP, 4);
            if (DECODED) {
                PIX.assign(DECODED, DECODED + static_cast<size_t>(W) * H * 4);
                stbi_image_free(DECODED);
            }
        } else if (image->uri &&
                   !std::string_view{image->uri}.starts_with("data:")) {
            int COMP = 0;
            unsigned char* DECODED = stbi_load(
                (modelDir + "/" + image->uri).c_str(), &W, &H, &COMP, 4);
            if (DECODED) {
                PIX.assign(DECODED, DECODED + static_cast<size_t>(W) * H * 4);
                stbi_image_free(DECODED);
            }
        }

        if (!PIX.empty())
            return uploadGLTex(PIX, W, H);
        // stb failed: fall through to the cairo path (exotic formats).
    }

    // Fallback: hyprgraphics/cairo.
    std::unique_ptr<Hyprgraphics::CImage> img;

    if (image && image->buffer_view && image->buffer_view->buffer &&
        image->buffer_view->buffer->data) {
        const auto* BV = image->buffer_view;
        const auto* DATA = static_cast<const uint8_t*>(BV->buffer->data) + BV->offset;
        img = std::make_unique<Hyprgraphics::CImage>(
            std::span<const uint8_t>{DATA, BV->size},
            Hyprgraphics::IMAGE_FORMAT_AUTO);
    } else if (image && image->uri &&
               !std::string_view{image->uri}.starts_with("data:"))
        img = std::make_unique<Hyprgraphics::CImage>(modelDir + "/" + image->uri);
    else
        return 0; // data-URI textures: skipped

    auto surface = (img && img->success()) ? img->cairoSurface() : nullptr;
    if (!surface || surface->status() != CAIRO_STATUS_SUCCESS)
        return 0;

    const int W      = static_cast<int>(surface->size().x);
    const int H      = static_cast<int>(surface->size().y);
    const int STRIDE = surface->stride();
    const auto* SRC  = surface->data();
    if (W <= 0 || H <= 0 || !SRC)
        return 0;

    std::vector<unsigned char> pixels(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        const auto* row = SRC + static_cast<size_t>(y) * STRIDE;
        auto* dst = pixels.data() + static_cast<size_t>(y) * W * 4;
        for (int x = 0; x < W; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 2];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 0];
            dst[x * 4 + 3] = row[x * 4 + 3];
        }
    }

    return uploadGLTex(pixels, W, H);
}

} // namespace

// --- quaternions ------------------------------------------------------------

CPlayerModel::SQuat CPlayerModel::SQuat::slerp(const SQuat& a, const SQuat& b,
                                               float u) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    SQuat B = b;
    if (d < 0.f) { // shortest arc
        B = SQuat{-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }

    if (d > 0.9995f) { // nearly identical: lerp + normalize
        SQuat R{a.x + (B.x - a.x) * u, a.y + (B.y - a.y) * u,
                a.z + (B.z - a.z) * u, a.w + (B.w - a.w) * u};
        const float L = std::sqrt(R.x * R.x + R.y * R.y + R.z * R.z + R.w * R.w);
        if (L > 1e-8f) {
            R.x /= L;
            R.y /= L;
            R.z /= L;
            R.w /= L;
        }
        return R;
    }

    const float TH0 = std::acos(std::clamp(d, -1.f, 1.f));
    const float S0  = std::sin((1.f - u) * TH0) / std::sin(TH0);
    const float S1  = std::sin(u * TH0) / std::sin(TH0);
    return SQuat{a.x * S0 + B.x * S1, a.y * S0 + B.y * S1,
                 a.z * S0 + B.z * S1, a.w * S0 + B.w * S1};
}

// --- sampling ---------------------------------------------------------------

void CPlayerModel::SSampler::sample(float t, float* out) const {
    const int N = static_cast<int>(times.size());
    if (N == 0)
        return;
    if (N == 1 || t <= times[0]) {
        std::memcpy(out, &values[0], sizeof(float) * comps);
        return;
    }
    if (t >= times[N - 1]) {
        const int LAST = (interp == 2 ? (N - 1) * comps * 3 + comps : (N - 1) * comps);
        std::memcpy(out, &values[LAST], sizeof(float) * comps);
        return;
    }

    // times are (assumed) ascending: linear scan is fine for character clips.
    int i = 0;
    while (i + 1 < N && times[i + 1] < t)
        ++i;

    const float T0 = times[i], T1 = times[i + 1];
    const float U = T1 > T0 ? (t - T0) / (T1 - T0) : 0.f;

    if (interp == 1) { // step
        std::memcpy(out, &values[i * comps], sizeof(float) * comps);
        return;
    }

    if (interp == 2) { // cubicspline: [inTan, value, outTan] per key
        const float* A = &values[(i * 3 + 1) * comps];
        const float* B = &values[(i * 3 + 3 + 1) * comps];
        for (int k = 0; k < comps; ++k)
            out[k] = A[k] + (B[k] - A[k]) * U;
        return;
    }

    const float* A = &values[i * comps];
    const float* B = &values[(i + 1) * comps];
    if (comps == 4) { // rotation: slerp
        const SQuat QA{A[0], A[1], A[2], A[3]};
        const SQuat QB{B[0], B[1], B[2], B[3]};
        const SQuat R = SQuat::slerp(QA, QB, U);
        out[0] = R.x;
        out[1] = R.y;
        out[2] = R.z;
        out[3] = R.w;
        return;
    }
    for (int k = 0; k < comps; ++k)
        out[k] = A[k] + (B[k] - A[k]) * U;
}

// --- anim assignment ----------------------------------------------------------

void CPlayerModel::setAnim(EState state, int index) {
    m_animFor[static_cast<int>(state)] = index;
}

bool CPlayerModel::setAnim(EState state, const std::string& name) {
    for (int i = 0; i < static_cast<int>(m_anims.size()); ++i) {
        if (m_anims[i].name == name) {
            m_animFor[static_cast<int>(state)] = i;
            return true;
        }
    }
    return false;
}

// --- pose / evaluation --------------------------------------------------------

void CPlayerModel::setPose(const Vec3& feet, float yawRad,
                           const Vec3& scale, const Vec3& offset,
                           const Vec3& rotDeg) {
    m_offset = offset;
    m_rotDeg = rotDeg;
    m_feet = feet;
    m_yaw  = yawRad;
    m_scale = Vec3{scale.x > 0.0001f ? scale.x : 0.0001f,
                   scale.y > 0.0001f ? scale.y : 0.0001f,
                   scale.z > 0.0001f ? scale.z : 0.0001f};
}

void CPlayerModel::evalLocals(EState state, float time,
                              std::vector<Vec3>& T, std::vector<SQuat>& R,
                              std::vector<Vec3>& S) const {
    // Rest TRS, then the state's animation channels override components.
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        T[i] = m_nodes[i].t;
        R[i] = SQuat{m_nodes[i].r[0], m_nodes[i].r[1], m_nodes[i].r[2],
                     m_nodes[i].r[3]};
        S[i] = m_nodes[i].s;
    }

    const int ANIM = m_animFor[static_cast<int>(state)];
    if (ANIM < 0 || ANIM >= static_cast<int>(m_anims.size()))
        return; // no clip for this state: the rest pose stands

    const auto& A = m_anims[ANIM];
    for (const auto& C : A.channels) {
        if (C.node < 0 || C.node >= static_cast<int>(m_nodes.size()) ||
            C.sampler < 0 || C.sampler >= static_cast<int>(m_samplers.size()))
            continue;

        float V[4];
        m_samplers[C.sampler].sample(time, V);

        switch (C.path) {
            case 0: T[C.node] = {V[0], V[1], V[2]}; break;
            case 1: R[C.node] = {V[0], V[1], V[2], V[3]}; break;
            case 2: S[C.node] = {V[0], V[1], V[2]}; break;
        }
    }
}

void CPlayerModel::evaluateNodes() {
    const size_t NN = m_nodes.size();

    std::vector<Vec3>  T(NN);
    std::vector<SQuat> R(NN);
    std::vector<Vec3>  S(NN);
    evalLocals(m_state, m_time, T, R, S);

    // Crossfade: while the blend runs, the previous state keeps playing and
    // its pose lerps/slerps into the current one.
    if (m_blend < 1.f && m_prevState != m_state) {
        std::vector<Vec3>  TP(NN);
        std::vector<SQuat> RP(NN);
        std::vector<Vec3>  SP(NN);
        evalLocals(m_prevState, m_prevTime, TP, RP, SP);

        const float B = m_blend;
        for (size_t i = 0; i < NN; ++i) {
            T[i] = TP[i] + (T[i] - TP[i]) * B;
            S[i] = SP[i] + (S[i] - SP[i]) * B;
            R[i] = SQuat::slerp(RP[i], R[i], B);
        }
    }

    m_world.assign(NN, Mat4::identity());
    for (const int I : m_order) {
        const Mat4 LOCAL = Mat4::translation(T[I]) * (quatToMat4(R[I]) *
            (Mat4::scale(S[I])));
        if (m_nodes[I].parent >= 0)
            m_world[I] = m_world[m_nodes[I].parent] * LOCAL;
        else
            m_world[I] = LOCAL;
    }
}

void CPlayerModel::update(float dt) {
    if (!m_loaded)
        return;

    const int ANIM = m_animFor[static_cast<int>(m_state)];
    const float DUR =
        (ANIM >= 0 && ANIM < static_cast<int>(m_anims.size()))
            ? m_anims[ANIM].duration
            : 0.f;

    if (dt > 0.f)
        m_time += dt * m_animSpeed[static_cast<int>(m_state)];

    if (DUR > 0.f) {
        if (m_state == EState::Jump)
            m_time = std::min(m_time, DUR); // hold the landing pose
        else
            m_time = std::fmod(m_time, DUR);
    }

    // The crossfade: the previous clip keeps playing while the blend eases
    // from its pose into the current one (0.25 s).
    if (m_blend < 1.f) {
        m_blend = std::min(1.f, m_blend + dt / 0.25f);

        const int PANIM = m_animFor[static_cast<int>(m_prevState)];
        const float PDUR =
            (PANIM >= 0 && PANIM < static_cast<int>(m_anims.size()))
                ? m_anims[PANIM].duration
                : 0.f;
        if (dt > 0.f)
            m_prevTime += dt * m_animSpeed[static_cast<int>(m_prevState)];
        if (PDUR > 0.f) {
            if (m_prevState == EState::Jump)
                m_prevTime = std::min(m_prevTime, PDUR);
            else
                m_prevTime = std::fmod(m_prevTime, PDUR);
        }
    }

    evaluateNodes();

    // Joint matrices per skin: jointWorld * inverseBind (the glTF spec).
    // This model's bind pose is LARGER than its rest hierarchy (the exporter
    // scaled the armature after skinning) -- the spec formula is what
    // reassembles the scattered bind-space vertices onto the skeleton.
    m_skinMats.assign(m_skins.size(), {});
    for (size_t s = 0; s < m_skins.size(); ++s) {
        const auto& SK = m_skins[s];
        auto& OUT = m_skinMats[s];
        OUT.resize(SK.joints.size(), Mat4::identity());
        for (size_t j = 0; j < SK.joints.size(); ++j) {
            const int N = SK.joints[j];
            const Mat4 WJ = N >= 0 && N < static_cast<int>(m_world.size())
                ? m_world[N] : Mat4::identity();
            OUT[j] = WJ * SK.invBind[j];
        }
    }

    // Transform + upload the primitives whose vertices live on the CPU.
    // GPU-skinned prims never rebuild here (their data is static and the
    // palette goes to draw() as a uniform); rigid prims rebuild only when
    // their node world actually moved.
    for (auto& P : m_prims) {
        if (P.basePos.empty() || P.gpuSkinned)
            continue;

        const Mat4& W = m_world[P.node];

        if (P.uploadedOnce) {
            const Mat4& LW = P.lastWorld;
            bool SAME = true;
            for (int c = 0; c < 16; ++c)
                if (LW.m[c] != W.m[c]) {
                    SAME = false;
                    break;
                }
            if (SAME)
                continue;
        }
        P.lastWorld    = W;
        P.uploadedOnce = true;

        P.uploaded = buildVerts(P);
        const auto& BUF = P.uploaded;

        glBindVertexArray(P.vao);
        glBindBuffer(GL_ARRAY_BUFFER, P.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(BUF.size() * sizeof(float)),
                     BUF.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }
}

// Builds a primitive's interleaved vertex buffer (pos3/nrm3/uv2) for the
// CURRENT pose: rigid prims follow their node world, CPU-skinned prims get
// linear blend skinning on top of the mesh node's world.
std::vector<float> CPlayerModel::buildVerts(const SPrim& P) const {
    const int NV = static_cast<int>(P.basePos.size() / 3);
    const bool SKINNED = P.skin >= 0 &&
        P.skin < static_cast<int>(m_skinMats.size()) && !P.joints.empty();
    const Mat4 W = m_world[P.node];
    // The space the skin's bind expects (see load()): a spec export skins
    // the raw vertex, without the mesh node's world.
    const bool MESH_WORLD = !SKINNED ||
        P.skin >= static_cast<int>(m_skinMeshWorld.size()) ||
        m_skinMeshWorld[P.skin];
    const Mat4 BIND = MESH_WORLD ? W : Mat4::identity();

    std::vector<float> buf;
    buf.reserve(static_cast<size_t>(NV) * 8);
    for (int v = 0; v < NV; ++v) {
        Vec3 POS;
        Vec3 NRM;

        if (SKINNED) {
                // Linear blend skinning: M = sum(w * jointWorld * invBind),
                // applied to the vertex taken into the bind space by the
                // mesh node's world FIRST (three.js / Godot convention --
                // exporters put scales/rotations on the mesh node).
                Mat4 M{};
                for (int k = 0; k < 4; ++k) {
                    const float WK = P.weights[v * 4 + k];
                    const auto JI = P.joints[v * 4 + k];
                    if (WK < 1e-6f ||
                        JI >= m_skinMats[P.skin].size())
                        continue;
                    const auto& JM = m_skinMats[P.skin][JI];
                    for (int c = 0; c < 16; ++c)
                        M.m[c] += WK * JM.m[c];
                }
                POS = transformPoint(M, transformPoint(BIND, {
                    P.basePos[v * 3 + 0], P.basePos[v * 3 + 1],
                    P.basePos[v * 3 + 2]}));
                NRM = transformDir(M, transformDir(BIND, {
                    P.baseNrm[v * 3 + 0], P.baseNrm[v * 3 + 1],
                    P.baseNrm[v * 3 + 2]}));
            } else {
                POS = transformPoint(W, {P.basePos[v * 3 + 0],
                                         P.basePos[v * 3 + 1],
                                         P.basePos[v * 3 + 2]});
                NRM = transformDir(W, {P.baseNrm[v * 3 + 0],
                                       P.baseNrm[v * 3 + 1],
                                       P.baseNrm[v * 3 + 2]});
            }

            const float L = std::sqrt(NRM.x * NRM.x + NRM.y * NRM.y + NRM.z * NRM.z);
            buf.push_back(POS.x);
            buf.push_back(POS.y);
            buf.push_back(POS.z);
            buf.push_back(L > 1e-8f ? NRM.x / L : 0.f);
            buf.push_back(L > 1e-8f ? NRM.y / L : 0.f);
            buf.push_back(L > 1e-8f ? NRM.z / L : 0.f);
            buf.push_back(v < static_cast<int>(P.baseUv.size() / 2)
                              ? P.baseUv[v * 2 + 0]
                              : 0.f);
            buf.push_back(v < static_cast<int>(P.baseUv.size() / 2)
                              ? P.baseUv[v * 2 + 1]
                              : 0.f);
    }

    return buf;
}

std::vector<float> CPlayerModel::cpuSkinnedVerts(int prim) const {
    if (prim < 0 || prim >= static_cast<int>(m_prims.size()))
        return {};
    const auto& P = m_prims[prim];
    if (P.basePos.empty() || P.skin < 0 || P.joints.empty())
        return {};
    return buildVerts(P);
}

Mat4 CPlayerModel::jointMat(int skin, int joint) const {
    static const Mat4 I = Mat4::identity();
    if (skin < 0 || skin >= static_cast<int>(m_skinMats.size()))
        return I;
    const auto& JM = m_skinMats[skin];
    if (joint < 0 || joint >= static_cast<int>(JM.size()))
        return I;
    return JM[joint];
}

const std::vector<float>& CPlayerModel::lastUploaded(int prim) const {
    static const std::vector<float> EMPTY;
    return prim >= 0 && prim < static_cast<int>(m_prims.size())
               ? m_prims[prim].uploaded
               : EMPTY;
}

Mat4 CPlayerModel::nodeWorld(int index) const {
    if (index < 0 || index >= static_cast<int>(m_world.size()))
        return Mat4::identity();
    return m_world[index];
}

// --- load / destroy -----------------------------------------------------------

bool CPlayerModel::load(const std::string& path) {
    destroy();

    m_path = path;

    cgltf_options options{};
    cgltf_data*   data = nullptr;

    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        return false;
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    const auto NODE_IDX = [&](const cgltf_node* N) {
        return N ? static_cast<int>(N - data->nodes) : -1;
    };

    // Nodes: rest TRS (decompose a full matrix when TRS is absent).
    m_nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto& N  = data->nodes[i];
        auto&       S  = m_nodes[i];
        S.parent       = NODE_IDX(N.parent);

        if (N.has_translation)
            S.t = {N.translation[0], N.translation[1], N.translation[2]};
        if (N.has_rotation) {
            S.r[0] = N.rotation[0];
            S.r[1] = N.rotation[1];
            S.r[2] = N.rotation[2];
            S.r[3] = N.rotation[3];
        }
        if (N.has_scale)
            S.s = {N.scale[0], N.scale[1], N.scale[2]};

        if (N.has_matrix && !N.has_translation && !N.has_rotation && !N.has_scale) {
            const float* m = N.matrix;
            S.t = {m[12], m[13], m[14]};
            const float CX[3] = {m[0], m[1], m[2]};
            const float CY[3] = {m[4], m[5], m[6]};
            const float CZ[3] = {m[8], m[9], m[10]};
            const float SX = std::sqrt(CX[0] * CX[0] + CX[1] * CX[1] + CX[2] * CX[2]);
            const float SY = std::sqrt(CY[0] * CY[0] + CY[1] * CY[1] + CY[2] * CY[2]);
            const float SZ = std::sqrt(CZ[0] * CZ[0] + CZ[1] * CZ[1] + CZ[2] * CZ[2]);
            S.s = {SX, SY, SZ};
            if (SX > 1e-8f && SY > 1e-8f && SZ > 1e-8f) {
                // Rotation matrix -> quaternion (columns normalized).
                const float R[3][3] = {
                    {CX[0] / SX, CY[0] / SY, CZ[0] / SZ},
                    {CX[1] / SX, CY[1] / SY, CZ[1] / SZ},
                    {CX[2] / SX, CY[2] / SY, CZ[2] / SZ},
                };
                const float TR = R[0][0] + R[1][1] + R[2][2];
                float       q[4];
                if (TR > 0.f) {
                    const float S4 = std::sqrt(TR + 1.f) * 2.f;
                    q[3] = 0.25f * S4;
                    q[0] = (R[2][1] - R[1][2]) / S4;
                    q[1] = (R[0][2] - R[2][0]) / S4;
                    q[2] = (R[1][0] - R[0][1]) / S4;
                } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
                    const float S4 = std::sqrt(1.f + R[0][0] - R[1][1] - R[2][2]) * 2.f;
                    q[3] = (R[2][1] - R[1][2]) / S4;
                    q[0] = 0.25f * S4;
                    q[1] = (R[0][1] + R[1][0]) / S4;
                    q[2] = (R[0][2] + R[2][0]) / S4;
                } else if (R[1][1] > R[2][2]) {
                    const float S4 = std::sqrt(1.f + R[1][1] - R[0][0] - R[2][2]) * 2.f;
                    q[3] = (R[0][2] - R[2][0]) / S4;
                    q[0] = (R[0][1] + R[1][0]) / S4;
                    q[1] = 0.25f * S4;
                    q[2] = (R[1][2] + R[2][1]) / S4;
                } else {
                    const float S4 = std::sqrt(1.f + R[2][2] - R[0][0] - R[1][1]) * 2.f;
                    q[3] = (R[1][0] - R[0][1]) / S4;
                    q[0] = (R[0][2] + R[2][0]) / S4;
                    q[1] = (R[1][2] + R[2][1]) / S4;
                    q[2] = 0.25f * S4;
                }
                S.r[0] = q[0];
                S.r[1] = q[1];
                S.r[2] = q[2];
                S.r[3] = q[3];
            }
        }
    }

    // Evaluation order: parents before children (depth sort).
    m_order.clear();
    m_order.reserve(m_nodes.size());
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        int depth = 0;
        for (int p = m_nodes[i].parent; p >= 0 && depth < 256; p = m_nodes[p].parent)
            ++depth;
        (void)depth;
        m_order.push_back(static_cast<int>(i));
    }
    std::stable_sort(m_order.begin(), m_order.end(), [&](int A, int B) {
        const auto DEPTH = [&](int I) {
            int d = 0;
            for (int p = m_nodes[I].parent; p >= 0 && d < 256; p = m_nodes[p].parent)
                ++d;
            return d;
        };
        return DEPTH(A) < DEPTH(B);
    });

    // Skins.
    m_skins.clear();
    for (cgltf_size s = 0; s < data->skins_count; ++s) {
        const auto& SK = data->skins[s];
        SSkin SKN;
        for (cgltf_size j = 0; j < SK.joints_count; ++j)
            SKN.joints.push_back(NODE_IDX(SK.joints[j]));

        if (SK.inverse_bind_matrices) {
            const auto* ACC = SK.inverse_bind_matrices;
            std::vector<float> F(static_cast<size_t>(ACC->count) * 16);
            cgltf_accessor_unpack_floats(ACC, F.data(), F.size());
            SKN.invBind.resize(ACC->count);
            for (size_t j = 0; j < SKN.invBind.size(); ++j)
                SKN.invBind[j] = Mat4::fromColumnMajor(&F[j * 16]);
        } else {
            SKN.invBind.resize(SK.joints_count, Mat4::identity());
        }

        m_skins.push_back(std::move(SKN));
    }

    // Animations.
    m_samplers.clear();
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const auto& A = data->animations[a];
        SAnim       AN;
        AN.name = A.name ? A.name : "";

        // Samplers of ALL animations share one flat vector; a channel's
        // sampler pointer indexes its OWN animation's array, so the base
        // offset must be added -- without it every animation after the
        // first read the FIRST animation's curves (all states playing the
        // same clip).
        const int SAMPLER_BASE = static_cast<int>(m_samplers.size());

        for (cgltf_size s = 0; s < A.samplers_count; ++s) {
            const auto& S = A.samplers[s];
            SSampler SAM;
            SAM.interp = static_cast<int>(S.interpolation);
            SAM.comps = S.output->type == cgltf_type_vec4 ? 4 : 3;

            SAM.times.resize(S.input->count);
            cgltf_accessor_unpack_floats(S.input, SAM.times.data(), S.input->count);

            SAM.values.resize(static_cast<size_t>(S.output->count) * 4);
            cgltf_accessor_unpack_floats(S.output, SAM.values.data(),
                                         static_cast<cgltf_size>(SAM.values.size()));

            SAM.duration = S.input->count > 0 && S.input->has_max
                ? static_cast<float>(S.input->max[0])
                : (SAM.times.empty() ? 0.f : SAM.times.back());
            AN.duration = std::max(AN.duration, SAM.duration);

            m_samplers.push_back(std::move(SAM));
        }

        for (cgltf_size c = 0; c < A.channels_count; ++c) {
            const auto& C = A.channels[c];
            SChannel CH;
            CH.node    = NODE_IDX(C.target_node);
            CH.sampler = C.sampler
                ? SAMPLER_BASE + static_cast<int>(C.sampler - A.samplers)
                : -1;
            switch (C.target_path) {
                case cgltf_animation_path_type_translation: CH.path = 0; break;
                case cgltf_animation_path_type_rotation: CH.path = 1; break;
                case cgltf_animation_path_type_scale: CH.path = 2; break;
                default: continue; // weights: not supported (no morph targets)
            }
            AN.channels.push_back(CH);
        }

        m_anims.push_back(std::move(AN));
    }

    // Primitives: per-node rigid meshes.
    const std::string MODEL_DIR = path.substr(0, path.find_last_of('/'));
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto& N = data->nodes[i];
        if (!N.mesh)
            continue;

        for (cgltf_size p = 0; p < N.mesh->primitives_count; ++p) {
            const auto& PR = N.mesh->primitives[p];

            const cgltf_accessor* POS = nullptr;
            const cgltf_accessor* NRM = nullptr;
            const cgltf_accessor* UV = nullptr;
            const cgltf_accessor* JNT = nullptr;
            const cgltf_accessor* WGT = nullptr;
            for (cgltf_size a = 0; a < PR.attributes_count; ++a) {
                switch (PR.attributes[a].type) {
                    case cgltf_attribute_type_position: POS = PR.attributes[a].data; break;
                    case cgltf_attribute_type_normal: NRM = PR.attributes[a].data; break;
                    case cgltf_attribute_type_texcoord:
                        if (!UV)
                            UV = PR.attributes[a].data;
                        break;
                    case cgltf_attribute_type_joints: JNT = PR.attributes[a].data; break;
                    case cgltf_attribute_type_weights: WGT = PR.attributes[a].data; break;
                    default: break;
                }
            }
            if (!POS)
                continue;

            SPrim P;
            P.node  = static_cast<int>(i);
            P.count = static_cast<int>(POS->count);
            P.indexed = PR.indices != nullptr;

            if (N.skin && JNT && WGT &&
                N.skin >= data->skins && N.skin < data->skins + data->skins_count) {
                P.skin = static_cast<int>(N.skin - data->skins);
                P.joints.resize(static_cast<size_t>(POS->count) * 4);
                P.weights.resize(static_cast<size_t>(POS->count) * 4);
                for (cgltf_size v = 0; v < POS->count; ++v) {
                    float W[4] = {1.f, 0.f, 0.f, 0.f};
                    if (WGT)
                        cgltf_accessor_read_float(WGT, v, W, 4);
                    const float SUM =
                        W[0] + W[1] + W[2] + W[3] > 1e-8f
                            ? W[0] + W[1] + W[2] + W[3]
                            : 1.f;
                    // JOINTS_0 are raw indices (ubyte/ushort) -- read per
                    // ELEMENT; read_index() only works on SCALAR accessors.
                    cgltf_uint J[4] = {0, 0, 0, 0};
                    if (JNT)
                        cgltf_accessor_read_uint(JNT, v, J, 4);
                    for (int k = 0; k < 4; ++k) {
                        P.joints[v * 4 + k] = static_cast<uint16_t>(J[k]);
                        P.weights[v * 4 + k] = W[k] / SUM;
                    }
                }
            }

            P.basePos.resize(static_cast<size_t>(POS->count) * 3);
            cgltf_accessor_unpack_floats(POS, P.basePos.data(), P.basePos.size());
            if (NRM) {
                P.baseNrm.resize(static_cast<size_t>(NRM->count) * 3);
                cgltf_accessor_unpack_floats(NRM, P.baseNrm.data(), P.baseNrm.size());
            } else {
                P.baseNrm.assign(static_cast<size_t>(POS->count) * 3, 0.f);
                for (size_t v = 0; v < P.baseNrm.size(); v += 3)
                    P.baseNrm[v + 1] = 1.f;
            }
            if (UV) {
                P.baseUv.resize(static_cast<size_t>(UV->count) * 2);
                cgltf_accessor_unpack_floats(UV, P.baseUv.data(), P.baseUv.size());
            }

            if (PR.material) {
                const auto& M = *PR.material;
                for (int k = 0; k < 4; ++k)
                    P.color[k] = M.pbr_metallic_roughness.base_color_factor[k];
                if (M.pbr_metallic_roughness.base_color_texture.texture &&
                    M.pbr_metallic_roughness.base_color_texture.texture->image)
                    P.texture = uploadTexture(
                        MODEL_DIR,
                        M.pbr_metallic_roughness.base_color_texture.texture->image);
            }

            // GPU skinning when the palette fits the shader's uniform
            // array; bigger rigs fall back to the CPU path.
            P.gpuSkinned = P.skin >= 0 &&
                !P.joints.empty() &&
                m_skins[P.skin].joints.size() <= MAX_JOINTS;

            // GL buffers. GPU-skinned: STATIC interleaved
            // pos3/nrm3/uv2/joint4/weight4, filled here once. Rigid:
            // pos3/nrm3/uv2, filled by update() when the node world moves.
            glGenVertexArrays(1, &P.vao);
            glGenBuffers(1, &P.vbo);
            if (P.indexed) {
                glGenBuffers(1, &P.ebo);
                P.count = static_cast<int>(PR.indices->count);
                std::vector<unsigned int> IDX(PR.indices->count);
                for (cgltf_size k = 0; k < PR.indices->count; ++k)
                    IDX[k] = static_cast<unsigned int>(
                        cgltf_accessor_read_index(PR.indices, k));

                glBindVertexArray(P.vao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, P.ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(IDX.size() * sizeof(unsigned int)),
                             IDX.data(), GL_STATIC_DRAW);
            } else {
                glBindVertexArray(P.vao);
            }

            glBindBuffer(GL_ARRAY_BUFFER, P.vbo);
            if (P.gpuSkinned) {
                const int NV = static_cast<int>(P.basePos.size() / 3);
                std::vector<float> BUF;
                BUF.reserve(static_cast<size_t>(NV) * 16);
                for (int v = 0; v < NV; ++v) {
                    for (int k = 0; k < 3; ++k)
                        BUF.push_back(P.basePos[v * 3 + k]);
                    for (int k = 0; k < 3; ++k)
                        BUF.push_back(P.baseNrm[v * 3 + k]);
                    BUF.push_back(v < static_cast<int>(P.baseUv.size() / 2)
                                      ? P.baseUv[v * 2 + 0] : 0.f);
                    BUF.push_back(v < static_cast<int>(P.baseUv.size() / 2)
                                      ? P.baseUv[v * 2 + 1] : 0.f);
                    for (int k = 0; k < 4; ++k)
                        BUF.push_back(static_cast<float>(P.joints[v * 4 + k]));
                    for (int k = 0; k < 4; ++k)
                        BUF.push_back(P.weights[v * 4 + k]);
                }
                glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(BUF.size() * sizeof(float)),
                             BUF.data(), GL_STATIC_DRAW);
                P.uploaded = std::move(BUF); // test hook: bind pose
            } else {
                glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);
            }

            const GLsizei STRIDE = P.gpuSkinned ? 16 * sizeof(float)
                                                : 8 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(6 * sizeof(float)));
            if (P.gpuSkinned) {
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(8 * sizeof(float)));
                glEnableVertexAttribArray(4);
                glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(12 * sizeof(float)));
            }
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            m_prims.push_back(std::move(P));
        }
    }

    // Program.
    const GLuint VS = compileShader(GL_VERTEX_SHADER, PLAYER_VERTEX);
    const GLuint FS = compileShader(GL_FRAGMENT_SHADER, PLAYER_FRAGMENT);
    if (VS && FS) {
        const GLuint P = glCreateProgram();
        glAttachShader(P, VS);
        glAttachShader(P, FS);
        glLinkProgram(P);
        GLint ok = GL_FALSE;
        glGetProgramiv(P, GL_LINK_STATUS, &ok);
        if (ok == GL_TRUE) {
            m_program  = P;
            m_uMVP     = glGetUniformLocation(P, "uMVP");
            m_uModel   = glGetUniformLocation(P, "uModel");
            m_uColor   = glGetUniformLocation(P, "uColor");
            m_uTex     = glGetUniformLocation(P, "uTex");
            m_uHasTex  = glGetUniformLocation(P, "uHasTex");
            m_uCamPos  = glGetUniformLocation(P, "uCamPos");
            m_uJoints    = glGetUniformLocation(P, "uJoints");
            m_uSkinned   = glGetUniformLocation(P, "uSkinned");
            m_uMeshWorld = glGetUniformLocation(P, "uMeshWorld");
            m_uFlat      = glGetUniformLocation(P, "uFlat");
        }
        glDeleteShader(VS);
        glDeleteShader(FS);
    }

    cgltf_free(data);

    m_loaded = true;
    evaluateNodes(); // rest pose for the first frame

    // Which space the inverse bind matrices expect, per skin. glTF says the
    // skinned mesh node's transform is ignored: at rest jointWorld * invBind
    // is the identity and vertices go straight through the palette (Blender
    // and Mixamo exports, where the mesh sits under a 0.01-scaled armature).
    // Some exports bake the mesh node's world into the bind instead; those
    // keep the mesh-world-first path. Applying the mesh world to a spec
    // export scaled every vertex by the armature twice and collapsed it onto
    // its joints (membranes stretched between the bones).
    m_skinMeshWorld.assign(m_skins.size(), true);
    for (size_t s = 0; s < m_skins.size(); ++s) {
        const auto& SK = m_skins[s];
        if (SK.joints.empty() || SK.invBind.empty())
            continue;
        const int N = SK.joints[0];
        if (N < 0 || N >= static_cast<int>(m_world.size()))
            continue;

        const Mat4 REST = m_world[N] * SK.invBind[0];
        const Mat4 I    = Mat4::identity();
        bool identity   = true;
        for (int c = 0; c < 16; ++c)
            if (std::fabs(REST.m[c] - I.m[c]) > 1e-3f)
                identity = false;
        m_skinMeshWorld[s] = !identity;
    }

    return true;
}

void CPlayerModel::destroy() {
    for (auto& P : m_prims) {
        if (P.ebo)
            glDeleteBuffers(1, &P.ebo);
        if (P.vbo)
            glDeleteBuffers(1, &P.vbo);
        if (P.vao)
            glDeleteVertexArrays(1, &P.vao);
        if (P.texture)
            glDeleteTextures(1, &P.texture);
    }
    m_prims.clear();

    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }

    m_nodes.clear();
    m_order.clear();
    m_world.clear();
    m_anims.clear();
    m_samplers.clear();
    m_skins.clear();
    m_skinMats.clear();
    m_loaded = false;
    m_time = 0.f;
    m_state = EState::Idle;
    m_path.clear();
}

void CPlayerModel::draw(const Mat4& vp, const Vec3& cameraPos) const {
    if (!m_loaded || !m_program || m_prims.empty())
        return;

    // The transform semantics match the scene objects: POSITION is a
    // world-space anchor shift (does NOT swing with the camera), ROTATION
    // and SCALE are local, around the anchor.
    constexpr float DEG = 3.14159265358979f / 180.0f;
    const Mat4 LROT = Mat4::rotationX(m_rotDeg.x * DEG) *
        (Mat4::rotationY(m_rotDeg.y * DEG) * Mat4::rotationZ(m_rotDeg.z * DEG));
    const Mat4 MODEL = Mat4::translation(m_feet + m_offset) *
        (Mat4::rotationY(m_yaw) * (LROT * Mat4::scale(m_scale)));
    const Mat4 MVP = vp * MODEL;

    glUseProgram(m_program);
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, MVP.m.data());
    glUniformMatrix4fv(m_uModel, 1, GL_FALSE, MODEL.m.data());
    glUniform1i(m_uTex, 0);
    glUniform1i(m_uFlat, m_flat);
    glUniform3f(m_uCamPos, cameraPos.x, cameraPos.y, cameraPos.z);

    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);

    for (const auto& P : m_prims) {
        if (!P.count)
            continue;

        if (P.gpuSkinned) {
            const auto& JM = m_skinMats[P.skin];
            const int COUNT = std::min<size_t>(JM.size(), MAX_JOINTS);
            glUniform1i(m_uSkinned, COUNT ? 1 : 0);
            const bool MESH_WORLD = P.skin < static_cast<int>(m_skinMeshWorld.size()) &&
                m_skinMeshWorld[P.skin];
            const Mat4 W = MESH_WORLD ? m_world[P.node] : Mat4::identity();
            glUniformMatrix4fv(m_uMeshWorld, 1, GL_FALSE, W.m.data());
            if (COUNT)
                glUniformMatrix4fv(m_uJoints, COUNT, GL_FALSE,
                                   JM[0].m.data());
        } else {
            glUniform1i(m_uSkinned, 0);
        }

        glUniform4f(m_uColor, P.color[0] * m_emissiveScale,
                    P.color[1] * m_emissiveScale,
                    P.color[2] * m_emissiveScale, P.color[3]);
        glUniform1i(m_uHasTex, P.texture != 0);
        glBindTexture(GL_TEXTURE_2D, P.texture);
        glBindVertexArray(P.vao);
        if (P.indexed)
            glDrawElements(GL_TRIANGLES, P.count, GL_UNSIGNED_INT, nullptr);
        else
            glDrawArrays(GL_TRIANGLES, 0, P.count);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace H3D

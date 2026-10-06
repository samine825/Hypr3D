#define CGLTF_IMPLEMENTATION
#include "../../third_party/cgltf.h"

#include <GLES3/gl32.h>

#include "Render/MapModel.hpp"

#include "World/MapCollision.hpp"

#include <hyprgraphics/image/Image.hpp>

#include "../../third_party/stb_image.h"
#include <hyprutils/memory/SharedPtr.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace H3D {

namespace {

constexpr const char* MAP_VERTEX = R"GLSL(
#version 300 es

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uMVP;
uniform mat4 uModel;

out vec3 vNormal;
out vec2 vUV;

void main() {
    gl_Position = uMVP * vec4(aPosition, 1.0);
    // Uniform scale only, so the rotated basis is a valid normal basis.
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
}
)GLSL";

constexpr const char* DEBUG_VERTEX = R"GLSL(
#version 300 es

layout(location = 0) in vec3 aPosition;

uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPosition, 1.0);
}
)GLSL";

constexpr const char* DEBUG_FRAGMENT = R"GLSL(
#version 300 es

precision mediump float;

out vec4 fragColor;

void main() {
    fragColor = vec4(1.0, 0.1, 0.1, 1.0);
}
)GLSL";

constexpr const char* MAP_FRAGMENT = R"GLSL(
#version 300 es

precision mediump float;

in vec3 vNormal;
in vec2 vUV;

uniform sampler2D uTex;
uniform vec4 uColor;
uniform sampler2D uEmissive;
uniform int uHasEmissive;
uniform vec3 uEmissiveFactor;
uniform float uEmissiveStrength;
uniform int uFlat;   // baked-map mode: albedo/emission as-is, no dynamic light
uniform int uAlphaMode;      // 0 opaque, 1 mask, 2 blend (glTF alphaMode)
uniform float uAlphaCutoff;  // mask cutoff

out vec4 fragColor;

void main() {
    vec3 base = uColor.rgb * texture(uTex, vUV).rgb;

    // glTF alpha: baseColor alpha = factor.a * texture.a. Opaque forces 1;
    // mask discards below the cutoff; blend passes it through (the draw
    // call enables blending only for the blend pass).
    float alpha = uColor.a * texture(uTex, vUV).a;
    if (uAlphaMode == 0)
        alpha = 1.0;

    // Baked-map mode (uFlat): the textures already contain ALL lighting
    // and shading (Sketchfab softbakes bake light into baseColor AND set
    // emissive = baseColor). Any dynamic term re-shades what is baked --
    // down-facing white lamp panels came out grey at 0.64x while the author
    // intended them bright. Show albedo/emission exactly as authored, like
    // Blender's view of a baked map.
    if (uFlat != 0) {
        if (uHasEmissive != 0) {
            vec3 e = texture(uEmissive, vUV).rgb * uEmissiveFactor *
                uEmissiveStrength;
            float ea = uColor.a * texture(uEmissive, vUV).a;
            if (uAlphaMode == 1 && ea < uAlphaCutoff)
                discard;
            fragColor = vec4(e, uAlphaMode == 0 ? 1.0 : ea);
        } else {
            if (uAlphaMode == 1 && alpha < uAlphaCutoff)
                discard;
            fragColor = vec4(base, alpha);
        }
        return;
    }

    // Dynamic mode: half-lambert (wrapped) light -- no hard terminator; a
    // plain lambert crushed down-facing surfaces to the ambient floor.
    vec3 N = normalize(vNormal);
    float wrap = clamp(dot(N, normalize(vec3(0.4, 0.8, 0.45))) * 0.5 + 0.5,
                       0.0, 1.0);

    // Emissive replaces the lambert for baked lights: the bake already
    // contains the lighting, adding both pushed rooms 1.6-2x over albedo.
    if (uHasEmissive != 0) {
        vec3 emissive = texture(uEmissive, vUV).rgb * uEmissiveFactor *
            uEmissiveStrength;
        float ea = uColor.a;
        if (uAlphaMode == 1 && ea < uAlphaCutoff)
            discard;
        fragColor = vec4(emissive, uAlphaMode == 0 ? 1.0 : ea);
    } else {
        if (uAlphaMode == 1 && alpha < uAlphaCutoff)
            discard;
        fragColor = vec4(base * mix(vec3(0.6), vec3(1.0), wrap), alpha);
    }
}
)GLSL";

GLuint compileMapShader(GLenum type, const char* src) {
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

// Rotation around a local pivot: world = position + R * S * (local - pivot).
// composeModel composes as R * (S * x - S * pivot) + position, so the model
// matrix T * R * T(-S*pivot) * S places the PIVOT at `position`.
Mat4 composeModel(const Vec3& position, const Vec3& rotationDeg,
                  const Vec3& scale, const Vec3& pivot) {
    constexpr float DEG = 3.14159265358979f / 180.0f;

    const Mat4 T  = Mat4::translation(position);
    const Mat4 RY = Mat4::rotationY(rotationDeg.y * DEG);
    const Mat4 RX = Mat4::rotationX(rotationDeg.x * DEG);
    const Mat4 RZ = Mat4::rotationZ(rotationDeg.z * DEG);
    const Mat4 S  = Mat4::scale(scale);

    const Vec3 PS =
        Vec3{scale.x * pivot.x, scale.y * pivot.y, scale.z * pivot.z};

    // Same convention as the window model: RY * RX * RZ.
    return T * RY * (RX * (RZ * (Mat4::translation(PS * -1.0f) * S)));
}

[[maybe_unused]] Mat4 composeModel(const Vec3& position,
                                   const Vec3& rotationDeg,
                                   const Vec3& scale) {
    return composeModel(position, rotationDeg, scale, Vec3{0.f, 0.f, 0.f});
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
    };
}

} // namespace

// --- decoding (worker thread) -------------------------------------------------

// Everything a load reads and decodes, without a GL object: it is built on a
// worker thread. Inside the render pass the same work froze Hyprland for
// 437 ms on a 47 MB model (Khronos' FlightHelmet, measured with perf).
struct CMapModel::SDecoded {
    struct SImage {
        int                        w = 0, h = 0;
        std::vector<unsigned char> rgba; // empty: unused or undecodable
    };

    struct SPrim {
        SPrimitive            params;             // GL ids are still 0
        int                   baseImage     = -1; // index into images
        int                   emissiveImage = -1;
        std::vector<float>    verts;
        std::vector<uint32_t> indices;
    };

    std::vector<SImage> images; // by glTF image index
    std::vector<SPrim>  prims;
    std::vector<STL>    localTriangles;
};

namespace {

// One glTF image as RGBA rows, the top row first. Leaves rgba empty when the
// image cannot be read.
void decodeImage(const std::string& modelDir, const cgltf_image* image, int& w,
                 int& h, std::vector<unsigned char>& rgba) {
    std::unique_ptr<Hyprgraphics::CImage> img;

    const uint8_t* embedded = nullptr;
    size_t         embeddedSize = 0;

    if (image->buffer_view) {
        const auto* BV   = image->buffer_view;
        const auto* BASE = static_cast<const uint8_t*>(cgltf_buffer_view_data(BV));
        if (!BASE)
            return;
        embedded     = BASE;
        embeddedSize = BV->size;
        img = std::make_unique<Hyprgraphics::CImage>(
            std::span<const uint8_t>{BASE, BV->size}, Hyprgraphics::IMAGE_FORMAT_AUTO);
    } else if (image->uri && !std::string_view{image->uri}.starts_with("data:")) {
        img = std::make_unique<Hyprgraphics::CImage>(modelDir + "/" + image->uri);
    } else {
        return; // data URIs are rare in maps; skip rather than fail
    }

    auto surface = (img && img->success()) ? img->cairoSurface() : nullptr;

    if (!surface || surface->status() != CAIRO_STATUS_SUCCESS) {
        // hyprgraphics decodes only PNG, AVIF and SVG from memory, and a .glb
        // embeds other formats as often (all five images of Khronos'
        // DamagedHelmet.glb are image/jpeg). A failed embedded image falls
        // back to the vendored stb_image -- the same decoder PlayerModel.cpp
        // implements -- which auto-detects JPEG, PNG, BMP, TGA, GIF, PSD, HDR,
        // PIC and PNM from bytes. stb's rows are top-first RGBA with straight
        // alpha, exactly what the cairo conversion below produces.
        if (embedded) {
            int W = 0, H = 0, COMP = 0;
            if (auto* PX = stbi_load_from_memory(
                    embedded, static_cast<int>(embeddedSize), &W, &H, &COMP, 4)) {
                rgba.assign(PX, PX + static_cast<size_t>(W) * H * 4);
                stbi_image_free(PX);
                w = W;
                h = H;
            }
        }
        return;
    }

    const int W      = static_cast<int>(surface->size().x);
    const int H      = static_cast<int>(surface->size().y);
    const int STRIDE = surface->stride();
    const auto* SRC  = surface->data();

    if (W <= 0 || H <= 0 || !SRC)
        return;

    // Cairo ARGB32 is premultiplied BGRA; textures want RGBA. NO row flip:
    // glTF's UV origin is the image TOP-LEFT with v growing down, and GL's
    // v = 0 samples the first uploaded row -- so cairo row 0 (the top) must
    // land in the first texel row. Flipping here mirrors every texture.
    rgba.resize(static_cast<size_t>(W) * H * 4);

    for (int y = 0; y < H; ++y) {
        const auto* row = SRC + static_cast<size_t>(y) * STRIDE;
        auto*       dst = rgba.data() + static_cast<size_t>(y) * W * 4;

        for (int x = 0; x < W; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 2];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 0];
            dst[x * 4 + 3] = row[x * 4 + 3];
        }
    }

    w = W;
    h = H;
}

} // namespace

std::unique_ptr<CMapModel::SDecoded>
CMapModel::decode(const std::string& path, std::stop_token stop) {
    cgltf_options options{};
    cgltf_data*   raw = nullptr;

    if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success)
        return nullptr;

    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data{raw, cgltf_free};

    // Relative .bin / image URIs resolve against the model file's directory
    // only when the model file's own path is passed here.
    if (cgltf_load_buffers(&options, data.get(), path.c_str()) != cgltf_result_success)
        return nullptr;

    if (cgltf_validate(data.get()) != cgltf_result_success)
        return nullptr;

    auto result = std::make_unique<SDecoded>();
    result->images.resize(data->images_count);

    const std::string MODEL_DIR =
        std::filesystem::path{path}.parent_path().string();

    // The image behind a texture slot, decoded the first time it is used.
    std::vector<bool> tried(data->images_count, false);
    const auto IMAGE = [&](const cgltf_texture_view& view) {
        if (!view.texture || !view.texture->image)
            return -1;

        const auto IDX = static_cast<size_t>(view.texture->image - data->images);
        if (IDX >= result->images.size())
            return -1;

        if (!tried[IDX]) {
            tried[IDX] = true;
            auto& IMG  = result->images[IDX];
            decodeImage(MODEL_DIR, view.texture->image, IMG.w, IMG.h, IMG.rgba);
        }

        return result->images[IDX].rgba.empty() ? -1 : static_cast<int>(IDX);
    };

    // Depth-first over the scene graph so nested meshes keep their world
    // placement.
    std::vector<const cgltf_node*> stack;
    if (data->scene)
        for (cgltf_size i = 0; i < data->scene->nodes_count; ++i)
            stack.push_back(data->scene->nodes[i]);

    while (!stack.empty()) {
        // A newer load or the plugin's exit waits for this thread.
        if (stop.stop_requested())
            return nullptr;

        const cgltf_node* node = stack.back();
        stack.pop_back();

        for (cgltf_size i = 0; i < node->children_count; ++i)
            stack.push_back(node->children[i]);

        if (!node->mesh)
            continue;

        // Nodes named "nocol*" render but never collide (decor convention).
        const bool NOCOL =
            node->name && std::string_view{node->name}.starts_with("nocol");

        cgltf_float nodeWorld[16];
        cgltf_node_transform_world(node, nodeWorld);
        const Mat4 NODE = Mat4::fromColumnMajor(nodeWorld);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive* prim = &node->mesh->primitives[p];

            const cgltf_accessor* POS = nullptr;
            const cgltf_accessor* NRM = nullptr;
            const cgltf_accessor* TEX = nullptr;

            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                const auto* ATTR = &prim->attributes[a];
                if (ATTR->type == cgltf_attribute_type_position)
                    POS = ATTR->data;
                else if (ATTR->type == cgltf_attribute_type_normal)
                    NRM = ATTR->data;
                else if (ATTR->type == cgltf_attribute_type_texcoord &&
                         ATTR->index == 0)
                    TEX = ATTR->data;
            }

            if (!POS || POS->count == 0)
                continue;

            SDecoded::SPrim out_{};
            SPrimitive&     out = out_.params;

            if (prim->material) {
                // glTF alpha pipeline.
                out.alphaMode = static_cast<int>(prim->material->alpha_mode);
                out.alphaCutoff = prim->material->alpha_cutoff > 0.f ?
                    static_cast<float>(prim->material->alpha_cutoff) : 0.5f;

                const auto& PBR = prim->material->pbr_metallic_roughness;
                for (int c = 0; c < 4; ++c)
                    out.color[c] = static_cast<float>(PBR.base_color_factor[c]);

                // KHR_materials_pbrSpecularGlossiness (older Sketchfab and
                // AI-generated exports) keeps the colour in its diffuse
                // texture/factor and leaves the metallic-roughness base empty;
                // without this such models came out plain white.
                const bool SPEC_GLOSS =
                    prim->material->has_pbr_specular_glossiness &&
                    !PBR.base_color_texture.texture;
                const auto& SG = prim->material->pbr_specular_glossiness;
                if (SPEC_GLOSS)
                    for (int c = 0; c < 4; ++c)
                        out.color[c] = static_cast<float>(SG.diffuse_factor[c]);

                // Emissive: factor defaults to black per spec, strength to 1
                // unless KHR_materials_emissive_strength says otherwise.
                // NOTE: black factor really means "no emission" -- e.g. the
                // backrooms Ceiling_Lamp carries none in either export; its
                // brightness comes from the albedo shading below.
                for (int c = 0; c < 3; ++c)
                    out.emissiveFactor[c] =
                        static_cast<float>(prim->material->emissive_factor[c]);
                out.emissiveStrength =
                    prim->material->has_emissive_strength ?
                        static_cast<float>(
                            prim->material->emissive_strength
                                .emissive_strength) :
                        1.0f;

                out_.baseImage     = IMAGE(SPEC_GLOSS ? SG.diffuse_texture :
                                                       PBR.base_color_texture);
                out_.emissiveImage = IMAGE(prim->material->emissive_texture);
            }

            // Interleaved attribute stream for the GPU: pos(3) normal(3)
            // uv(2). The glTF NODE transform is baked in here -- the shader
            // only knows the map's config transform (uModel), so without the
            // bake the mesh renders in raw accessor orientation (Sketchfab
            // roots are rotated; the model shows up on its side) while the
            // collision triangles land correctly.
            std::vector<float>& verts = out_.verts;
            verts.reserve(static_cast<size_t>(POS->count) * 8);

            // Node-space AABB center for the blend pass's distance sort.
            Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};

            float cpos[3] = {0, 0, 0}, cnrm[3] = {0, 0, 1}, cuv[2] = {0, 0};

            for (cgltf_size i = 0; i < POS->count; ++i) {
                cgltf_accessor_read_float(POS, i, cpos, 3);
                if (NRM)
                    cgltf_accessor_read_float(NRM, i, cnrm, 3);
                if (TEX)
                    cgltf_accessor_read_float(TEX, i, cuv, 2);

                const Vec3 P = transformPoint(NODE, {cpos[0], cpos[1], cpos[2]});
                Vec3 N = {
                    NODE.m[0] * cnrm[0] + NODE.m[4] * cnrm[1] + NODE.m[8] * cnrm[2],
                    NODE.m[1] * cnrm[0] + NODE.m[5] * cnrm[1] + NODE.m[9] * cnrm[2],
                    NODE.m[2] * cnrm[0] + NODE.m[6] * cnrm[1] + NODE.m[10] * cnrm[2],
                };
                const float LEN = std::sqrt(N.x * N.x + N.y * N.y + N.z * N.z);
                if (LEN > 1e-9f) {
                    N.x /= LEN;
                    N.y /= LEN;
                    N.z /= LEN;
                }

                lo = {std::min(lo.x, P.x), std::min(lo.y, P.y),
                      std::min(lo.z, P.z)};
                hi = {std::max(hi.x, P.x), std::max(hi.y, P.y),
                      std::max(hi.z, P.z)};

                verts.push_back(P.x);
                verts.push_back(P.y);
                verts.push_back(P.z);
                verts.push_back(N.x);
                verts.push_back(N.y);
                verts.push_back(N.z);
                verts.push_back(cuv[0]);
                verts.push_back(cuv[1]);
            }

            std::vector<uint32_t>& indices = out_.indices;
            if (prim->indices) {
                indices.resize(prim->indices->count);
                for (cgltf_size i = 0; i < prim->indices->count; ++i) {
                    cgltf_uint idx = 0;
                    cgltf_accessor_read_uint(prim->indices, i, &idx, 1);
                    indices[i] = idx;
                }
            } else {
                indices.resize(POS->count);
                for (cgltf_size i = 0; i < POS->count; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            }

            // Collision triangles in NODE space (the map's config transform
            // is NOT applied here). The vertices are already node-transformed
            // (baked above), so the triangle set takes them as-is -- applying
            // NODE here again would double-transform the collision geometry.
            if (!NOCOL) {
                for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                    const auto V0 = indices[t] * 8;
                    const auto V1 = indices[t + 1] * 8;
                    const auto V2 = indices[t + 2] * 8;

                    result->localTriangles.push_back({
                        {verts[V0 + 0], verts[V0 + 1], verts[V0 + 2]},
                        {verts[V1 + 0], verts[V1 + 1], verts[V1 + 2]},
                        {verts[V2 + 0], verts[V2 + 1], verts[V2 + 2]},
                    });
                }
            }

            out.centroid = Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                                (lo.z + hi.z) * 0.5f};

            out.count   = static_cast<int>(indices.size());
            out.indexed = true;

            result->prims.push_back(std::move(out_));
        }
    }

    return result;
}

// --- load / upload / destroy --------------------------------------------------

CMapModel::CMapModel() = default;

CMapModel::~CMapModel() {
    stopWorker();
}

void CMapModel::stopWorker() {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_decoded.reset();
    m_decodedReady.store(false, std::memory_order_relaxed);
}

bool CMapModel::load(const std::string& path, const Vec3& position,
                     const Vec3& rotationDeg, const Vec3& scale) {
    // A load still decoding is dropped; its thread stops at the next node.
    // One still uploading its textures is dropped as well.
    stopWorker();
    dropUpload();

    // Another file: the old mesh goes now, as before. The same file again
    // (written anew, e.g. from Blender): the old mesh stays until the new
    // one is decoded, so the room does not flicker.
    if (m_meshPath != path)
        releaseMesh();

    setTransform(position, rotationDeg, scale);
    m_path   = path;
    m_failed = false;

    m_worker = std::jthread([this, path](std::stop_token stop) {
        // An exception leaving this thread would terminate Hyprland.
        try {
            m_decoded = decode(path, stop);
        } catch (...) {
            m_decoded.reset();
        }
        m_decodedReady.store(true, std::memory_order_release);
    });

    return true;
}

void CMapModel::poll() {
    if (!m_uploading) {
        if (!m_worker.joinable() || !m_decodedReady.load(std::memory_order_acquire))
            return;

        m_worker.join();
        m_decodedReady.store(false, std::memory_order_relaxed);

        if (!m_decoded) {
            m_failed = true;
            return;
        }

        m_uploading = std::move(m_decoded);
        m_uploadTextures.assign(m_uploading->images.size(), 0);
        m_uploadNext = 0;
    }

    // One texture per frame: six 2048x2048 textures with their mipmaps held
    // Hyprland ~100 ms in a single frame (VM, virgl).
    while (m_uploadNext < m_uploading->images.size()) {
        auto& IMG = m_uploading->images[m_uploadNext++];
        if (IMG.rgba.empty())
            continue;

        unsigned int tex = 0;
        glGenTextures(1, &tex);

        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, IMG.w, IMG.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, IMG.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);

        m_uploadTextures[m_uploadNext - 1] = tex;
        IMG.rgba = {}; // uploaded: the pixels can go
        return;
    }

    finishUpload();
}

void CMapModel::finishUpload() {
    const auto DECODED = std::move(m_uploading);

    releaseMesh();

    if (!m_program) {
        const GLuint VS = compileMapShader(GL_VERTEX_SHADER, MAP_VERTEX);
        const GLuint FS = compileMapShader(GL_FRAGMENT_SHADER, MAP_FRAGMENT);

        if (VS && FS) {
            const GLuint P = glCreateProgram();
            glAttachShader(P, VS);
            glAttachShader(P, FS);
            glLinkProgram(P);

            GLint ok = GL_FALSE;
            glGetProgramiv(P, GL_LINK_STATUS, &ok);
            if (ok == GL_TRUE) {
                m_program = P;
                m_uMVP    = glGetUniformLocation(P, "uMVP");
                m_uModel  = glGetUniformLocation(P, "uModel");
                m_uColor  = glGetUniformLocation(P, "uColor");
                m_uTex    = glGetUniformLocation(P, "uTex");
                m_uEmissive         = glGetUniformLocation(P, "uEmissive");
                m_uHasEmissive      = glGetUniformLocation(P, "uHasEmissive");
                m_uEmissiveFactor   = glGetUniformLocation(P, "uEmissiveFactor");
                m_uEmissiveStrength = glGetUniformLocation(P, "uEmissiveStrength");
                m_uFlat             = glGetUniformLocation(P, "uFlat");
                m_uAlphaMode        = glGetUniformLocation(P, "uAlphaMode");
                m_uAlphaCutoff      = glGetUniformLocation(P, "uAlphaCutoff");
            }
        }

        if (VS)
            glDeleteShader(VS);
        if (FS)
            glDeleteShader(FS);
    }

    // One texture per decoded image, shared by every primitive using it.
    // destroy() frees them; map reloads would otherwise leak every texture,
    // base and emissive alike.
    const std::vector<unsigned int> TEXTURES = std::move(m_uploadTextures);
    m_uploadTextures.clear();
    for (const auto T : TEXTURES)
        if (T != 0)
            m_ownedTextures.push_back(T);

    for (auto& P : DECODED->prims) {
        SPrimitive out = P.params;
        out.texture     = P.baseImage >= 0 ? TEXTURES[P.baseImage] : 0;
        out.emissiveTex = P.emissiveImage >= 0 ? TEXTURES[P.emissiveImage] : 0;

        glGenVertexArrays(1, &out.vao);
        glGenBuffers(1, &out.vbo);
        glGenBuffers(1, &out.ebo);

        glBindVertexArray(out.vao);

        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(P.verts.size() * sizeof(float)),
                     P.verts.data(), GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                              reinterpret_cast<void*>(6 * sizeof(float)));

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(P.indices.size() * sizeof(uint32_t)),
                     P.indices.data(), GL_STATIC_DRAW);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        m_primitives.push_back(out);
    }

    m_localTriangles = std::move(DECODED->localTriangles);
    m_meshPath       = m_path;
    m_loaded         = true;
    resolvePivot();
    recomputeTriangles();
    ++m_meshVersion;
}

void CMapModel::dropUpload() {
    for (const auto T : m_uploadTextures)
        if (T != 0)
            glDeleteTextures(1, &T);
    m_uploadTextures.clear();
    m_uploading.reset();
    m_uploadNext = 0;
}

float CMapModel::rayCast(const Vec3& origin, const Vec3& dir) const {
    if (!m_loaded || m_triangles.empty())
        return -1.f;

    return CMapCollision::rayTriangles(m_triangles, origin, dir);
}

void CMapModel::releaseMesh() {
    if (!m_ownedTextures.empty()) {
        glDeleteTextures(static_cast<GLsizei>(m_ownedTextures.size()),
                         m_ownedTextures.data());
        m_ownedTextures.clear();
    }

    for (const auto& P : m_primitives) {
        if (P.ebo)
            glDeleteBuffers(1, &P.ebo);
        if (P.vbo)
            glDeleteBuffers(1, &P.vbo);
        if (P.vao)
            glDeleteVertexArrays(1, &P.vao);
    }
    m_primitives.clear();

    m_triangles.clear();
    m_localTriangles.clear();
    m_meshPath.clear();
    m_loaded = false;
    ++m_generation;
    ++m_meshVersion;
}

void CMapModel::destroy() {
    stopWorker();
    dropUpload();
    m_failed = false;

    releaseMesh();

    glDeleteBuffers(1, &m_debugVBO);
    glDeleteVertexArrays(1, &m_debugVAO);
    m_debugVBO = m_debugVAO = 0;
    m_debugVerts = 0;
    m_debugPending = false;
    m_debugBuiltGen = 0;

    if (m_debugProgram) {
        glDeleteProgram(m_debugProgram);
        m_debugProgram = 0;
        m_debugMVP = -1;
    }

    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
        m_uMVP = m_uModel = m_uColor = m_uTex = -1;
    }
    m_path.clear();
}

// --- transform / draw -------------------------------------------------------

void CMapModel::setCenter(ECenter mode, const Vec3& offset) {
    m_center       = mode;
    m_centerOffset = offset;
    if (m_loaded) {
        resolvePivot();
        recomputeTriangles(); // the pivot moved: world triangles follow
        ++m_meshVersion;      // physics bodies rebuild at the new pivot --
                              // without this the Jolt hull stayed at the
                              // OLD offset while the visual shifted
    }
}

void CMapModel::resolvePivot() {
    // Logical = the LOCAL AABB center of the untransformed mesh.
    if (m_center == ECenter::Logical && !m_localTriangles.empty()) {
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const auto& T : m_localTriangles)
            for (const Vec3* P : {&T.a, &T.b, &T.c}) {
                lo = {std::min(lo.x, P->x), std::min(lo.y, P->y),
                      std::min(lo.z, P->z)};
                hi = {std::max(hi.x, P->x), std::max(hi.y, P->y),
                      std::max(hi.z, P->z)};
            }
        m_pivot = Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                       (lo.z + hi.z) * 0.5f};
    } else {
        m_pivot = Vec3{0.f, 0.f, 0.f};
    }
    m_pivot = m_pivot + m_centerOffset;
}

void CMapModel::setTransform(const Vec3& position, const Vec3& rotationDeg,
                             const Vec3& scale) {
    // Zero axes would collapse the map; clamp to a hair.
    const Vec3 NEW_SCALE{
        scale.x > 0.001f ? scale.x : 0.001f,
        scale.y > 0.001f ? scale.y : 0.001f,
        scale.z > 0.001f ? scale.z : 0.001f,
    };

    // update3D pushes the config transform every frame; only a real change
    // may trigger the (O(triangles)) world-space recompute.
    if (m_loaded && m_position.x == position.x && m_position.y == position.y &&
        m_position.z == position.z && m_rotationDeg.x == rotationDeg.x &&
        m_rotationDeg.y == rotationDeg.y && m_rotationDeg.z == rotationDeg.z &&
        m_scale.x == NEW_SCALE.x && m_scale.y == NEW_SCALE.y &&
        m_scale.z == NEW_SCALE.z)
        return;

    m_position    = position;
    m_rotationDeg = rotationDeg;
    m_scale       = NEW_SCALE;

    if (m_loaded)
        recomputeTriangles();
}

void CMapModel::recomputeTriangles() {
    const Mat4 MODEL =
        composeModel(m_position, m_rotationDeg, m_scale, m_pivot);

    m_triangles.clear();
    m_triangles.reserve(m_localTriangles.size());

    for (const auto& T : m_localTriangles)
        m_triangles.push_back({
            transformPoint(MODEL, T.a),
            transformPoint(MODEL, T.b),
            transformPoint(MODEL, T.c),
        });

    // The debug wireframe must follow the new world-space set.
    m_debugPending = m_debugOn;

    // Collision rebuilds its BVH off this.
    ++m_generation;
}

void CMapModel::setDebugCollisions(bool on) {
    if (m_debugOn == on)
        return;

    m_debugOn      = on;
    m_debugPending = on; // rebuild (or drop) the line buffer on next draw
}

void CMapModel::drawDebug(const Mat4& vp) {
    if (!m_debugOn || m_triangles.empty() || !m_program)
        return;

    // (Re)build the line buffer whenever the triangles changed. The flag is
    // set by recomputeTriangles() itself -- generation counters alone missed
    // scale/rotation edits and the wireframe stayed at the old size.
    if (m_debugPending) {
        m_debugPending = false;
        glDeleteBuffers(1, &m_debugVBO);
        glDeleteVertexArrays(1, &m_debugVAO);
        m_debugVBO = m_debugVAO = 0;

        std::vector<float> lines;
        lines.reserve(m_triangles.size() * 6 * 3);

        for (const auto& T : m_triangles) {
            const Vec3* EDS[3][2] = {{&T.a, &T.b}, {&T.b, &T.c}, {&T.c, &T.a}};
            for (const auto& E : EDS)
                for (const Vec3* P : E) {
                    lines.push_back(P->x);
                    lines.push_back(P->y);
                    lines.push_back(P->z);
                }
        }

        glGenVertexArrays(1, &m_debugVAO);
        glGenBuffers(1, &m_debugVBO);

        glBindVertexArray(m_debugVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_debugVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(lines.size() * sizeof(float)),
                     lines.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_debugVerts = static_cast<int>(lines.size() / 3);
    }

    // A separate tiny program: the map program's fragment stage multiplies
    // by vertex normals and a texture that the line stream does not have.
    if (!m_debugProgram) {
        const GLuint VS = compileMapShader(GL_VERTEX_SHADER, DEBUG_VERTEX);
        const GLuint FS = compileMapShader(GL_FRAGMENT_SHADER, DEBUG_FRAGMENT);
        if (VS && FS) {
            const GLuint P = glCreateProgram();
            glAttachShader(P, VS);
            glAttachShader(P, FS);
            glLinkProgram(P);
            m_debugProgram = P;
            m_debugMVP     = glGetUniformLocation(P, "uMVP");
        }
        if (VS)
            glDeleteShader(VS);
        if (FS)
            glDeleteShader(FS);
        if (!m_debugProgram)
            return;
    }

    // The line vertices are already in WORLD space (m_triangles are the
    // transformed collision set) -- the model matrix must NOT be applied
    // again: a carried object's wireframe would draw displaced by exactly
    // its own position (invisible for an identity transform, obvious once
    // the object moves).
    glUseProgram(m_debugProgram);
    glUniformMatrix4fv(m_debugMVP, 1, GL_FALSE, vp.m.data());

    // X-ray: depth test off so collision geometry inside walls stays visible.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    glBindVertexArray(m_debugVAO);
    glDrawArrays(GL_LINES, 0, m_debugVerts);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void CMapModel::draw(const Mat4& vp, const Vec3& cameraPos) const {
    if (!m_loaded || !m_program || m_primitives.empty())
        return;

    const Mat4 MODEL =
        composeModel(m_position, m_rotationDeg, m_scale, m_pivot);
    const Mat4 MVP   = vp * MODEL;

    glUseProgram(m_program);
    glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, MVP.m.data());
    glUniformMatrix4fv(m_uModel, 1, GL_FALSE, MODEL.m.data());
    glUniform1i(m_uTex, 0);
    glUniform1i(m_uEmissive, 1);
    glUniform1i(m_uFlat, m_flat ? 1 : 0);

    glEnable(GL_DEPTH_TEST);

    glActiveTexture(GL_TEXTURE0);

    const auto DRAW_PRIM = [&](const SPrimitive& P) {
        glUniform4f(m_uColor, P.color[0], P.color[1], P.color[2], P.color[3]);
        glUniform1i(m_uAlphaMode, P.alphaMode);
        glUniform1f(m_uAlphaCutoff, P.alphaCutoff);

        const bool HAS_EMISSIVE =
            P.emissiveTex != 0 &&
            (P.emissiveFactor[0] > 0.f || P.emissiveFactor[1] > 0.f ||
             P.emissiveFactor[2] > 0.f);
        glUniform1i(m_uHasEmissive, HAS_EMISSIVE ? 1 : 0);
        glUniform3f(m_uEmissiveFactor, P.emissiveFactor[0], P.emissiveFactor[1],
                    P.emissiveFactor[2]);
        glUniform1f(m_uEmissiveStrength, P.emissiveStrength * m_emissiveScale);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, P.emissiveTex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, P.texture);

        glBindVertexArray(P.vao);
        glDrawElements(GL_TRIANGLES, P.count, GL_UNSIGNED_INT,
                       reinterpret_cast<void*>(0));
    };

    // Pass 1: opaque + mask -- depth write on, no blending. These carve the
    // depth buffer the blend pass tests against.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    for (const auto& P : m_primitives)
        if (P.alphaMode != 2)
            DRAW_PRIM(P);

    // Pass 2: blend (alphaMode BLEND) -- depth TEST on, depth WRITE off, so
    // translucent surfaces never occlude each other's sorting, drawn
    // far-to-near by centroid distance (correct back-to-front compositing).
    const auto BLENDABLE = std::count_if(
        m_primitives.begin(), m_primitives.end(),
        [](const SPrimitive& P) { return P.alphaMode == 2; });

    if (BLENDABLE > 0) {
        std::vector<const SPrimitive*> blend;
        blend.reserve(static_cast<size_t>(BLENDABLE));
        for (const auto& P : m_primitives)
            if (P.alphaMode == 2)
                blend.push_back(&P);

        std::sort(blend.begin(), blend.end(),
                  [&](const SPrimitive* lhs, const SPrimitive* rhs) {
                      const Vec3 L = transformPoint(MODEL, lhs->centroid);
                      const Vec3 R = transformPoint(MODEL, rhs->centroid);
                      const float DL = (L - cameraPos).x * (L - cameraPos).x +
                          (L - cameraPos).y * (L - cameraPos).y +
                          (L - cameraPos).z * (L - cameraPos).z;
                      const float DR = (R - cameraPos).x * (R - cameraPos).x +
                          (R - cameraPos).y * (R - cameraPos).y +
                          (R - cameraPos).z * (R - cameraPos).z;
                      return DL > DR; // far first
                  });

        glEnable(GL_BLEND);
        glBlendFuncSeparate(
            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA
        );
        glDepthMask(GL_FALSE);

        for (const auto* P : blend)
            DRAW_PRIM(*P);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
}

} // namespace H3D

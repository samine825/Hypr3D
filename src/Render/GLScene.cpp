#include "GLScene.hpp"

#include "../../third_party/font8x8_basic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GLES3/gl32.h>

// The test stubs carry only a subset of the GL constants.
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif

#include <hyprgraphics/image/Image.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <vector>
#include <iterator>

namespace H3D {

namespace {

constexpr float PI = 3.14159265358979323846f;

// Real ground plane. The camera is constrained above this level and the grid
// is drawn on this plane, not on a ceiling. Windows live above it.
constexpr float FLOOR_Y = Camera::kFloorY;

// --- window transparency: BSP-ordered exact compositing ---------------------
//
// Two translucent window quads crossing in space cannot be blended correctly
// by a single global draw order: per pixel, the draw order must match which
// plane is actually closer, and for crossing planes that flips across the
// intersection line. A BSP built from the window planes splits crossing quads
// along those lines and the back-to-front traversal yields a per-pixel
// correct order (the classic use of BSP for alpha sorting). Every window is
// a handful of polygons, so the tree is tiny.

struct SWVert {
    Vec3  p{};
    float u = 0.f, v = 0.f;
};

struct SWPoly {
    std::vector<SWVert> verts;
    unsigned int        tex = 0;
    float               alpha = 1.0f; // the window's room alpha (g_fsFade etc.)
};

// Split a polygon by a plane. Pieces on the positive side go to `front`,
// negative to `back`; vertices on the plane land in both.
static void bspSplitPoly(const SWPoly& poly, const Vec3& pl, const Vec3& n,
                         SWPoly& front, SWPoly& back,
                         bool& hasFront, bool& hasBack) {
    front.verts.clear();
    back.verts.clear();
    // Both pieces belong to the same window: carry its texture and alpha,
    // or a split piece renders with texture 0 -- pure black.
    front.tex      = poly.tex;
    front.alpha    = poly.alpha;
    back.tex       = poly.tex;
    back.alpha     = poly.alpha;
    hasFront = hasBack = false;

    const size_t COUNT = poly.verts.size();
    if (COUNT < 3 || COUNT > 32)
        return;

    float dist[32];
    for (size_t i = 0; i < COUNT; ++i)
        dist[i] = dot(poly.verts[i].p - pl, n);

    for (size_t i = 0; i < COUNT; ++i) {
        const size_t J = (i + 1) % COUNT;
        const auto&  A = poly.verts[i];
        const auto&  B = poly.verts[J];
        const float  DA = dist[i], DB = dist[J];

        if (DA >= 0.f) {
            front.verts.push_back(A);
            if (DA > 0.f)
                hasFront = true;
        }
        if (DA <= 0.f) {
            back.verts.push_back(A);
            if (DA < 0.f)
                hasBack = true;
        }

        if ((DA > 0.f && DB < 0.f) || (DA < 0.f && DB > 0.f)) {
            const float T = DA / (DA - DB);
            SWVert I{
                A.p + (B.p - A.p) * T,
                A.u + (B.u - A.u) * T,
                A.v + (B.v - A.v) * T,
            };
            front.verts.push_back(I);
            back.verts.push_back(I);
        }
    }

    hasFront = hasFront && front.verts.size() >= 3;
    hasBack  = hasBack  && back.verts.size()  >= 3;
}

struct SBspNode {
    Vec3 p{}, n{};
    std::vector<SWPoly>        coplanar;
    std::unique_ptr<SBspNode>  front, back;
};

static void bspBuild(SBspNode& node, std::vector<SWPoly>& polys, int depth) {
    // A split can leave slivers with fewer than three vertices: nothing to
    // draw, and no plane to take (reading verts[2] of one crashed).
    std::erase_if(polys, [](const SWPoly& p) { return p.verts.size() < 3; });
    if (polys.empty())
        return;

    // Node plane from the first polygon with a usable one.
    const Vec3 A = polys[0].verts[0].p;
    const Vec3 B = polys[0].verts[1].p;
    const Vec3 C = polys[0].verts[2].p;
    const Vec3 CR = cross(B - A, C - A);
    if (!(dot(CR, CR) > 1e-12f)) {
        // Degenerate (collinear) first polygon: no splitting plane here.
        for (auto& P : polys)
            node.coplanar.push_back(std::move(P));
        return;
    }
    node.p = A;
    node.n = normalize(CR);

    std::vector<SWPoly> frontList, backList;

    for (auto& POLY : polys) {
        float dist[32];
        const size_t COUNT = std::min<size_t>(POLY.verts.size(), 32);

        bool pos = false, neg = false;
        for (size_t i = 0; i < COUNT; ++i) {
            dist[i] = dot(POLY.verts[i].p - node.p, node.n);
            pos = pos || dist[i] > 1e-4f;
            neg = neg || dist[i] < -1e-4f;
        }

        if (!pos) {
            node.coplanar.push_back(std::move(POLY));
        } else if (!neg) {
            frontList.push_back(std::move(POLY));
        } else {
            SWPoly front, back;
            bool hasFront = false, hasBack = false;
            bspSplitPoly(POLY, node.p, node.n, front, back, hasFront, hasBack);

            if (hasFront)
                frontList.push_back(std::move(front));
            if (hasBack)
                backList.push_back(std::move(back));
            // Fully straddling polygons lose nothing: both pieces carry the
            // texture onward.
        }
    }

    constexpr int MAX_DEPTH = 12;

    if (depth < MAX_DEPTH && !frontList.empty()) {
        node.front = std::make_unique<SBspNode>();
        bspBuild(*node.front, frontList, depth + 1);
    } else {
        for (auto& P : frontList)
            node.coplanar.push_back(std::move(P));
    }

    if (depth < MAX_DEPTH && !backList.empty()) {
        node.back = std::make_unique<SBspNode>();
        bspBuild(*node.back, backList, depth + 1);
    } else {
        for (auto& P : backList)
            node.coplanar.push_back(std::move(P));
    }
}

static void bspTraverse(const SBspNode& node, const Vec3& eye,
                        const std::function<void(const SWPoly&)>& emit) {
    const float SIDE = dot(eye - node.p, node.n);

    // The side the eye is on is NEARER: draw the other side first.
    const SBspNode* FAR_FIRST = SIDE > 0 ? node.back.get() : node.front.get();
    const SBspNode* NEAR_LAST = SIDE > 0 ? node.front.get() : node.back.get();

    if (FAR_FIRST)
        bspTraverse(*FAR_FIRST, eye, emit);

    for (const auto& P : node.coplanar)
        emit(P);

    if (NEAR_LAST)
        bspTraverse(*NEAR_LAST, eye, emit);
}

static GLuint compileShader(
    GLenum type,
    const char* source
) {
    const GLuint shader = glCreateShader(type);

    if (!shader)
        return 0;

    glShaderSource(
        shader,
        1,
        &source,
        nullptr
    );

    glCompileShader(shader);

    GLint compiled = GL_FALSE;

    glGetShaderiv(
        shader,
        GL_COMPILE_STATUS,
        &compiled
    );

    if (compiled == GL_TRUE)
        return shader;

    glDeleteShader(shader);

    return 0;
}

static GLuint linkProgram(
    GLuint vertexShader,
    GLuint fragmentShader
) {
    const GLuint program = glCreateProgram();

    if (!program)
        return 0;

    glAttachShader(
        program,
        vertexShader
    );

    glAttachShader(
        program,
        fragmentShader
    );

    glLinkProgram(program);

    GLint linked = GL_FALSE;

    glGetProgramiv(
        program,
        GL_LINK_STATUS,
        &linked
    );

    if (linked == GL_TRUE)
        return program;

    glDeleteProgram(program);

    return 0;
}

static void setupMeshVAO(
    GLuint vao,
    GLuint vbo,
    const std::vector<float>& vertices
) {
    glBindVertexArray(vao);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        vbo
    );

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            vertices.size() * sizeof(float)
        ),
        vertices.data(),
        GL_STATIC_DRAW
    );

    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        0,
        3,
        GL_FLOAT,
        GL_FALSE,
        5 * sizeof(float),
        reinterpret_cast<void*>(0)
    );

    glEnableVertexAttribArray(1);

    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        5 * sizeof(float),
        reinterpret_cast<void*>(
            3 * sizeof(float)
        )
    );

    glBindVertexArray(0);
}

static void appendVertex(
    std::vector<float>& out,
    float x,
    float y,
    float z,
    float u,
    float v
) {
    out.push_back(x);
    out.push_back(y);
    out.push_back(z);
    out.push_back(u);
    out.push_back(v);
}

static void appendTriangle(
    std::vector<float>& out,
    float x0, float y0, float z0, float u0, float v0,
    float x1, float y1, float z1, float u1, float v1,
    float x2, float y2, float z2, float u2, float v2
) {
    appendVertex(out, x0, y0, z0, u0, v0);
    appendVertex(out, x1, y1, z1, u1, v1);
    appendVertex(out, x2, y2, z2, u2, v2);
}

// UV convention this scene relies on: u grows with +X, v grows with +Y.
// A window quad therefore samples (u0,v0) at its bottom-left and (u1,v1) at
// its top-right, which is why the caller flips logical Y when computing the
// subrect (Hyprland logical coordinates have their origin top-left).
static void appendQuad(
    std::vector<float>& out,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    float x3, float y3, float z3
) {
    appendTriangle(
        out,
        x0, y0, z0, 0.0f, 0.0f,
        x1, y1, z1, 1.0f, 0.0f,
        x2, y2, z2, 1.0f, 1.0f
    );

    appendTriangle(
        out,
        x0, y0, z0, 0.0f, 0.0f,
        x2, y2, z2, 1.0f, 1.0f,
        x3, y3, z3, 0.0f, 1.0f
    );
}

} // namespace

GLScene::GLScene() {
    reset();
}

bool GLScene::initialize() {
    if (m_initialized)
        return true;

    if (!createPrograms())
        return false;

    if (!createMeshes())
        return false;

    m_initialized = true;

    return true;
}

bool GLScene::createPrograms() {
    static constexpr const char* sceneVertexShader = R"GLSL(
#version 300 es

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;

uniform mat4 uMVP;

// xy = subrect origin, zw = subrect size. Keeps the whole scene on one
// program: untextured geometry just passes (0,0,1,1).
uniform vec4 uUVRect;

out vec2 vUV;

void main() {
    gl_Position = uMVP * vec4(aPosition, 1.0);
    vUV = aUV * uUVRect.zw + uUVRect.xy;
}
)GLSL";

    static constexpr const char* sceneFragmentShader = R"GLSL(
#version 300 es

precision mediump float;

in vec2 vUV;

uniform sampler2D uTexture;
uniform int uTextured;
uniform int uPremult;
uniform vec4 uColor;

out vec4 fragColor;

void main() {
    vec4 c = uTextured != 0 ? texture(uTexture, vUV) * uColor : uColor;

    // Premultiplied mode (the 2D<->3D view morph): the scene buffer holds
    // color-already-multiplied-by-alpha, so the composite can stack the
    // environment and the window quads with (ONE, ONE_MINUS_SRC_ALPHA)
    // and the desktop shows through exactly where alpha is 0.
    if (uPremult != 0)
        fragColor = vec4(c.rgb * c.a, c.a);
    else
        fragColor = c;
}
)GLSL";

    static constexpr const char* blitVertexShader = R"GLSL(
#version 300 es

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUV;

out vec2 vUV;

void main() {
    gl_Position = vec4(
        aPosition,
        0.0,
        1.0
    );

    vUV = aUV;
}
)GLSL";

    static constexpr const char* blitFragmentShader = R"GLSL(
#version 300 es

precision mediump float;

in vec2 vUV;

uniform sampler2D uTexture;
uniform float uAlpha;
uniform int uPerPx;

out vec4 fragColor;

void main() {
    // Hyprland framebuffers are vertically flipped relative to the GL
    // convention: their projection maps logical Y (growing DOWN) into NDC
    // without negation, so NDC y = -1 is the TOP of the screen (the DRM
    // scanout compensates). Our scene FBO is rendered with the standard
    // convention (NDC -1 = scene bottom = texture v 0). Sampling with a
    // flipped v puts the scene upright on screen; without it the whole
    // world renders mirrored vertically -- the floor reads as a ceiling,
    // window hover lands on the opposite half of the window, and the
    // camera look feels inverted.
    vec2 uv = vec2(vUV.x, 1.0 - vUV.y);

    vec4 color = texture(
        uTexture,
        uv
    );

    if (uPerPx != 0) {
        // The scene buffer holds a premultiplied image (the view morph's
        // per-pixel environment fade); composite it as such. uAlpha rides
        // both channels so a caller-side fade stays available.
        fragColor = color * uAlpha;
    } else {
        // The scene itself is opaque. uAlpha is only the 2D->3D transition; it
        // must not be multiplied by per-pixel scene alpha left behind by grid or
        // window texture blending.
        fragColor = vec4(
            color.rgb,
            uAlpha
        );
    }
}
)GLSL";

    const GLuint sceneVS =
        compileShader(
            GL_VERTEX_SHADER,
            sceneVertexShader
        );

    if (!sceneVS)
        return false;

    const GLuint sceneFS =
        compileShader(
            GL_FRAGMENT_SHADER,
            sceneFragmentShader
        );

    if (!sceneFS) {
        glDeleteShader(sceneVS);
        return false;
    }

    m_sceneProgram =
        linkProgram(
            sceneVS,
            sceneFS
        );

    glDeleteShader(sceneVS);
    glDeleteShader(sceneFS);

    if (!m_sceneProgram)
        return false;

    const GLuint blitVS =
        compileShader(
            GL_VERTEX_SHADER,
            blitVertexShader
        );

    if (!blitVS)
        return false;

    const GLuint blitFS =
        compileShader(
            GL_FRAGMENT_SHADER,
            blitFragmentShader
        );

    if (!blitFS) {
        glDeleteShader(blitVS);
        return false;
    }

    m_blitProgram =
        linkProgram(
            blitVS,
            blitFS
        );

    glDeleteShader(blitVS);
    glDeleteShader(blitFS);

    if (!m_blitProgram)
        return false;

    static constexpr const char* panoramaVertexShader = R"GLSL(
#version 300 es

layout(location = 0) in vec2 aPosition;

out vec2 vNdc;

void main() {
    vNdc = aPosition;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)GLSL";

    static constexpr const char* panoramaFragmentShader = R"GLSL(
#version 300 es

// highp is mandatory here: mediump cannot represent the longitude with texel
// precision, and the atan2 discontinuity at the +-pi wrap point then shows up
// as a one-pixel seam.
precision highp float;

in vec2 vNdc;

uniform vec3 uFwd;
uniform vec3 uRight;
uniform vec3 uUp;
uniform float uTanHalfX;
uniform float uTanHalfY;
uniform float uLod;
uniform sampler2D uPanorama;

out vec4 fragColor;

void main() {
    // Reconstruct the view ray for this pixel from the camera basis, then
    // sample the equirectangular panorama. fract() keeps u strictly inside
    // [0, 1) so the wrap point never rides the texture's outer edge.
    vec3 dir = normalize(
        uFwd + uRight * (vNdc.x * uTanHalfX) + uUp * (vNdc.y * uTanHalfY));

    float lon = atan(dir.x, -dir.z);
    float lat = asin(clamp(dir.y, -1.0, 1.0));

    vec2 uv = vec2(
        fract(0.5 + lon / 6.28318530718),
        0.5 - lat / 3.14159265359);

    // textureLod with an analytically computed level: the implicit derivative
    // of u explodes at the atan2 wrap point, which made the GPU pick a coarse
    // mip for exactly one pixel -- the seam. An explicit LOD never spikes.
    fragColor = vec4(textureLod(uPanorama, uv, uLod).rgb, 1.0);
}
)GLSL";

    const GLuint PANORAMA_VS =
        compileShader(
            GL_VERTEX_SHADER,
            panoramaVertexShader
        );

    if (!PANORAMA_VS)
        return false;

    const GLuint PANORAMA_FS =
        compileShader(
            GL_FRAGMENT_SHADER,
            panoramaFragmentShader
        );

    if (!PANORAMA_FS) {
        glDeleteShader(PANORAMA_VS);
        return false;
    }

    m_panoramaProgram =
        linkProgram(
            PANORAMA_VS,
            PANORAMA_FS
        );

    glDeleteShader(PANORAMA_VS);
    glDeleteShader(PANORAMA_FS);

    if (!m_panoramaProgram)
        return false;

    m_sceneMVP =
        glGetUniformLocation(
            m_sceneProgram,
            "uMVP"
        );

    m_sceneTexture =
        glGetUniformLocation(
            m_sceneProgram,
            "uTexture"
        );

    m_sceneTextured =
        glGetUniformLocation(
            m_sceneProgram,
            "uTextured"
        );

    m_sceneColorUniform =
        glGetUniformLocation(
            m_sceneProgram,
            "uColor"
        );

    m_sceneUVRect =
        glGetUniformLocation(
            m_sceneProgram,
            "uUVRect"
        );

    m_scenePremult =
        glGetUniformLocation(
            m_sceneProgram,
            "uPremult"
        );

    m_blitTexture =
        glGetUniformLocation(
            m_blitProgram,
            "uTexture"
        );

    m_blitAlpha =
        glGetUniformLocation(
            m_blitProgram,
            "uAlpha"
        );

    m_blitPerPx =
        glGetUniformLocation(
            m_blitProgram,
            "uPerPx"
        );

    m_panoramaFwd =
        glGetUniformLocation(
            m_panoramaProgram,
            "uFwd"
        );

    m_panoramaRight =
        glGetUniformLocation(
            m_panoramaProgram,
            "uRight"
        );

    m_panoramaUp =
        glGetUniformLocation(
            m_panoramaProgram,
            "uUp"
        );

    m_panoramaTanX =
        glGetUniformLocation(
            m_panoramaProgram,
            "uTanHalfX"
        );

    m_panoramaTanY =
        glGetUniformLocation(
            m_panoramaProgram,
            "uTanHalfY"
        );

    m_panoramaLod =
        glGetUniformLocation(
            m_panoramaProgram,
            "uLod"
        );

    m_panoramaSampler =
        glGetUniformLocation(
            m_panoramaProgram,
            "uPanorama"
        );

    return
        m_sceneMVP >= 0 &&
        m_sceneTexture >= 0 &&
        m_sceneTextured >= 0 &&
        m_sceneColorUniform >= 0 &&
        m_sceneUVRect >= 0 &&
        m_scenePremult >= 0 &&
        m_blitTexture >= 0 &&
        m_blitAlpha >= 0 &&
        m_blitPerPx >= 0 &&
        m_panoramaFwd >= 0 &&
        m_panoramaRight >= 0 &&
        m_panoramaUp >= 0 &&
        m_panoramaTanX >= 0 &&
        m_panoramaTanY >= 0 &&
        m_panoramaLod >= 0 &&
        m_panoramaSampler >= 0;
}

bool GLScene::createMeshes() {
    // Unit window quad, centred on the origin, facing +Z (the pose windows
    // are modelled in). Per-window position and size come from the model
    // matrix, so every window is independently placeable and scalable.
    std::vector<float> quad;

    appendQuad(
        quad,
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f
    );

    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);

    setupMeshVAO(m_quadVAO, m_quadVBO, quad);

    m_quadVertexCount =
        static_cast<int>(quad.size() / 5);

    // Ground quad lying in the XZ plane.
    std::vector<float> floor;

    // Counter-clockwise when viewed from above: +Y is the floor normal.
    appendQuad(
        floor,
        -0.5f, 0.0f,  0.5f,
         0.5f, 0.0f,  0.5f,
         0.5f, 0.0f, -0.5f,
        -0.5f, 0.0f, -0.5f
    );

    glGenVertexArrays(1, &m_floorVAO);
    glGenBuffers(1, &m_floorVBO);

    setupMeshVAO(m_floorVAO, m_floorVBO, floor);

    m_floorVertexCount =
        static_cast<int>(floor.size() / 5);

    std::vector<float> grid;

    constexpr int   gridMin = -20;
    constexpr int   gridMax = 20;

    for (int i = gridMin; i <= gridMax; ++i) {
        const float p = static_cast<float>(i);

        grid.push_back(static_cast<float>(gridMin));
        grid.push_back(FLOOR_Y);
        grid.push_back(p);

        grid.push_back(static_cast<float>(gridMax));
        grid.push_back(FLOOR_Y);
        grid.push_back(p);

        grid.push_back(p);
        grid.push_back(FLOOR_Y);
        grid.push_back(static_cast<float>(gridMin));

        grid.push_back(p);
        grid.push_back(FLOOR_Y);
        grid.push_back(static_cast<float>(gridMax));
    }

    glGenVertexArrays(1, &m_gridVAO);
    glGenBuffers(1, &m_gridVBO);

    glBindVertexArray(m_gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVBO);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(grid.size() * sizeof(float)),
        grid.data(),
        GL_STATIC_DRAW
    );

    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        0,
        3,
        GL_FLOAT,
        GL_FALSE,
        3 * sizeof(float),
        reinterpret_cast<void*>(0)
    );

    glDisableVertexAttribArray(1);

    glBindVertexArray(0);

    m_gridVertexCount =
        static_cast<int>(grid.size() / 3);

    const float fullscreen[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,

        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
    };

    glGenVertexArrays(1, &m_fullscreenVAO);
    glGenBuffers(1, &m_fullscreenVBO);

    glBindVertexArray(m_fullscreenVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_fullscreenVBO);

    glBufferData(
        GL_ARRAY_BUFFER,
        sizeof(fullscreen),
        fullscreen,
        GL_STATIC_DRAW
    );

    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        0,
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(float),
        reinterpret_cast<void*>(0)
    );

    glEnableVertexAttribArray(1);

    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(float),
        reinterpret_cast<void*>(2 * sizeof(float))
    );

    glBindVertexArray(0);

    // Dynamic screen-space crosshair. Four independent rectangular arms are
    // used instead of GL_LINES so the shape stays visibly cross-like and its
    // thickness remains stable across drivers.
    glGenVertexArrays(1, &m_crosshairVAO);
    glGenBuffers(1, &m_crosshairVBO);

    glBindVertexArray(m_crosshairVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_crosshairVBO);

    const std::vector<float> crosshair(24 * 5, 0.0f);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(crosshair.size() * sizeof(float)),
        crosshair.data(),
        GL_DYNAMIC_DRAW
    );

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        5 * sizeof(float), reinterpret_cast<void*>(0)
    );

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float))
    );

    glBindVertexArray(0);

    // Typing-mode cursor: same vertex layout, filled per frame with
    // already-projected NDC positions.
    glGenVertexArrays(1, &m_pointerVAO);
    glGenBuffers(1, &m_pointerVBO);

    glBindVertexArray(m_pointerVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_pointerVBO);

    const std::vector<float> pointer(kPointerVerts * 5, 0.0f);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(pointer.size() * sizeof(float)),
        pointer.data(),
        GL_DYNAMIC_DRAW
    );

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        5 * sizeof(float), reinterpret_cast<void*>(0)
    );

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float))
    );

    glBindVertexArray(0);

    return true;
}

bool GLScene::ensureSceneFramebuffer(
    int width,
    int height
) {
    if (
        m_sceneFBO &&
        m_sceneWidth == width &&
        m_sceneHeight == height
    )
        return true;

    if (!m_sceneFBO) {
        glGenFramebuffers(1, &m_sceneFBO);
        glGenTextures(1, &m_sceneColor);
        glGenRenderbuffers(1, &m_sceneDepth);
    }

    m_sceneWidth = width;
    m_sceneHeight = height;

    glBindTexture(GL_TEXTURE_2D, m_sceneColor);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr
    );

    glBindTexture(GL_TEXTURE_2D, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, m_sceneDepth);

    glRenderbufferStorage(
        GL_RENDERBUFFER,
        GL_DEPTH_COMPONENT24,
        width,
        height
    );

    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);

    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        m_sceneColor,
        0
    );

    glFramebufferRenderbuffer(
        GL_FRAMEBUFFER,
        GL_DEPTH_ATTACHMENT,
        GL_RENDERBUFFER,
        m_sceneDepth
    );

    const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;

    glDrawBuffers(1, &drawBuffer);

    const GLenum status =
        glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return status == GL_FRAMEBUFFER_COMPLETE;
}

void GLScene::drawQuad(
    unsigned int vao,
    int vertexCount,
    const Mat4& mvp,
    unsigned int texture,
    const float uvRect[4],
    float r, float g, float b, float a
) {
    glUseProgram(m_sceneProgram);

    glUniformMatrix4fv(
        m_sceneMVP,
        1,
        GL_FALSE,
        mvp.m.data()
    );

    glUniform1i(m_sceneTextured, texture ? 1 : 0);

    glUniform4f(m_sceneColorUniform, r, g, b, a);

    glUniform4f(
        m_sceneUVRect,
        uvRect[0],
        uvRect[1],
        uvRect[2] - uvRect[0],
        uvRect[3] - uvRect[1]
    );

    if (texture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(m_sceneTexture, 0);
    }

    glBindVertexArray(vao);

    glDrawArrays(GL_TRIANGLES, 0, vertexCount);

    glBindVertexArray(0);

    if (texture)
        glBindTexture(GL_TEXTURE_2D, 0);
}

void GLScene::drawFloor(
    const Mat4& vp
) {
    // Solid ground just under the grid. The floor is deliberately single-sided:
    // seeing this plane from below is a ceiling by definition, so back-face
    // culling makes the world coordinate system visually unambiguous.
    // The platform is centered on world zero and matches the grid lines'
    // extent (both halves of the visual and the collision slab in main.cpp).
    const Mat4 model =
        Mat4::translation({0.0f, FLOOR_Y - 0.02f, 0.0f}) *
        Mat4::scale({40.0f, 1.0f, 40.0f});

    static constexpr float fullUV[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    GLint oldCullMode = GL_BACK;
    GLint oldFrontFace = GL_CCW;
    GLboolean oldCull = GL_FALSE;

    glGetBooleanv(GL_CULL_FACE, &oldCull);
    glGetIntegerv(GL_CULL_FACE_MODE, &oldCullMode);
    glGetIntegerv(GL_FRONT_FACE, &oldFrontFace);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    drawQuad(
        m_floorVAO,
        m_floorVertexCount,
        vp * model,
        0,
        fullUV,
        0.018f, 0.026f, 0.040f, 1.0f
    );

    glCullFace(static_cast<GLenum>(oldCullMode));
    glFrontFace(static_cast<GLenum>(oldFrontFace));

    if (oldCull)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
}

void GLScene::refreshPanorama() {
    if (m_panoramaPath.empty()) {
        if (m_panoramaTex) {
            glDeleteTextures(1, &m_panoramaTex);
            m_panoramaTex = 0;
            m_panoramaLoaded.clear();
            m_panoramaMtimeValid = false;
        }
        return;
    }

    std::error_code ec;
    const auto MTIME = std::filesystem::last_write_time(m_panoramaPath, ec);

    if (!ec && m_panoramaMtimeValid && m_panoramaTex &&
        MTIME == m_panoramaMtime && m_panoramaLoaded == m_panoramaPath)
        return; // unchanged since the last load

    m_panoramaMtime      = MTIME;
    m_panoramaMtimeValid = !ec;
    m_panoramaLoaded     = m_panoramaPath;

    Hyprgraphics::CImage image(m_panoramaPath);

    auto surface = image.success() ? image.cairoSurface() : nullptr;

    if (!surface || surface->status() != CAIRO_STATUS_SUCCESS) {
        // Drop the old texture so the fallback void shows through.
        if (m_panoramaTex) {
            glDeleteTextures(1, &m_panoramaTex);
            m_panoramaTex = 0;
        }
        return;
    }

    const int W      = static_cast<int>(surface->size().x);
    const int H      = static_cast<int>(surface->size().y);
    const int STRIDE = surface->stride();
    const auto* SRC  = surface->data();

    if (W <= 0 || H <= 0 || !SRC) {
        if (m_panoramaTex) {
            glDeleteTextures(1, &m_panoramaTex);
            m_panoramaTex = 0;
        }
        return;
    }

    // Cairo ARGB32 is premultiplied BGRA in memory; the panorama background
    // is opaque, so a straight channel swap to RGBA is the whole conversion.
    std::vector<unsigned char> pixels(static_cast<size_t>(W) * H * 4);

    for (int y = 0; y < H; ++y) {
        const auto* row = SRC + static_cast<size_t>(y) * STRIDE;
        auto* dst = pixels.data() + static_cast<size_t>(y) * W * 4;

        for (int x = 0; x < W; ++x) {
            dst[x * 4 + 0] = row[x * 4 + 2];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }

    if (m_panoramaTex)
        glDeleteTextures(1, &m_panoramaTex);

    glGenTextures(1, &m_panoramaTex);

    glBindTexture(GL_TEXTURE_2D, m_panoramaTex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);

    m_panoramaW = W;
}

// Diff-apply the config object list against the slot vector.
void GLScene::setSceneObjects(const std::vector<SSceneSpec>& specs) {
    while (m_slots.size() > specs.size()) {
        if (m_slots.back().model)
            m_slots.back().model->destroy();
        m_slots.pop_back();
    }

    m_slots.resize(specs.size());

    for (size_t i = 0; i < specs.size(); ++i) {
        auto& S = m_slots[i];

        if (!S.model)
            S.model = std::make_unique<CMapModel>();

        const bool TRANSFORM_CHANGED =
            S.spec.position.x != specs[i].position.x ||
            S.spec.position.y != specs[i].position.y ||
            S.spec.position.z != specs[i].position.z ||
            S.spec.rotationDeg.x != specs[i].rotationDeg.x ||
            S.spec.rotationDeg.y != specs[i].rotationDeg.y ||
            S.spec.rotationDeg.z != specs[i].rotationDeg.z ||
            S.spec.scale.x != specs[i].scale.x ||
            S.spec.scale.y != specs[i].scale.y ||
            S.spec.scale.z != specs[i].scale.z;

        const bool CENTER_CHANGED =
            S.spec.center != specs[i].center ||
            S.spec.centerOffset.x != specs[i].centerOffset.x ||
            S.spec.centerOffset.y != specs[i].centerOffset.y ||
            S.spec.centerOffset.z != specs[i].centerOffset.z;

        S.spec = specs[i];

        S.model->setEmissiveScale(S.spec.emissiveScale);
        S.model->setFlat(S.spec.flat);

        if (CENTER_CHANGED)
            S.model->setCenter(S.spec.center, S.spec.centerOffset);

        if (TRANSFORM_CHANGED)
            S.model->setTransform(S.spec.position, S.spec.rotationDeg,
                                  S.spec.scale);
    }
}

void GLScene::setSceneObjectTransform(size_t index, const Vec3& position,
                                      const Vec3& rotationDeg) {
    if (index >= m_slots.size() || !m_slots[index].model)
        return;

    auto& S = m_slots[index];
    S.spec.position    = position;
    S.spec.rotationDeg = rotationDeg;
    S.model->setTransform(S.spec.position, S.spec.rotationDeg, S.spec.scale);
}

uint64_t GLScene::sceneFingerprint() const {
    // Fold per-model generations + slot count: any (re)load, transform
    // change, or add/remove changes the fingerprint.
    uint64_t F = 1469598103934665603ull;

    const auto MIX = [&](uint64_t V) {
        F ^= V;
        F *= 1099511628211ull;
    };

    MIX(m_slots.size());

    for (const auto& S : m_slots)
        MIX(S.model ? S.model->generation() : 0ull);

    return F;
}

// Loads (or reloads) each scene object when its config path or the file's
// mtime changed. Runs inside render() so the EGL context is current -- the
// same rule as the panorama.
void GLScene::refreshScene() {
    for (auto& S : m_slots) {
        // A file read and decoded on the model's worker gets its GL objects
        // here, with the context current.
        if (S.model)
            S.model->poll();

        const std::string& CFG_PATH = S.spec.path;

        if (CFG_PATH.empty()) {
            if (S.model && (S.model->loaded() || S.model->pending())) {
                S.model->destroy();
                S.loadedPath.clear();
                S.mtimeValid = false;
            }
            continue;
        }

        // Expand a leading tilde like the panorama path does -- config
        // paths arrive as "~/..." and cgltf would look for a literal '~'.
        std::string path = CFG_PATH;
        if (path.starts_with('~')) {
            if (const char* HOME = getenv("HOME"))
                path = std::string{HOME} + path.substr(1);
        }

        std::error_code ec;
        const auto MTIME = std::filesystem::last_write_time(path, ec);

        // The same file as last time, also when it is still missing. Loading,
        // loaded or failed, it is not started again until it changes: a load
        // per frame would start a worker per frame.
        const bool SAME_FILE = S.loadedPath == path &&
            S.mtimeValid == !ec && (ec || MTIME == S.mtime);
        const bool UNCHANGED = S.model && SAME_FILE &&
            (S.model->loaded() || S.model->pending() || S.model->failed());

        if (UNCHANGED)
            continue;

        S.mtime      = MTIME;
        S.mtimeValid = !ec;
        S.loadedPath = path;

        S.model->load(path, S.spec.position, S.spec.rotationDeg,
                      S.spec.scale);
    }
}

// The player's collision capsule outline (F3): three circles + four
// verticals, rebuilt every drawn frame (a couple hundred floats).
void GLScene::drawPlayerDebugCapsule(const Mat4& vp) {
    if (!m_pDbgProgram) {
        static const char* VS = R"GLSL(
#version 320 es
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)GLSL";
        static const char* FS = R"GLSL(
#version 320 es
precision mediump float;
out vec4 fragColor;
void main() { fragColor = vec4(0.2, 1.0, 0.3, 1.0); }
)GLSL";
        const GLuint VSx = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(VSx, 1, &VS, nullptr);
        glCompileShader(VSx);
        const GLuint FSx = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(FSx, 1, &FS, nullptr);
        glCompileShader(FSx);
        const GLuint P = glCreateProgram();
        glAttachShader(P, VSx);
        glAttachShader(P, FSx);
        glLinkProgram(P);
        glDeleteShader(VSx);
        glDeleteShader(FSx);
        m_pDbgProgram = P;
        m_pDbgMVP     = glGetUniformLocation(P, "uMVP");
    }

    const float R = 0.3f;             // Camera::kBodyHalfWidth
    const float CY = m_pDbgCenter.y;  // the Jolt body center (feet + 0.9)
    const float HH = 0.6f;            // the cylinder half height (0.9 - r)
    const int SEG = 24;

    std::vector<float> V;
    V.reserve(3 * (3 * SEG * 2 + 8));
    const auto CIRCLE = [&](float y) {
        for (int s = 0; s < SEG; ++s) {
            const float A0 = float(s) / SEG * 6.2831853f;
            const float A1 = float(s + 1) / SEG * 6.2831853f;
            V.push_back(m_pDbgCenter.x + std::cos(A0) * R);
            V.push_back(CY + y);
            V.push_back(m_pDbgCenter.z + std::sin(A0) * R);
            V.push_back(m_pDbgCenter.x + std::cos(A1) * R);
            V.push_back(CY + y);
            V.push_back(m_pDbgCenter.z + std::sin(A1) * R);
        }
    };
    CIRCLE(-HH);
    CIRCLE(0.f);
    CIRCLE(HH);
    for (int s = 0; s < 4; ++s) {
        const float A = float(s) / 4 * 6.2831853f + 0.3926991f;
        const float X = m_pDbgCenter.x + std::cos(A) * R;
        const float Z = m_pDbgCenter.z + std::sin(A) * R;
        V.push_back(X); V.push_back(CY - HH); V.push_back(Z);
        V.push_back(X); V.push_back(CY + HH); V.push_back(Z);
    }
    m_pDbgVerts = static_cast<int>(V.size() / 3);

    if (!m_pDbgVAO) {
        glGenVertexArrays(1, &m_pDbgVAO);
        glGenBuffers(1, &m_pDbgVBO);
        glBindVertexArray(m_pDbgVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_pDbgVBO);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glBindVertexArray(0);
    }

    glUseProgram(m_pDbgProgram);
    glUniformMatrix4fv(m_pDbgMVP, 1, GL_FALSE, vp.m.data());
    glBindVertexArray(m_pDbgVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_pDbgVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(V.size() * sizeof(float)),
                 V.data(), GL_STREAM_DRAW);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_LINES, 0, m_pDbgVerts);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

// Loads (or reloads) the player character when the config path or the
// file's mtime changed; applies the per-state animation assignment after a
// successful load. Runs inside render() so the EGL context is current.
void GLScene::refreshPlayer() {
    const std::string& CFG_PATH = m_playerCfg.path;

    if (CFG_PATH.empty()) {
        if (m_player.loaded())
            m_player.destroy();
        m_playerPath.clear();
        return;
    }

    std::string path = CFG_PATH;
    if (path.starts_with('~')) {
        if (const char* HOME = getenv("HOME"))
            path = std::string{HOME} + path.substr(1);
    }

    std::error_code ec;
    const auto MTIME = std::filesystem::last_write_time(path, ec);

    const bool UNCHANGED = m_player.loaded() && m_playerPath == path &&
        !ec && m_playerMtimeValid && MTIME == m_playerMtime;
    if (UNCHANGED)
        return;

    m_playerPath       = path;
    m_playerMtime      = MTIME;
    m_playerMtimeValid = !ec;

    if (ec) {
        if (m_player.loaded())
            m_player.destroy();
        return;
    }

    if (m_player.load(path)) {
        for (int s = 0; s < CPlayerModel::kStateCount; ++s) {
            const auto ST = static_cast<CPlayerModel::EState>(s);
            if (!m_playerCfg.animName[s].empty())
                m_player.setAnim(ST, m_playerCfg.animName[s]);
            else
                m_player.setAnim(ST, m_playerCfg.animIdx[s]);
            m_player.setAnimSpeed(ST, m_playerCfg.animSpeed[s]);
        }
    }
}

void GLScene::drawPanorama(float aspect) {
    if (!m_panoramaTex || !m_panoramaProgram)
        return;

    glUseProgram(m_panoramaProgram);

    Vec3 FWD   = m_camera.forward();
    Vec3 RIGHT = m_camera.right();
    Vec3 UP    = cross(RIGHT, FWD);
    // Zoomed fov (C key): the panorama must narrow with the scene, so its
    // half-tangent divides by the magnification exactly like the render
    // projection's fov does.
    const float TANY =
        std::tan(kFovDeg * PI / 360.0f) / std::max(m_zoom, 0.01f);

    // Roll (walk bob): rotate the pixel->ray basis around the view axis by
    // the SAME angle the view matrix tilts its up vector, or the panorama
    // stays level while the scene rolls. Matches Camera::view(): screen up
    // = UP*cos(r) + RIGHT*sin(r), screen right = RIGHT*cos(r) - UP*sin(r).
    const float RROLL = m_camera.roll;
    Vec3 RRIGHT = RIGHT * std::cos(RROLL) - UP * std::sin(RROLL);
    Vec3 RUP    = UP * std::cos(RROLL) + RIGHT * std::sin(RROLL);

    // The front third-person view looks BACK along the look axis: the view
    // matrix and the world flip with it, and the panorama's pixel->ray
    // basis must flip too (direction and screen right; the up stays).
    if (m_camera.mirrorView) {
        FWD   = FWD * -1.0f;
        RRIGHT = RRIGHT * -1.0f;
    }

    // Analytic mip level: texels per screen pixel at the view centre. The
    // panorama is W texels around 2*pi radians; one screen pixel spans about
    // 2*tanX / screenWidth radians.
    float lod = 0.0f;

    if (m_panoramaW > 0 && m_sceneWidth > 0) {
        const float texelsPerPixel =
            static_cast<float>(m_panoramaW) / (2.0f * PI) *
            (2.0f * TANY * aspect) / static_cast<float>(m_sceneWidth);

        lod = std::clamp(std::log2(std::max(texelsPerPixel, 0.03125f)), 0.0f, 12.0f);
    }

    glUniform3f(m_panoramaFwd, FWD.x, FWD.y, FWD.z);
    glUniform3f(m_panoramaRight, RRIGHT.x, RRIGHT.y, RRIGHT.z);
    glUniform3f(m_panoramaUp, RUP.x, RUP.y, RUP.z);
    glUniform1f(m_panoramaTanX, TANY * aspect);
    glUniform1f(m_panoramaTanY, TANY);
    glUniform1f(m_panoramaLod, lod);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_panoramaTex);
    glUniform1i(m_panoramaSampler, 0);

    // Pure background: no depth interaction, everything else draws on top.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glBindVertexArray(m_fullscreenVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLScene::drawGrid(
    const Mat4& vp
) {
    // Lines, not triangles, and translucent: the grid is the visual floor and
    // sits just above the solid ground quad so the two never z-fight.
    glUseProgram(m_sceneProgram);

    glUniformMatrix4fv(
        m_sceneMVP,
        1,
        GL_FALSE,
        vp.m.data()
    );

    glUniform1i(m_sceneTextured, 0);
    glUniform4f(m_sceneColorUniform, 0.08f, 0.18f, 0.30f, 0.62f);
    glUniform4f(m_sceneUVRect, 0.0f, 0.0f, 1.0f, 1.0f);

    glBindVertexArray(m_gridVAO);

    glDrawArrays(GL_LINES, 0, m_gridVertexCount);

    glBindVertexArray(0);
}

void GLScene::drawWindows(
    const Mat4& vp,
    const std::vector<WindowRender>& windows
) {
    // World-space polygons for the visible windows (the unit quad's corners
    // run through each window's model matrix; uvRect is folded into the
    // corner UVs). A depth slab adds the back face and one wall quad per
    // silhouette segment; the slab's interior is kept SEPARATE from the
    // content face and assembled after all faces -- see the invariant at
    // the assembly below.
    struct SWinSlab {
        SWPoly              front{}; // the content face
        SWPoly              back{};  // the mirrored content face
        std::vector<SWPoly> inner;   // the walls
        float               dist = 0.f;
        bool                fromBehind = false; // eye on the back side
    };
    std::vector<SWinSlab> slabs;
    slabs.reserve(windows.size());

    const Vec3 eye = m_camera.position;

    for (const auto& window : windows) {
        if (!window.texture)
            continue;

        if (window.alpha <= 0.0f)
            continue;

        if (window.width <= 0.0f || window.height <= 0.0f)
            continue;

        const Mat4 model =
            Mat4::translation({window.x, window.y, window.z}) *
            Mat4::rotationY(window.yaw) *
            Mat4::rotationX(window.pitch) *
            Mat4::rotationZ(window.roll) *
            Mat4::scale({window.width, window.height, 1.0f});

        // Local quad point -> world, through the model matrix columns (the
        // scale in m[12..] means this is NOT a plain matrix*vec).
        const auto WORLD = [&model](float lx, float ly, float lz) {
            return Vec3{
                model.m[0] * lx + model.m[4] * ly + model.m[8] * lz + model.m[12],
                model.m[1] * lx + model.m[5] * ly + model.m[9] * lz + model.m[13],
                model.m[2] * lx + model.m[6] * ly + model.m[10] * lz + model.m[14],
            };
        };

        const float CU[4] = {0.f, 1.f, 1.f, 0.f};
        const float CV[4] = {0.f, 0.f, 1.f, 1.f};
        const float LX[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
        const float LY[4] = {-0.5f, -0.5f, 0.5f, 0.5f};

        SWinSlab slab;
        slab.front.tex   = window.texture;
        slab.front.alpha = window.alpha;

        for (int c = 0; c < 4; ++c) {
            SWVert V{
                WORLD(LX[c], LY[c], 0.f),
                window.u0 + (window.u1 - window.u0) * CU[c],
                window.v0 + (window.v1 - window.v0) * CV[c],
            };
            slab.front.verts.push_back(V);
        }

        // Which side of the window the eye is on: the slab's painter order
        // flips with it (see the assembly below).
        const Vec3 NORMAL =
            normalize(Vec3{model.m[8], model.m[9], model.m[10]});
        slab.fromBehind =
            dot(eye - Vec3{window.x, window.y, window.z}, NORMAL) < 0.0f;

        // --- depth slab -----------------------------------------------------
        // Extruded backwards along the window's normal: the front face keeps
        // its exact plane, so picking, input mapping and collision geometry
        // are untouched. Walls hug the captured alpha silhouette (when one
        // was traced), so rounded corners keep their shape, and every wall
        // samples its silhouette texel -- the window texture's edge colors
        // paint the whole side.
        if (window.depth > 0.0f) {
            const float DEPTH = window.depth;

            // Back face: the mirrored content quad at the rear plane. It
            // renders ONLY for an eye on the back side (see the assembly):
            // each content face is visible exclusively from its own side,
            // so the two never stack through a translucent window.
            slab.back.tex   = window.texture;
            slab.back.alpha = window.alpha;

            for (int c = 0; c < 4; ++c) {
                SWVert V{
                    WORLD(LX[c], LY[c], 0.f) - NORMAL * DEPTH,
                    window.u0 + (window.u1 - window.u0) * CU[c],
                    window.v0 + (window.v1 - window.v0) * CV[c],
                };
                slab.back.verts.push_back(V);
            }

            const auto SUBRECT_UV = [&window](const Vec2& P) {
                return Vec2{
                    window.u0 + (window.u1 - window.u0) * P.x,
                    // Outline y is top-down (row 0 = the box's top), and v1
                    // is the subrect's top edge: v runs from v1 (y=0) to
                    // v0 (y=1).
                    window.v1 + (window.v0 - window.v1) * P.y,
                };
            };

            // Wall loops: the traced silhouette when available, else the
            // plain box outline (first frames before a mask was read back).
            //
            // Every segment is emitted TWICE-SIDED -- no facing test: a
            // translucent window must show its FAR walls through the front
            // face (they blend in BSP order, far first), and for opaque
            // windows the hidden walls are simply depth-rejected by the
            // front face drawn after them.
            const auto EMIT_WALLS = [&](const std::vector<Vec2>& pts,
                                        const std::vector<Vec2>& uvs) {
                const size_t N = pts.size();
                if (N < 3)
                    return;

                for (size_t i = 0; i < N; ++i) {
                    const Vec2& A  = pts[i];
                    const Vec2& B  = pts[(i + 1) % N];
                    const Vec2& UA = uvs[i];
                    const Vec2& UB = uvs[(i + 1) % N];

                    const Vec3 AF = WORLD(A.x - 0.5f, 0.5f - A.y, 0.f);
                    const Vec3 BF = WORLD(B.x - 0.5f, 0.5f - B.y, 0.f);

                    const Vec2 SUA = SUBRECT_UV(UA);
                    const Vec2 SUB = SUBRECT_UV(UB);

                    SWPoly wall;
                    wall.tex   = window.texture;
                    wall.alpha = window.alpha;

                    wall.verts.push_back(SWVert{AF, SUA.x, SUA.y});
                    wall.verts.push_back(SWVert{BF, SUB.x, SUB.y});
                    wall.verts.push_back(
                        SWVert{BF - NORMAL * DEPTH, SUB.x, SUB.y});
                    wall.verts.push_back(
                        SWVert{AF - NORMAL * DEPTH, SUA.x, SUA.y});

                    slab.inner.push_back(std::move(wall));
                }
            };

            if (window.outlines && !window.outlines->empty()) {
                for (const auto& LOOP : *window.outlines)
                    EMIT_WALLS(LOOP.pts, LOOP.uvs);
            } else {
                // No silhouette traced (yet) -- the mask readback may fail
                // legitimately. The slab must never lose its sides: fall
                // back to the plain box outline.
                static const std::vector<Vec2> RECT = {
                    {0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
                EMIT_WALLS(RECT, RECT);
            }
        }

        const float DX = slab.front.verts[0].p.x - eye.x;
        const float DY = slab.front.verts[0].p.y - eye.y;
        const float DZ = slab.front.verts[0].p.z - eye.z;
        slab.dist = DX * DX + DY * DY + DZ * DZ;

        slabs.push_back(std::move(slab));
    }

    // Painter-order assembly, windows far-to-near (coplanar overlapping
    // faces blend far first). Within a window the surfaces are ordered
    // far-to-near FOR THE EYE'S SIDE OF THE SLAB -- walls then face from
    // the front; face then walls from behind. This order survives the BSP
    // unchanged for all non-crossing polys (the builder files every
    // behind-or-on-plane poly into the node's coplanar list in insertion
    // order), so whichever surface is farthest blends FIRST and a
    // translucent slab shows its far walls through the near face from BOTH
    // sides -- no ordering luck. Genuinely crossing polys are still split
    // by the node planes as before.
    std::sort(
        slabs.begin(),
        slabs.end(),
        [](const SWinSlab& a, const SWinSlab& b) { return a.dist > b.dist; });

    std::vector<SWPoly> polys;
    polys.reserve(slabs.size() * 8);

    for (auto& S : slabs) {
        // Walls first, then the content face the eye is actually on: the
        // other content face is not emitted at all, so the two never stack
        // through a translucent window -- from the front you see the front
        // face (plus the far walls through it), from behind the back face
        // (plus the far walls through it).
        for (auto& P : S.inner)
            polys.push_back(std::move(P));

        polys.push_back(std::move(S.fromBehind ? S.back : S.front));
    }

    if (polys.empty())
        return;

    // Exact ordering: split crossing quads along each other's planes and
    // traverse back-to-front from the eye.
    SBspNode root;
    bspBuild(root, polys, 0);

    std::vector<SWPoly> ordered;
    bspTraverse(root, eye, [&ordered](const SWPoly& P) {
        ordered.push_back(P);
    });

    if (ordered.empty())
        return;

    if (!m_polyVAO) {
        glGenVertexArrays(1, &m_polyVAO);
        glGenBuffers(1, &m_polyVBO);

        glBindVertexArray(m_polyVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_polyVBO);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glUseProgram(m_sceneProgram);
    glUniformMatrix4fv(m_sceneMVP, 1, GL_FALSE, vp.m.data());
    glUniform4f(m_sceneUVRect, 0.f, 0.f, 1.f, 1.f);
    glUniform1i(m_sceneTexture, 0);
    // Premultiplied mode (the view morph): color is stored already scaled
    // by alpha, and the alpha channel accumulates -- the composite then
    // stacks windows and environment in one consistent convention.
    glUniform1i(m_scenePremult, m_windowsPremultiplied ? 1 : 0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    if (m_windowsPremultiplied)
        glBlendFuncSeparate(
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE, GL_ONE_MINUS_SRC_ALPHA
        );
    else
        glBlendFuncSeparate(
            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
            GL_ZERO, GL_ONE
        );

    glBindVertexArray(m_polyVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_polyVBO);

    glActiveTexture(GL_TEXTURE0);

    // Consecutive polys with the same texture and alpha merge into ONE
    // upload + draw: a depth slab adds ~100 wall quads per window, and a
    // draw call per wall quad would sink the frame. Only CONSECUTIVE polys
    // group, so the BSP's back-to-front blend order is untouched -- batches
    // interleave exactly where windows overlap.
    std::vector<float> verts;

    for (size_t i = 0; i < ordered.size();) {
        size_t j = i;
        while (j < ordered.size() && ordered[j].tex == ordered[i].tex &&
               ordered[j].alpha == ordered[i].alpha)
            ++j;

        verts.clear();

        for (size_t k = i; k < j; ++k) {
            const auto& P = ordered[k];

            for (size_t v = 1; v + 1 < P.verts.size(); ++v) {
                const auto& A = P.verts[0];
                const auto& B = P.verts[v];
                const auto& C = P.verts[v + 1];

                verts.insert(verts.end(), {
                    A.p.x, A.p.y, A.p.z, A.u, A.v,
                    B.p.x, B.p.y, B.p.z, B.u, B.v,
                    C.p.x, C.p.y, C.p.z, C.u, C.v,
                });
            }
        }

        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                     verts.data(), GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ordered[i].tex);
        glUniform1i(m_sceneTextured, 1);
        glUniform4f(m_sceneColorUniform, 1.f, 1.f, 1.f, ordered[i].alpha);

        glDrawArrays(GL_TRIANGLES, 0,
                     static_cast<GLint>(verts.size() / 5));

        i = j;
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
}

// Restored from the pre-unification revision: the black cross with the
// white outline at the screen centre (screen-space, scene program).
void GLScene::drawCrosshair(int width, int height) {
    if (!m_crosshairVisible)
        return;

    if (!m_crosshairVAO || !m_crosshairVBO || width <= 0 || height <= 0)
        return;

    const float pxX = 2.0f / static_cast<float>(width);
    const float pxY = 2.0f / static_cast<float>(height);

    constexpr float OUTER_PX   = 10.0f;
    constexpr float INNER_PX   = 3.5f;
    constexpr float THICK_PX   = 1.4f;
    constexpr float OUTLINE_PX = 1.0f;

    const auto addRect = [](std::vector<float>& v, float x0, float y0, float x1, float y1) {
        const float z = 0.0f;
        const float u = 0.0f;
        const float t = 0.0f;
        const float verts[] = {
            x0,y0,z,u,t, x1,y0,z,u,t, x1,y1,z,u,t,
            x0,y0,z,u,t, x1,y1,z,u,t, x0,y1,z,u,t,
        };
        v.insert(v.end(), std::begin(verts), std::end(verts));
    };

    // One layer of the crosshair: four arms with the given pixel metrics and
    // color, uploaded and drawn as screen-space geometry.
    const auto drawLayer = [&](float outer, float inner, float thick,
                               float r, float g, float b, float a) {
        const float outerX = outer * pxX;
        const float innerX = inner * pxX;
        const float halfTX = (thick * pxX) * 0.5f;
        const float outerY = outer * pxY;
        const float innerY = inner * pxY;
        const float halfTY = (thick * pxY) * 0.5f;

        std::vector<float> verts;
        verts.reserve(24 * 5);

        // Horizontal arm.
        addRect(verts, -outerX, -halfTY, -innerX, halfTY);
        addRect(verts,  innerX, -halfTY,  outerX, halfTY);
        // Vertical arm.
        addRect(verts, -halfTX,  innerY, halfTX, outerY);
        addRect(verts, -halfTX, -outerY, halfTX, -innerY);

        glUseProgram(m_sceneProgram);

        const Mat4 IDENTITY = Mat4::identity();
        glUniformMatrix4fv(m_sceneMVP, 1, GL_FALSE, IDENTITY.m.data());
        glUniform1i(m_sceneTextured, 0);
        glUniform4f(m_sceneColorUniform, r, g, b, a);
        glUniform4f(m_sceneUVRect, 0.0f, 0.0f, 1.0f, 1.0f);

        glBindBuffer(GL_ARRAY_BUFFER, m_crosshairVBO);
        glBufferSubData(
            GL_ARRAY_BUFFER, 0,
            static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
            verts.data()
        );

        glBindVertexArray(m_crosshairVAO);
        glDrawArrays(GL_TRIANGLES, 0, 24);
        glBindVertexArray(0);
    };

    // Black core with a white outline: the slightly larger white cross is
    // drawn first, so the black one keeps full contrast on any background.
    drawLayer(OUTER_PX + OUTLINE_PX, std::max(0.0f, INNER_PX - OUTLINE_PX),
        THICK_PX + 2.0f * OUTLINE_PX, 1.0f, 1.0f, 1.0f, 1.0f);
    drawLayer(OUTER_PX, INNER_PX, THICK_PX, 0.0f, 0.0f, 0.0f, 1.0f);
}

void GLScene::drawPointer(const Mat4& vp, int width, int height) {
    if (m_pointer.mode == SPointer::EMode::Hidden || !m_pointerVAO ||
        !m_pointerVBO || width <= 0 || height <= 0)
        return;

    // Arrow outline in cursor pixels (x right, y down, tip at the origin),
    // as five triangles.
    static constexpr float ARROW[][2] = {
        {0.0f, 0.0f},  {0.0f, 16.0f}, {4.0f, 12.5f},
        {0.0f, 0.0f},  {4.0f, 12.5f}, {6.5f, 11.5f},
        {0.0f, 0.0f},  {6.5f, 11.5f}, {11.5f, 11.5f},
        {4.0f, 12.5f}, {7.0f, 19.0f}, {9.5f, 18.0f},
        {4.0f, 12.5f}, {9.5f, 18.0f}, {6.5f, 11.5f},
    };
    static_assert(std::size(ARROW) == kPointerVerts);
    static_assert(kPointerVerts >= 6, "the VBO also holds the image quad");

    // The built-in arrow is slightly larger than its bare outline, about a
    // 28px cursor; a client image is drawn at its own size.
    constexpr float ARROW_SIZE = 1.4f;
    const float SIZE = m_pointer.texture ? 1.0f : ARROW_SIZE;

    const auto toNdc = [&](float x, float y, float& nx, float& ny) {
        x *= SIZE;
        y *= SIZE;

        if (m_pointer.mode == SPointer::EMode::Screen) {
            nx = m_pointer.ndcX + x * 2.0f / static_cast<float>(width);
            ny = m_pointer.ndcY - y * 2.0f / static_cast<float>(height);
            return true;
        }

        const Vec3 P = m_pointer.tip +
            m_pointer.right * (x * m_pointer.pxWorld) +
            m_pointer.down * (y * m_pointer.pxWorld);

        // Column-major vp * (P, 1).
        const auto& M = vp.m;
        const float CX = M[0] * P.x + M[4] * P.y + M[8] * P.z + M[12];
        const float CY = M[1] * P.x + M[5] * P.y + M[9] * P.z + M[13];
        const float CW = M[3] * P.x + M[7] * P.y + M[11] * P.z + M[15];

        if (CW <= 1e-4f)
            return false; // behind the camera

        nx = CX / CW;
        ny = CY / CW;
        return true;
    };

    const auto drawLayer = [&](float ox, float oy, float r, float g, float b) {
        std::vector<float> verts;
        verts.reserve(kPointerVerts * 5);

        for (const auto& V : ARROW) {
            float nx = 0.0f, ny = 0.0f;
            if (!toNdc(V[0] + ox, V[1] + oy, nx, ny))
                return;

            // Drawn straight into Hyprland's framebuffer, which is stored
            // vertically flipped (the scene blit flips v for the same
            // reason): mirror y or the arrow and its motion appear upside
            // down.
            verts.insert(verts.end(), {nx, -ny, 0.0f, 0.0f, 0.0f});
        }

        glUseProgram(m_sceneProgram);

        const Mat4 IDENTITY = Mat4::identity();
        glUniformMatrix4fv(m_sceneMVP, 1, GL_FALSE, IDENTITY.m.data());
        glUniform1i(m_sceneTextured, 0);
        glUniform4f(m_sceneColorUniform, r, g, b, 1.0f);
        glUniform4f(m_sceneUVRect, 0.0f, 0.0f, 1.0f, 1.0f);

        glBindBuffer(GL_ARRAY_BUFFER, m_pointerVBO);
        glBufferSubData(
            GL_ARRAY_BUFFER, 0,
            static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
            verts.data()
        );

        glBindVertexArray(m_pointerVAO);
        glDrawArrays(GL_TRIANGLES, 0, kPointerVerts);
        glBindVertexArray(0);
    };

    if (m_pointer.texture) {
        // Two triangles over the image, its hotspot on the cursor point.
        const float X0 = -m_pointer.hotX, Y0 = -m_pointer.hotY;
        const float X1 = m_pointer.texW - m_pointer.hotX;
        const float Y1 = m_pointer.texH - m_pointer.hotY;

        const float CORNERS[6][4] = {
            {X0, Y0, 0.0f, 0.0f}, {X1, Y0, 1.0f, 0.0f}, {X1, Y1, 1.0f, 1.0f},
            {X0, Y0, 0.0f, 0.0f}, {X1, Y1, 1.0f, 1.0f}, {X0, Y1, 0.0f, 1.0f},
        };

        std::vector<float> verts;
        verts.reserve(6 * 5);

        for (const auto& C : CORNERS) {
            float nx = 0.0f, ny = 0.0f;
            if (!toNdc(C[0], C[1], nx, ny))
                return;

            // Flipped framebuffer, as below.
            verts.insert(verts.end(), {nx, -ny, 0.0f, C[2], C[3]});
        }

        glUseProgram(m_sceneProgram);

        const Mat4 IDENTITY = Mat4::identity();
        glUniformMatrix4fv(m_sceneMVP, 1, GL_FALSE, IDENTITY.m.data());
        glUniform1i(m_sceneTextured, 1);
        glUniform1i(m_sceneTexture, 0);
        glUniform4f(m_sceneColorUniform, 1.0f, 1.0f, 1.0f, 1.0f);
        glUniform4f(m_sceneUVRect, 0.0f, 0.0f, 1.0f, 1.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_pointer.texture);

        // Hyprland sets filtering per draw of its own; left at the GL default
        // (a mipmap min filter, no mipmaps) the texture is incomplete and
        // samples as opaque black.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Wayland buffers carry premultiplied alpha.
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);

        glBindBuffer(GL_ARRAY_BUFFER, m_pointerVBO);
        glBufferSubData(
            GL_ARRAY_BUFFER, 0,
            static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
            verts.data()
        );

        glBindVertexArray(m_pointerVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glBindTexture(GL_TEXTURE_2D, 0);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
        return;
    }

    // A one-pixel black outline from eight offset copies, then the white
    // arrow: legible on any content, in any perspective.
    constexpr float D = 1.0f / ARROW_SIZE;
    constexpr float K = 0.7071f * D;
    const float OUTLINE[][2] = {
        {-D, 0.0f}, {D, 0.0f}, {0.0f, -D}, {0.0f, D},
        {-K, -K},   {K, -K},   {-K, K},    {K, K},
    };

    for (const auto& O : OUTLINE)
        drawLayer(O[0], O[1], 0.0f, 0.0f, 0.0f);

    drawLayer(0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
}

void GLScene::drawFullscreen(
    float alpha,
    bool perPixel
) {
    glUseProgram(m_blitProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneColor);

    glUniform1i(m_blitTexture, 0);
    glUniform1f(m_blitAlpha, alpha);
    glUniform1i(m_blitPerPx, perPixel ? 1 : 0);

    glBindVertexArray(m_fullscreenVAO);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

// The environment fade (the view morph): one NDC quad in the scene program's
// layout, drawn with (GL_ZERO, GL_SRC_COLOR) -- it scales the rgba already
// sitting in the scene buffer by `a`, turning the opaque room image into its
// premultiplied faded form in place. Color and alpha scale together, so the
// later premultiplied composite shows the desktop through at exactly (1 - a).
void GLScene::drawEnvFade(float a) {
    if (!m_fadeVAO) {
        glGenVertexArrays(1, &m_fadeVAO);
        glGenBuffers(1, &m_fadeVBO);

        const float verts[] = {
            // pos(3) + uv(2), NDC fullscreen triangle pair
            -1.f, -1.f, 0.f, 0.f, 0.f,
             1.f, -1.f, 0.f, 0.f, 0.f,
             1.f,  1.f, 0.f, 0.f, 0.f,
            -1.f, -1.f, 0.f, 0.f, 0.f,
             1.f,  1.f, 0.f, 0.f, 0.f,
            -1.f,  1.f, 0.f, 0.f, 0.f,
        };

        glBindVertexArray(m_fadeVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_fadeVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts,
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glUseProgram(m_sceneProgram);

    const Mat4 IDENTITY = Mat4::identity();
    glUniformMatrix4fv(m_sceneMVP, 1, GL_FALSE, IDENTITY.m.data());
    glUniform1i(m_sceneTextured, 0);
    glUniform1i(m_scenePremult, 0);
    glUniform4f(m_sceneColorUniform, a, a, a, a);
    glUniform4f(m_sceneUVRect, 0.0f, 0.0f, 1.0f, 1.0f);

    glBindVertexArray(m_fadeVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void GLScene::requestProbe() {
    m_probeRequested = true;
}

bool GLScene::probeValid() const {
    return m_probeValid;
}

const unsigned char* GLScene::probeRGBA() const {
    return m_probe;
}

    // F3 HUD text: a streaming quad per set font bit, screen-space ortho.
// font8x8 is public domain (daniel hepper / marcel sondaar).
void GLScene::drawDebugOverlay(int width, int height) {
    if (!m_debugOverlay)
        return;

    // The HUD must land in the scene texture or the composite never picks
    // it up. Save and restore whatever framebuffer was current.
    GLint oldFBO = 0, oldViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldFBO);
    glGetIntegerv(GL_VIEWPORT, oldViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
    glViewport(0, 0, width, height);

    // The lines: coordinates, view angles, fps, map size.
    const auto& CAM = m_camera;
    char line0[96], line1[96], line2[96], line3[96];
    snprintf(line0, sizeof(line0), "XYZ %.2f %.2f %.2f",
             CAM.position.x, CAM.position.y, CAM.position.z);
    snprintf(line1, sizeof(line1), "YAW %.1f  PIT %.1f",
             CAM.yaw * 180.0f / 3.14159265f, CAM.pitch * 180.0f / 3.14159265f);
    snprintf(line2, sizeof(line2), "FPS %.0f", m_debugFps);
    size_t tris = 0;
    size_t loaded = 0;
    for (const auto& S : m_slots)
        if (S.model && S.model->loaded()) {
            ++loaded;
            tris += S.model->triangles().size();
        }

    snprintf(line3, sizeof(line3), "MAP %zu/%zu objects %zu tris",
             loaded, m_slots.size(), tris);

    const char* LINES[4] = {line0, line1, line2, line3};

    constexpr float GLYPH = 8.0f;
    constexpr float SCALE = 2.0f;   // 16 px tall text
    constexpr float LINE  = GLYPH * SCALE + 4.0f;

    std::vector<float> verts;
    verts.reserve(64 * 1024);

    // pos(3) + uv(2) -- the SCENE program's layout, so the text rides the
    // same shader that already renders the floor and windows. A dedicated
    // mini-program rendered nothing on real GLES; the scene program is the
    // one path guaranteed to work.
    const auto QUAD = [&](float l, float t, float r, float b) {
        verts.insert(verts.end(), {
            l, t, 0, 0, 0,  r, t, 0, 0, 0,  l, b, 0, 0, 0,
            l, b, 0, 0, 0,  r, t, 0, 0, 0,  r, b, 0, 0, 0,
        });
    };

    const auto EMIT_TEXT = [&](float x, float y) {
        for (int li = 0; li < 4; ++li) {
            float cx = x;
            for (const char* P = LINES[li]; *P; ++P) {
                const auto ROWS = font8x8_basic[static_cast<unsigned char>(*P)];
                for (int row = 0; row < 8; ++row) {
                    const unsigned BITS = ROWS[row];
                    if (!BITS)
                        continue;
                    for (int col = 0; col < 8; ++col) {
                        if (!(BITS & (1u << col)))
                            continue;
                        QUAD(cx + col * SCALE, y + li * LINE + row * SCALE,
                             cx + col * SCALE + SCALE,
                             y + li * LINE + row * SCALE + SCALE);
                    }
                }
                cx += GLYPH * SCALE;
            }
        }
    };

    // Shadow text (offset +2,+2) then white -- readable without a bar.
    EMIT_TEXT(10.0f, 12.0f);
    const size_t SHADOW_VERTS = verts.size() / 5;
    EMIT_TEXT(8.0f, 10.0f);

    if (verts.empty())
        return;

    if (!m_textVAO) {
        glGenVertexArrays(1, &m_textVAO);
        glGenBuffers(1, &m_textVBO);
        glBindVertexArray(m_textVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_textVBO);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glBindVertexArray(m_textVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_textVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);

    // Pixel coords -> NDC, y down. The negative y scale mirrors winding,
    // so face culling stays off for this pass.
    const Mat4 ORTHO = Mat4::translation(Vec3{-1.f, 1.f, 0.f}) *
        Mat4::scale(Vec3{2.0f / width, -2.0f / height, 1.0f});

    glUseProgram(m_sceneProgram);
    glUniformMatrix4fv(m_sceneMVP, 1, GL_FALSE, ORTHO.m.data());
    glUniform4f(m_sceneUVRect, 0.f, 0.f, 1.f, 1.f);
    glUniform1i(m_sceneTextured, 0);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDisable(GL_CULL_FACE);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));

    glUniform4f(m_sceneColorUniform, 0.f, 0.f, 0.f, 0.9f);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLint>(SHADOW_VERTS));

    glUniform4f(m_sceneColorUniform, 1.f, 1.f, 1.f, 1.f);
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(SHADOW_VERTS),
                 static_cast<GLint>(verts.size() / 5 - SHADOW_VERTS));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFBO));
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
}
bool GLScene::render(
    unsigned int targetFBO,
    int width,
    int height,
    float alpha,
    float dt,
    const std::vector<WindowRender>& windows
) {
    if (width <= 0 || height <= 0)
        return false;

    if (!initialize())
        return false;

    if (!ensureSceneFramebuffer(width, height))
        return false;

    m_time += std::clamp(dt, 0.0f, 0.1f);

    GLint oldDrawFBO = 0;
    GLint oldReadFBO = 0;
    GLint oldProgram = 0;
    GLint oldVAO = 0;
    GLint oldArrayBuffer = 0;
    GLint oldActiveTexture = 0;
    GLint oldTexture0 = 0;
    GLint oldViewport[4] = {};

    GLint oldBlendSrcRGB = 0;
    GLint oldBlendDstRGB = 0;
    GLint oldBlendSrcAlpha = 0;
    GLint oldBlendDstAlpha = 0;
    GLint oldBlendEqRGB = 0;
    GLint oldBlendEqAlpha = 0;

    GLint oldDepthFunc = 0;
    GLfloat oldLineWidth = 1.0f;

    GLboolean oldBlend = GL_FALSE;
    GLboolean oldDepthTest = GL_FALSE;
    GLboolean oldScissor = GL_FALSE;
    GLboolean oldCull = GL_FALSE;
    GLboolean oldDepthMask = GL_TRUE;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFBO);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFBO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);

    glActiveTexture(GL_TEXTURE0);

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &oldBlendEqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &oldBlendEqAlpha);
    glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
    glGetFloatv(GL_LINE_WIDTH, &oldLineWidth);
    glGetBooleanv(GL_BLEND, &oldBlend);
    glGetBooleanv(GL_DEPTH_TEST, &oldDepthTest);
    glGetBooleanv(GL_SCISSOR_TEST, &oldScissor);
    glGetBooleanv(GL_CULL_FACE, &oldCull);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);

    // --- scene pass: rendered offscreen, then composited over the desktop ---
    //
    // The desktop underneath is left exactly as Hyprland drew it and simply
    // fades out via `alpha`. It is never copied or re-projected, so the
    // workspace never appears as an object inside the 3D world.
    //
    // MORPH (the 2D<->3D view morph, envAlpha < 1) switches the buffer to a
    // premultiplied per-pixel image: transparent clear, everything drawn so
    // far (panorama, map, floor, grid, player) multiplied by envAlpha in
    // place, window quads composited premultiplied on top at full alpha.
    // At envAlpha 0 the scene is windows only -- projected onto their 2D
    // rectangles they replace the desktop pixels 1:1, no fade at either
    // handoff; as envAlpha rises the room materializes behind the flying
    // windows.
    const bool MORPH = m_envAlpha < 0.999f;

    m_windowsPremultiplied = MORPH;

    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);

    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    if (MORPH)
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    else
        glClearColor(0.012f, 0.019f, 0.032f, 1.0f);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect =
        static_cast<float>(width) / static_cast<float>(height);

    const float ZFOV =
        2.0f * std::atan(std::tan(kFovDeg * PI / 360.0f) /
                         std::max(m_zoom, 0.01f));

    const Mat4 projection =
        Mat4::perspective(ZFOV, aspect, 0.05f, 200.0f);

    m_width  = width;
    m_height = height;

    const Mat4 vp = projection * m_camera.view();

    // Background panorama first, then the map (it replaces the flat floor
    // when loaded; depth rejects whatever is behind its geometry), then the
    // ground so windows behind it are depth-rejected.
    refreshPanorama();
    refreshScene();
    drawPanorama(aspect);

    for (auto& S : m_slots) {
        if (!S.model || !S.model->loaded())
            continue;

        S.model->draw(vp, m_camera.position);

        // Red x-ray wireframe of the collision triangles (debug).
        S.model->drawDebug(vp);
    }

    // The player's character (hidden in first person): animated inside
    // render -- the clock and the vertex upload need the EGL context.
    refreshPlayer();
    if (m_pDbgOn)
        drawPlayerDebugCapsule(vp);
    if (m_playerVisible && m_player.loaded()) {
        m_player.setPose(m_playerFeet, m_playerYaw, m_playerCfg.scale,
                         m_playerCfg.posOffset + m_playerCfg.centerOffset,
                         m_playerCfg.rotDeg);
        m_player.setFlat(m_playerCfg.flat);
        m_player.setEmissiveScale(m_playerCfg.emissiveScale);
        m_player.update(dt);
        m_player.draw(vp, m_camera.position);
    }

    if (m_gridVisible) {
        drawFloor(vp);

        glEnable(GL_BLEND);
        glBlendFuncSeparate(
            GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
            GL_ZERO, GL_ONE
        );
        glDepthMask(GL_FALSE);

        drawGrid(vp);
    }

    // MORPH: multiply everything drawn so far by the environment alpha.
    // (GL_ZERO, GL_SRC_COLOR) scales dst by the quad's rgba in place --
    // an opaque environment (alpha 1) becomes exactly (rgb * a, a), the
    // premultiplied representation of the room faded to `a`. Windows draw
    // after this and keep their own alpha, so they never dim.
    if (MORPH) {
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);

        drawEnvFade(m_envAlpha);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }

    // Windows submit back to front and write depth: crossing quads cut into
    // each other honestly, and translucency composites in order.
    drawWindows(vp, windows);

    m_windowsPremultiplied = false;

    // Everything after the windows (F3 HUD text) draws straight again.
    glUseProgram(m_sceneProgram);
    glUniform1i(m_scenePremult, 0);

    glDepthMask(GL_TRUE);

    // F3 HUD: always on top of the scene, never part of the 3D pass state.
    drawDebugOverlay(width, height);

    // Read back one pixel of the offscreen scene while it is still bound. A
    // floor point in the lower half of the screen, where ground and sky are
    // both opaque by construction: alpha below 255 here means the composite
    // cannot become fully opaque no matter what the fade is set to.
    if (m_probeRequested) {
        glReadPixels(
            width / 2,
            height / 4,
            1,
            1,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            m_probe
        );

        m_probeValid    = true;
        m_probeRequested = false;
    }

    // --- composite over whatever Hyprland already drew for this frame ---
    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);

    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);

    if (MORPH) {
        // The scene buffer holds a premultiplied image: composite it as
        // such so the desktop shows through exactly where the environment
        // faded out, and window pixels (alpha 1) replace it outright.
        glBlendFuncSeparate(
            GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA
        );
    } else {
        glBlendFuncSeparate(
            GL_SRC_ALPHA,
            GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA
        );
    }

    drawFullscreen(std::clamp(alpha, 0.0f, 1.0f), MORPH);
    drawCrosshair(width, height);
    drawPointer(vp, width, height);

    // --- restore compositor state ---
    glUseProgram(static_cast<GLuint>(oldProgram));
    glBindVertexArray(static_cast<GLuint>(oldVAO));

    glBindBuffer(
        GL_ARRAY_BUFFER,
        static_cast<GLuint>(oldArrayBuffer)
    );

    glActiveTexture(GL_TEXTURE0);

    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture0)
    );

    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    glBlendFuncSeparate(
        oldBlendSrcRGB,
        oldBlendDstRGB,
        oldBlendSrcAlpha,
        oldBlendDstAlpha
    );

    glBlendEquationSeparate(oldBlendEqRGB, oldBlendEqAlpha);
    glDepthFunc(oldDepthFunc);
    glDepthMask(oldDepthMask);
    glLineWidth(oldLineWidth);

    if (oldBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);

    if (oldDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);

    if (oldScissor)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    if (oldCull)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);

    glViewport(
        oldViewport[0],
        oldViewport[1],
        oldViewport[2],
        oldViewport[3]
    );

    glBindFramebuffer(
        GL_DRAW_FRAMEBUFFER,
        static_cast<GLuint>(oldDrawFBO)
    );

    glBindFramebuffer(
        GL_READ_FRAMEBUFFER,
        static_cast<GLuint>(oldReadFBO)
    );

    return true;
}

void GLScene::setPanoramaPath(const std::string& path) {
    m_panoramaPath = path;
}


void GLScene::mouseMove(
    float dx,
    float dy
) {
    m_camera.mouseDelta(dx, dy);
}

void GLScene::rotateView(
    float yawDelta,
    float pitchDelta
) {
    m_camera.applyLook(yawDelta, pitchDelta);
}

std::uintptr_t GLScene::pick(
    const std::vector<WindowRender>& windows
) const {
    std::vector<RayQuad> quads;
    quads.reserve(windows.size());

    for (const auto& window : windows) {
        RayQuad quad;
        quad.id     = window.id;
        quad.center = {window.x, window.y, window.z};
        quad.width  = window.width;
        quad.height = window.height;
        quad.yaw    = window.yaw;
        quad.pitch  = window.pitch;

        // Match the draw rules exactly: nothing invisible is aimable, so
        // focus can never land on a window you cannot see.
        quad.pickable =
            window.texture != 0 &&
            window.alpha > 0.0f &&
            window.width > 0.0f &&
            window.height > 0.0f;

        quads.emplace_back(quad);
    }

    const RayHit hit =
        rayPickQuads(m_camera.position, m_camera.centerRay(), quads);

    return hit.hit ? hit.id : 0;
}

void GLScene::reset() {
    m_camera = Camera{};
    m_camera.position = {0.0f, 1.5f, 11.0f};
    m_camera.yaw = 0.0f;
    m_camera.pitch = -0.20f;
    m_time = 0.0f;
}

void GLScene::shutdown() {
    for (auto& S : m_slots)
        if (S.model)
            S.model->destroy();
    m_slots.clear();
    destroyGLObjects();
}

void GLScene::destroyGLObjects() {
    if (m_textVBO) {
        glDeleteBuffers(1, &m_textVBO);
        m_textVBO = 0;
    }
    if (m_textVAO) {
        glDeleteVertexArrays(1, &m_textVAO);
        m_textVAO = 0;
    }
    if (m_crosshairVBO) {
        glDeleteBuffers(1, &m_crosshairVBO);
        m_crosshairVBO = 0;
    }

    if (m_crosshairVAO) {
        glDeleteVertexArrays(1, &m_crosshairVAO);
        m_crosshairVAO = 0;
    }

    if (m_pointerVBO) {
        glDeleteBuffers(1, &m_pointerVBO);
        m_pointerVBO = 0;
    }

    if (m_pointerVAO) {
        glDeleteVertexArrays(1, &m_pointerVAO);
        m_pointerVAO = 0;
    }

    if (m_fullscreenVBO) {
        glDeleteBuffers(1, &m_fullscreenVBO);
        m_fullscreenVBO = 0;
    }

    if (m_fullscreenVAO) {
        glDeleteVertexArrays(1, &m_fullscreenVAO);
        m_fullscreenVAO = 0;
    }

    if (m_fadeVBO) {
        glDeleteBuffers(1, &m_fadeVBO);
        m_fadeVBO = 0;
    }

    if (m_fadeVAO) {
        glDeleteVertexArrays(1, &m_fadeVAO);
        m_fadeVAO = 0;
    }

    if (m_gridVBO) {
        glDeleteBuffers(1, &m_gridVBO);
        m_gridVBO = 0;
    }

    if (m_gridVAO) {
        glDeleteVertexArrays(1, &m_gridVAO);
        m_gridVAO = 0;
    }

    if (m_floorVBO) {
        glDeleteBuffers(1, &m_floorVBO);
        m_floorVBO = 0;
    }

    if (m_floorVAO) {
        glDeleteVertexArrays(1, &m_floorVAO);
        m_floorVAO = 0;
    }

    if (m_quadVBO) {
        glDeleteBuffers(1, &m_quadVBO);
        m_quadVBO = 0;
    }

    if (m_quadVAO) {
        glDeleteVertexArrays(1, &m_quadVAO);
        m_quadVAO = 0;
    }

    if (m_sceneDepth) {
        glDeleteRenderbuffers(1, &m_sceneDepth);
        m_sceneDepth = 0;
    }

    if (m_sceneColor) {
        glDeleteTextures(1, &m_sceneColor);
        m_sceneColor = 0;
    }

    if (m_sceneFBO) {
        glDeleteFramebuffers(1, &m_sceneFBO);
        m_sceneFBO = 0;
    }

    if (m_sceneProgram) {
        glDeleteProgram(m_sceneProgram);
        m_sceneProgram = 0;
    }

    if (m_blitProgram) {
        glDeleteProgram(m_blitProgram);
        m_blitProgram = 0;
    }

    if (m_panoramaProgram) {
        glDeleteProgram(m_panoramaProgram);
        m_panoramaProgram = 0;
    }



    if (m_panoramaTex) {
        glDeleteTextures(1, &m_panoramaTex);
        m_panoramaTex = 0;
    }

    m_panoramaLoaded.clear();
    m_panoramaMtimeValid = false;

    m_initialized = false;

    m_sceneWidth = 0;
    m_sceneHeight = 0;
}

} // namespace H3D

#include "HyprlandCompat/WindowCapture.hpp"

#include "HyprlandCompat/WindowsCompat.hpp"

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/types/Buffer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>

#include <GLES3/gl32.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace H3D::Compat {

namespace {

// Raw identity of a surface's last-committed buffer. Taking the pointer does
// NOT keep the buffer alive (no reference is held), so the client's buffer
// lifecycle is untouched.
std::uintptr_t bufferIdentity(const WP<CWLSurfaceResource>& surface) {
    if (!surface || !surface->m_current.buffer.m_buffer)
        return 0;

    return reinterpret_cast<std::uintptr_t>(surface->m_current.buffer.m_buffer.get());
}

bool boxEquals(const CBox& a, const CBox& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void destroyTexture(unsigned int& tex) {
    if (!tex)
        return;

    glDeleteTextures(1, &tex);
    tex = 0;
}

GLuint createBlankTexture(int width, int height) {
    GLuint tex = 0;
    glGenTextures(1, &tex);

    // Zero-filled, not nullptr: glTexImage2D with null data leaves the
    // contents UNDEFINED, and any tile that fails to capture would then show
    // garbage instead of a clean black patch until the next frame retries.
    std::vector<unsigned char> blank(static_cast<size_t>(width) * height * 4, 0);

    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
        blank.data());

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// --- GPU alpha copy ---------------------------------------------------------
//
// The snapshot framebuffer's alpha channel cannot be read back directly:
// GPU sampling sees the real content opacity (windows render opaque with
// transparent rounded corners), while glReadPixels on the snapshot FB
// returns garbage in A -- the FB is backed by storage whose alpha bytes the
// CPU-side read path does not see. The reliable path is therefore a GPU-side
// copy: draw the snapshot texture into a small OWNED RGBA8 FBO with a plain
// textured shader, then read that FBO back -- a standard path, the same one
// GLScene's probe uses.

struct SCopyGL {
    GLuint prog = 0, vao = 0, vbo = 0;
    int    uUVRect = -1, uTex = -1;
};

SCopyGL& copyGL() {
    static SCopyGL C;
    return C;
}

GLuint compileShader(GLenum type, const char* src) {
    GLuint S = glCreateShader(type);
    glShaderSource(S, 1, &src, nullptr);
    glCompileShader(S);

    GLint ok = GL_FALSE;
    glGetShaderiv(S, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        glDeleteShader(S);
        return 0;
    }

    return S;
}

bool ensureCopyGL() {
    auto& C = copyGL();

    if (C.prog)
        return true;

    static constexpr const char* VS = R"GLSL(
#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform vec4 uUVRect; // xy = subrect origin, zw = subrect size
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV * uUVRect.zw + uUVRect.xy;
}
)GLSL";

    static constexpr const char* FS = R"GLSL(
#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vUV);
}
)GLSL";

    const GLuint VSx = compileShader(GL_VERTEX_SHADER, VS);
    const GLuint FSx = compileShader(GL_FRAGMENT_SHADER, FS);

    if (!VSx || !FSx) {
        if (VSx) glDeleteShader(VSx);
        if (FSx) glDeleteShader(FSx);
        return false;
    }

    C.prog = glCreateProgram();
    glAttachShader(C.prog, VSx);
    glAttachShader(C.prog, FSx);
    glLinkProgram(C.prog);
    glDeleteShader(VSx);
    glDeleteShader(FSx);

    GLint ok = GL_FALSE;
    glGetProgramiv(C.prog, GL_LINK_STATUS, &ok);

    if (!ok) {
        glDeleteProgram(C.prog);
        C.prog = 0;
        return false;
    }

    C.uUVRect = glGetUniformLocation(C.prog, "uUVRect");
    C.uTex    = glGetUniformLocation(C.prog, "uTex");

    // Fullscreen quad, UV (0,0) at the GL bottom-left: row 0 of the readback
    // ends up being the subrect's v0 row.
    static const float QUAD[] = {
        -1.f, -1.f, 0.f, 0.f,   1.f, -1.f, 1.f, 0.f,   1.f, 1.f, 1.f, 1.f,
        -1.f, -1.f, 0.f, 0.f,   1.f,  1.f, 1.f, 1.f,  -1.f, 1.f, 0.f, 1.f,
    };

    glGenVertexArrays(1, &C.vao);
    glGenBuffers(1, &C.vbo);

    glBindVertexArray(C.vao);
    glBindBuffer(GL_ARRAY_BUFFER, C.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void shutdownCopyGL() {
    auto& C = copyGL();

    if (C.prog) glDeleteProgram(C.prog);
    if (C.vao)  glDeleteVertexArrays(1, &C.vao);
    if (C.vbo)  glDeleteBuffers(1, &C.vbo);

    C = {};
}

// Draws `srcTex`'s UV subrect into a small owned FBO and returns the alpha
// bytes. Returns 0 on success, else a diagnostic code.
int copyTextureAlpha(GLuint srcTex, float u0, float v0, float u1, float v1,
                     int mw, int mh, std::vector<unsigned char>& alphaOut) {
    alphaOut.clear();

    if (!srcTex || mw <= 0 || mh <= 0)
        return 3;

    if (!ensureCopyGL())
        return 2;

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glGenFramebuffers(1, &fbo);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mw, mh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);

    GLint oldDraw = 0, oldRead = 0, oldActive = 0, oldTex = 0;
    GLint oldProg = 0, oldVAO = 0;
    GLint oldViewport[4] = {};
    GLboolean oldScissor = GL_FALSE, oldBlend = GL_FALSE,
              oldDepth = GL_FALSE;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    oldBlend   = glIsEnabled(GL_BLEND);
    oldDepth   = glIsEnabled(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    int err = 0;

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        err = 2;
    } else {
        glViewport(0, 0, mw, mh);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);

        glUseProgram(copyGL().prog);
        glUniform1i(copyGL().uTex, 0);
        glUniform4f(copyGL().uUVRect, u0, v0, u1 - u0, v1 - v0);

        glBindVertexArray(copyGL().vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        err = glGetError() == GL_NO_ERROR ? 0 : 4;

        if (err == 0) {
            std::vector<unsigned char> rgba(static_cast<size_t>(mw) * mh * 4);
            glReadPixels(0, 0, mw, mh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

            // Our own FBO: row 0 of the readback is the subrect's v0 row --
            // for top-down snapshot textures v0 is the box's top row, the
            // exact convention the outline tracer works with.
            alphaOut.resize(static_cast<size_t>(mw) * mh);
            for (size_t i = 0; i < alphaOut.size(); ++i)
                alphaOut[i] = rgba[i * 4 + 3];
        }
    }

    glActiveTexture(oldActive);
    glBindTexture(GL_TEXTURE_2D, oldTex);
    glUseProgram(oldProg);
    glBindVertexArray(oldVAO);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2],
               oldViewport[3]);
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (oldBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (oldDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDraw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, oldRead);

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return err;
}

// Trace + store the outline loops once the mask bytes are in. `insetTexels`
// is the interior UV inset in MASK texels -- the caller computes it in
// SOURCE pixels and scales by the mask resolution, so the sampling depth of
// the walls is independent of how much the mask was downscaled. The outline
// is owned by the per-window state (it must outlive this snapshot object)
// and shared into it.
void finishSkirtMask(CWindowCapture::SSkirtState& state,
                     CWindowCapture::SSnapshot& snapshot,
                     std::vector<unsigned char>&& alpha, int mw, int mh,
                     float insetTexels, int boxW, int boxH) {
    state.w    = mw;
    state.h    = mh;
    state.boxW = boxW;
    state.boxH = boxH;
    state.valid = true;

    state.outlines =
        std::make_shared<const std::vector<H3D::SOutlineLoop>>(
            H3D::traceOutlines(alpha.data(), mw, mh, 64, insetTexels, 4, 128));

    snapshot.outlines  = state.outlines;
    snapshot.skirtW    = mw;
    snapshot.skirtH    = mh;
    snapshot.skirtValid = true;
    snapshot.skirtError = 0;

    // One-shot diagnosis dump (first successful refresh per plugin load):
    // the alpha mask the wall silhouette was traced from.
    static bool dumped = false;

    if (!dumped) {
        dumped = true;

        std::ofstream M("/tmp/hypr3d-skirt-mask.pgm", std::ios::binary);
        if (M) {
            M << "P5\n" << mw << " " << mh << "\n255\n";
            M.write(reinterpret_cast<const char*>(alpha.data()),
                    static_cast<std::streamsize>(alpha.size()));
        }
    }
}

} // namespace

bool CWindowCapture::makeSnapshot(const PHLWINDOW& window, const PHLMONITOR& monitor, bool force) {
    if (!window || !monitor)
        return false;

    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    CBox     fullBox{};
    Vector2D surfOffset{}, surfSize{};
    if (!decoratedSurfaceBox(window, monitor, fullBox, surfOffset, surfSize))
        return false;

    const Vector2D MONLOGI = monitor->m_size;

    // Unchanged content and geometry: keep the previous snapshot. The nested
    // snapshot pass is by far the heaviest per-frame cost (a full fake render
    // plus a monitor-sized framebuffer allocation), and skipping it for idle
    // windows is what keeps the room cheap while a screen capture is running.
    const auto ID = reinterpret_cast<std::uintptr_t>(window.get());

    if (!force) {
        if (auto IT = m_snapshots.find(ID); IT != m_snapshots.end() &&
            IT->second.lastBuffer == bufferIdentity(window->resource()) &&
            boxEquals(IT->second.fullBox, fullBox))
            return true;
    }

    // makeSnapshotFB renders into a monitor-sized framebuffer and clips
    // everything outside the viewport. A window sticking past the screen edge
    // is pulled fully into view by warping the workspace render offset for
    // the duration of its pass. A window LARGER than the monitor can never
    // fit at all -- stretching the captured slice over the full quad
    // distorted the content -- so it is captured in monitor-sized tiles and
    // composited into one window-sized texture at native scale, which keeps
    // the true aspect ratio at any size. The 2D desktop underneath stays
    // hidden behind the 3D scene the whole time.

    if (fullBox.w > MONLOGI.x || fullBox.h > MONLOGI.y)
        return makeTiledSnapshot(window, monitor, fullBox, surfOffset, surfSize, ID, force);

    Vector2D shift;
    const auto WORKSPACE = window->m_workspace;

    if (WORKSPACE) {
        const Vector2D MONPOS = monitor->m_position;
        const double   GX     = MONPOS.x + fullBox.x;
        const double   GY     = MONPOS.y + fullBox.y;

        const double LEFT   = MONPOS.x - GX;
        const double RIGHT  = (GX + fullBox.w) - (MONPOS.x + MONLOGI.x);
        const double TOP    = MONPOS.y - GY;
        const double BOTTOM = (GY + fullBox.h) - (MONPOS.y + MONLOGI.y);

        if (LEFT > 0.0)
            shift.x = LEFT;
        else if (RIGHT > 0.0)
            shift.x = -RIGHT;

        if (TOP > 0.0)
            shift.y = TOP;
        else if (BOTTOM > 0.0)
            shift.y = -BOTTOM;
    }

    const bool     SHIFTED  = shift.x != 0.0 || shift.y != 0.0;
    const Vector2D ORIGINAL = WORKSPACE ? WORKSPACE->m_renderOffset->value() : Vector2D{};

    if (SHIFTED)
        WORKSPACE->m_renderOffset->setValueAndWarp(shift);

    // Renders the window (content + decorations) into a fresh plugin-ownable
    // framebuffer. Requires the window to currently pass shouldRenderWindow.
    auto fb = g_pHyprRenderer->makeSnapshotFB(window);

    if (SHIFTED)
        WORKSPACE->m_renderOffset->setValueAndWarp(ORIGINAL);

    if (!fb)
        return false;

    const auto TEXTURE = fb->getTexture();

    SSnapshot snapshot;
    snapshot.fb     = fb;
    snapshot.texID  = TEXTURE ? TEXTURE->m_texID : 0;
    snapshot.width  = TEXTURE ? static_cast<int>(TEXTURE->m_size.x) : 0;
    snapshot.height = TEXTURE ? static_cast<int>(TEXTURE->m_size.y) : 0;

    if (snapshot.texID == 0 || snapshot.width <= 0 || snapshot.height <= 0) {
        snapshot.fb = nullptr;
        return false;
    }

    // Record the geometry the snapshot was rendered at. The live box may be
    // written later in the same frame (the resize path runs after render.pre),
    // and the UV subrect must stay aligned with the captured pixels, not with
    // the live geometry.
    snapshot.fullBox = fullBox;
    snapshot.sampledBox = CBox{
        fullBox.x + static_cast<int>(shift.x),
        fullBox.y + static_cast<int>(shift.y),
        fullBox.w,
        fullBox.h,
    };
    snapshot.surfaceOffset = surfOffset;
    snapshot.surfaceSize   = surfSize;
    snapshot.texSpan       = MONLOGI;
    snapshot.lastBuffer    = bufferIdentity(window->resource());

    // Drop the previous snapshot first: the SP releases the framebuffer's GL
    // objects on destruction, and a leftover composite texture from a tiled
    // snapshot has to be deleted explicitly.
    if (auto IT = m_snapshots.find(ID); IT != m_snapshots.end()) {
        destroySnapshotGL(IT->second);
        m_snapshots.erase(IT);
    }
    m_snapshots.emplace(ID, std::move(snapshot));

    refreshSkirtMask(ID, m_snapshots.find(ID)->second, monitor);

    return true;
}

// A window larger than the monitor cannot fit into a single monitor-sized
// snapshot. Capture it as a grid of monitor-sized tiles -- each pass has the
// workspace render offset warped so its tile starts at the viewport origin --
// and copy every tile into one window-sized texture. The composite keeps the
// window's true aspect ratio at native pixel density: no distortion at all.
bool CWindowCapture::makeTiledSnapshot(
    const PHLWINDOW&  window,
    const PHLMONITOR& monitor,
    const CBox&       fullBox,
    const Vector2D&   surfOffset,
    const Vector2D&   surfSize,
    std::uintptr_t    id,
    bool              force
) {
    const Vector2D MONLOGI = monitor->m_size;
    const Vector2D MONPIX  = monitor->m_pixelSize;

    const double SX = MONPIX.x / MONLOGI.x; // logical -> texture px
    const double SY = MONPIX.y / MONLOGI.y;

    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);

    const int TEXW = std::min<GLint>(maxTex, static_cast<GLint>(std::lround(fullBox.w * SX)));
    const int TEXH = std::min<GLint>(maxTex, static_cast<GLint>(std::lround(fullBox.h * SY)));

    if (TEXW <= 0 || TEXH <= 0)
        return false;

    const auto IT = m_snapshots.find(id);

    // Unchanged content and geometry: keep the previous composite. The tile
    // grid is the most expensive capture path there is.
    if (!force && IT != m_snapshots.end() &&
        IT->second.lastBuffer == bufferIdentity(window->resource()) &&
        boxEquals(IT->second.fullBox, fullBox))
        return true;

    // Reuse the composite texture while the box size is unchanged.
    GLuint       bigTex = 0;
    if (IT != m_snapshots.end() && IT->second.bigTex &&
        static_cast<int>(IT->second.texSpan.x) == static_cast<int>(fullBox.w) &&
        static_cast<int>(IT->second.texSpan.y) == static_cast<int>(fullBox.h)) {
        bigTex = IT->second.bigTex;
    } else {
        if (IT != m_snapshots.end() && IT->second.bigTex)
            destroyTexture(IT->second.bigTex);

        bigTex = createBlankTexture(TEXW, TEXH);

        if (!bigTex)
            return false;
    }

    const auto WORKSPACE = window->m_workspace;
    const Vector2D ORIGINAL =
        WORKSPACE ? WORKSPACE->m_renderOffset->value() : Vector2D{};

    const int COLS = static_cast<int>(std::ceil(fullBox.w / MONLOGI.x));
    const int ROWS = static_cast<int>(std::ceil(fullBox.h / MONLOGI.y));

    bool anyTile = false;

    for (int j = 0; j < ROWS; ++j) {
        for (int i = 0; i < COLS; ++i) {
            const float TILE_X = i * MONLOGI.x;
            const float TILE_Y = j * MONLOGI.y;
            const float TILE_W = std::min<float>(MONLOGI.x, fullBox.w - TILE_X);
            const float TILE_H = std::min<float>(MONLOGI.y, fullBox.h - TILE_Y);

            // Warp the workspace offset so THIS tile's top-left corner sits
            // at the viewport origin; the rest of the window clips away.
            const Vector2D shift{
                -fullBox.x - TILE_X,
                -fullBox.y - TILE_Y,
            };

            if (WORKSPACE)
                WORKSPACE->m_renderOffset->setValueAndWarp(shift);

            auto fb = g_pHyprRenderer->makeSnapshotFB(window);

            if (WORKSPACE)
                WORKSPACE->m_renderOffset->setValueAndWarp(ORIGINAL);

            if (!fb)
                continue; // hole in the composite; next frame retries

            auto* GLFB = dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get());

            if (!GLFB)
                continue;

            const int SRCW = static_cast<int>(std::lround(TILE_W * SX));
            const int SRCH = static_cast<int>(std::lround(TILE_H * SY));

            // FB row 0 holds the tile's logical top row; copying it to dest
            // row TILE_Y*SY keeps the composite's row 0 at the window's
            // logical top -- the same top-down convention the monitor-sized
            // snapshots are sampled with.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, GLFB->getFBID());
            glBindTexture(GL_TEXTURE_2D, bigTex);
            glCopyTexSubImage2D(
                GL_TEXTURE_2D, 0,
                static_cast<int>(std::lround(TILE_X * SX)),
                static_cast<int>(std::lround(TILE_Y * SY)),
                0, 0, SRCW, SRCH);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

            anyTile = true;
        }
    }

    if (!anyTile)
        return false;

    // Drop a monitor-sized fb left over from a previous non-tiled snapshot;
    // the composite texture replaces it wholesale.
    SSnapshot snapshot;
    snapshot.fb     = nullptr;
    snapshot.texID  = 0;
    snapshot.bigTex = bigTex;
    snapshot.width  = TEXW;
    snapshot.height = TEXH;

    snapshot.fullBox       = fullBox;
    snapshot.sampledBox    = CBox{0, 0, fullBox.w, fullBox.h};
    snapshot.surfaceOffset = surfOffset;
    snapshot.surfaceSize   = surfSize;
    snapshot.texSpan       = Vector2D{fullBox.w, fullBox.h};
    snapshot.lastBuffer    = bufferIdentity(window->resource());

    m_snapshots.erase(id);
    m_snapshots.emplace(id, std::move(snapshot));

    refreshSkirtMaskTiled(id, m_snapshots.find(id)->second);

    return true;
}

// Depth-slab silhouette for windows: the snapshot texture's box region is
// drawn into a small owned FBO (the GPU samples the snapshot's real alpha --
// rounded corners included) and the outline is traced from that mask.
// GL y grows with logical y on Hyprland textures, so readback row 0 is the
// box's top row -- the convention the outline tracer works with.
//
// The cadence state and the traced outline live in m_skirtStates, keyed by
// window id: the snapshot object is recreated on every content update, and
// a state stored in it would reset the cadence to "now". Refresh policy:
// first trace at full mask resolution; during a drag-resize (box changes
// every frame) throttled to ~6.7/s at a reduced cap; the full-resolution
// trace runs once the box settles -- stopped changing while the outline is
// still stale.
void CWindowCapture::refreshSkirtMask(std::uintptr_t id, SSnapshot& snapshot,
                                      const PHLMONITOR& monitor) {
    if (!monitor)
        return;

    // View morph / fullscreen transition: boxes are being driven every
    // frame, a trace now would be stale on arrival. The cadence re-traces
    // once the flag clears and the boxes settle.
    if (m_skirtDeferred)
        return;

    auto& ST = m_skirtStates[id];

    const int CBW = static_cast<int>(snapshot.sampledBox.w);
    const int CBH = static_cast<int>(snapshot.sampledBox.h);

    const bool STALE =
        ST.valid && (ST.boxW != CBW || ST.boxH != CBH);
    const bool SETTLED =
        STALE && ST.lastBoxW == CBW && ST.lastBoxH == CBH;

    ST.lastBoxW = CBW;
    ST.lastBoxH = CBH;
    ++ST.age;

    const auto NOW = std::chrono::steady_clock::now();
    const bool THROTTLE_OK =
        !ST.hasTime || NOW - ST.lastRefresh > std::chrono::milliseconds(150);

    const bool DUE = !ST.valid || STALE || ST.age % 64 == 1;

    if (DUE && (SETTLED || THROTTLE_OK)) {
        const GLuint SRC = snapshot.texID;

        if (!SRC) {
            snapshot.skirtError = 1;
            return;
        }

        const double SX = monitor->m_pixelSize.x / monitor->m_size.x;
        const double SY = monitor->m_pixelSize.y / monitor->m_size.y;

        const int PW = std::max(1, static_cast<int>(std::lround(
                                      snapshot.sampledBox.w * SX)));
        const int PH = std::max(1, static_cast<int>(std::lround(
                                      snapshot.sampledBox.h * SY)));

        // UV subrect of the box inside the monitor-sized snapshot, clamped
        // to the texture. v0 is the box's TOP row (top-down texture).
        const float U0 = std::clamp(
            static_cast<float>(snapshot.sampledBox.x * SX / snapshot.width),
            0.f, 1.f);
        const float V0 = std::clamp(
            static_cast<float>(snapshot.sampledBox.y * SY / snapshot.height),
            0.f, 1.f);
        const float U1 = std::clamp(
            static_cast<float>((snapshot.sampledBox.x + snapshot.sampledBox.w) *
                               SX / snapshot.width),
            0.f, 1.f);
        const float V1 = std::clamp(
            static_cast<float>((snapshot.sampledBox.y + snapshot.sampledBox.h) *
                               SY / snapshot.height),
            0.f, 1.f);

        // The mask texel size IS the staircase step on the walls: full
        // resolution when settled (or on the first trace), reduced mid-drag
        // -- the silhouette is transient there anyway.
        const float CAP = (!ST.valid || SETTLED) ? 1536.0f : 512.0f;

        const float SCALE = std::min({1.0f, CAP / PW, CAP / PH});
        const int MW = std::max(1, static_cast<int>(std::lround(PW * SCALE)));
        const int MH = std::max(1, static_cast<int>(std::lround(PH * SCALE)));

        // Wall sampling depth, in SOURCE pixels: half the border band, so
        // the walls paint the middle of the border -- not the antialiased
        // fringe, not the content behind it. surfaceOffset.x is exactly
        // that band's width for server-side decorations; clamped for CSD
        // windows whose offset is the shadow margin instead.
        const float INSET_PX = std::clamp(
            static_cast<float>(snapshot.surfaceOffset.x) * 0.5f, 1.0f, 3.0f);

        std::vector<unsigned char> alpha;

        const int ERR =
            copyTextureAlpha(SRC, U0, V0, U1, V1, MW, MH, alpha);

        if (ERR != 0) {
            snapshot.skirtError = ERR;
            return;
        }

        finishSkirtMask(ST, snapshot, std::move(alpha), MW, MH,
                        INSET_PX * static_cast<float>(MW) / PW, CBW, CBH);

        ST.lastRefresh = NOW;
        ST.hasTime     = true;
    } else {
        // Gated out: keep sharing the surviving outline.
        snapshot.outlines   = ST.outlines;
        snapshot.skirtW     = ST.w;
        snapshot.skirtH     = ST.h;
        snapshot.skirtValid = ST.valid;
        snapshot.skirtError = 0;
    }
}

// Tiled composite: bigTex spans exactly the window box, so the copy subrect
// is the full texture. Same throttle/settle policy as the monitor-sized
// path (a tiled window resized below the monitor size re-routes through
// makeSnapshot's non-tiled path, so stale tiled outlines self-heal).
void CWindowCapture::refreshSkirtMaskTiled(std::uintptr_t id, SSnapshot& snapshot) {
    if (m_skirtDeferred)
        return; // view morph / FS transition: postpone, see refreshSkirtMask

    auto& ST = m_skirtStates[id];
    ++ST.age;

    const auto NOW = std::chrono::steady_clock::now();
    const bool THROTTLE_OK =
        !ST.hasTime || NOW - ST.lastRefresh > std::chrono::milliseconds(150);

    if (!ST.valid || ST.age % 64 == 1) {
        const GLuint SRC = snapshot.bigTex;

        if (!SRC || snapshot.width <= 0 || snapshot.height <= 0) {
            snapshot.skirtError = 1;
            return;
        }

        const float SCALE = std::min(
            {1.0f, 1536.0f / snapshot.width, 1536.0f / snapshot.height});
        const int MW =
            std::max(1, static_cast<int>(std::lround(snapshot.width * SCALE)));
        const int MH =
            std::max(1, static_cast<int>(std::lround(snapshot.height * SCALE)));

        const float INSET_PX = std::clamp(
            static_cast<float>(snapshot.surfaceOffset.x) * 0.5f, 1.0f, 3.0f);

        std::vector<unsigned char> alpha;

        const int ERR =
            copyTextureAlpha(SRC, 0.f, 0.f, 1.f, 1.f, MW, MH, alpha);

        if (ERR != 0) {
            snapshot.skirtError = ERR;
            return;
        }

        finishSkirtMask(ST, snapshot, std::move(alpha), MW, MH,
                        INSET_PX * static_cast<float>(MW) / snapshot.width,
                        static_cast<int>(snapshot.texSpan.x),
                        static_cast<int>(snapshot.texSpan.y));

        ST.lastRefresh = NOW;
        ST.hasTime     = true;
    } else {
        snapshot.outlines   = ST.outlines;
        snapshot.skirtW     = ST.w;
        snapshot.skirtH     = ST.h;
        snapshot.skirtValid = ST.valid;
        snapshot.skirtError = 0;
    }
}

// Layer-shell surfaces render at their monitor-local box with no
// decorations: the snapshot is monitor-sized with the layer already in
// place, so the UV math matches the window path with a zero surface offset.
// No off-screen pull here -- layers are anchored to the monitor by the shell.
bool CWindowCapture::makeSnapshotPopup(const WP<Desktop::View::CPopup>& popup,
                                       const CBox& box, const PHLMONITOR& monitor,
                                       bool force) {
    const auto POPUP = popup.lock();
    if (!POPUP || !monitor || box.w <= 0 || box.h <= 0)
        return false;

    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    const auto ID = reinterpret_cast<std::uintptr_t>(POPUP.get());

    if (!force) {
        if (auto IT = m_snapshots.find(ID); IT != m_snapshots.end() &&
            IT->second.lastBuffer == bufferIdentity(POPUP->resource()) &&
            boxEquals(IT->second.fullBox, box))
            return true;
    }

    // Like a layer: rendered at its own position into a monitor-sized
    // framebuffer, sampled at its box.
    auto fb = g_pHyprRenderer->makeSnapshotFB(popup);
    if (!fb)
        return false;

    const auto TEXTURE = fb->getTexture();

    SSnapshot snapshot;
    snapshot.fb     = fb;
    snapshot.texID  = TEXTURE ? TEXTURE->m_texID : 0;
    snapshot.width  = TEXTURE ? static_cast<int>(TEXTURE->m_size.x) : 0;
    snapshot.height = TEXTURE ? static_cast<int>(TEXTURE->m_size.y) : 0;

    if (snapshot.texID == 0 || snapshot.width <= 0 || snapshot.height <= 0)
        return false;

    snapshot.fullBox       = box;
    snapshot.sampledBox    = box;
    snapshot.surfaceOffset = Vector2D{0, 0};
    snapshot.surfaceSize   = Vector2D{box.w, box.h};
    snapshot.texSpan       = monitor->m_size;
    snapshot.lastBuffer    = bufferIdentity(POPUP->resource());

    m_snapshots.erase(ID);
    m_snapshots.emplace(ID, std::move(snapshot));
    return true;
}

bool CWindowCapture::makeSnapshotLayer(const PHLLS& layer, const PHLMONITOR& monitor, bool force) {
    if (!layer || !monitor)
        return false;

    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    const auto BOXOPT = layer->surfaceLogicalBox();

    if (!BOXOPT || BOXOPT->w <= 0 || BOXOPT->h <= 0)
        return false;

    const auto ID = reinterpret_cast<std::uintptr_t>(layer.get());

    const auto SURFACE =
        layer->m_layerSurface ? layer->m_layerSurface->m_surface.lock() : nullptr;

    if (!force) {
        if (auto IT = m_snapshots.find(ID); IT != m_snapshots.end() &&
            IT->second.lastBuffer == bufferIdentity(SURFACE) &&
            boxEquals(IT->second.fullBox, *BOXOPT))
            return true;
    }

    auto fb = g_pHyprRenderer->makeSnapshotFB(layer);

    if (!fb)
        return false;

    const auto TEXTURE = fb->getTexture();

    SSnapshot snapshot;
    snapshot.fb     = fb;
    snapshot.texID  = TEXTURE ? TEXTURE->m_texID : 0;
    snapshot.width  = TEXTURE ? static_cast<int>(TEXTURE->m_size.x) : 0;
    snapshot.height = TEXTURE ? static_cast<int>(TEXTURE->m_size.y) : 0;

    if (snapshot.texID == 0 || snapshot.width <= 0 || snapshot.height <= 0) {
        snapshot.fb = nullptr;
        return false;
    }

    snapshot.fullBox       = *BOXOPT;
    snapshot.sampledBox    = *BOXOPT;
    snapshot.surfaceOffset = Vector2D{0, 0};
    snapshot.surfaceSize   = Vector2D{BOXOPT->w, BOXOPT->h};
    snapshot.texSpan       = monitor->m_size;

    // Alpha mask for picking, refreshed every 32nd snapshot (or when the box
    // size changed): a downsampled readback of the layer's box region.
    // Invisible overlays then let the crosshair fall through their empty
    // pixels instead of swallowing input for the window underneath.
    ++snapshot.maskAge;

    const double SX = monitor->m_pixelSize.x / monitor->m_size.x;
    const double SY = monitor->m_pixelSize.y / monitor->m_size.y;

    const int PW = static_cast<int>(std::lround(BOXOPT->w * SX));
    const int PH = static_cast<int>(std::lround(BOXOPT->h * SY));

    const bool SIZE_CHANGED =
        snapshot.alphaValid &&
        (snapshot.alphaW * 4 != PW || snapshot.alphaH * 4 != PH);

    if (!snapshot.alphaValid || SIZE_CHANGED || snapshot.maskAge % 32 == 1) {
        auto* GLFB = dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get());

        // The layer sits at its box position inside the monitor-sized
        // snapshot; read exactly that region, clamped to the framebuffer.
        const int PX = std::clamp(static_cast<int>(std::lround(BOXOPT->x * SX)), 0,
            std::max(0, snapshot.width - 1));
        const int PY = std::clamp(static_cast<int>(std::lround(BOXOPT->y * SY)), 0,
            std::max(0, snapshot.height - 1));
        const int RW = std::min(PW, snapshot.width - PX);
        const int RH = std::min(PH, snapshot.height - PY);

        if (GLFB && RW > 0 && RH > 0) {
            std::vector<unsigned char> rgba(static_cast<size_t>(RW) * RH * 4);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, GLFB->getFBID());
            glReadPixels(PX, PY, RW, RH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

            const int MW = std::max(1, RW / 4);
            const int MH = std::max(1, RH / 4);

            std::vector<unsigned char> mask(static_cast<size_t>(MW) * MH, 255);

            for (int j = 0; j < MH; ++j) {
                const int SRCY = std::min(RH - 1, j * 4);

                for (int i = 0; i < MW; ++i) {
                    const int SRCX = std::min(RW - 1, i * 4);
                    mask[static_cast<size_t>(j) * MW + i] =
                        rgba[(static_cast<size_t>(SRCY) * RW + SRCX) * 4 + 3];
                }
            }

            snapshot.alphaMask  = std::move(mask);
            snapshot.alphaW     = MW;
            snapshot.alphaH     = MH;
            snapshot.alphaValid = true;

            // The same mask doubles as the depth-slab silhouette source:
            // bars and panels get contour-hugging walls for free.
            snapshot.outlines =
                std::make_shared<const std::vector<H3D::SOutlineLoop>>(
                    H3D::traceOutlines(snapshot.alphaMask.data(), MW, MH, 64,
                                       1.5f, 4, 128));
        }
    }

    snapshot.lastBuffer = bufferIdentity(SURFACE);

    m_snapshots.erase(ID);
    m_snapshots.emplace(ID, std::move(snapshot));

    return true;
}

bool CWindowCapture::has(std::uintptr_t id) const {
    return m_snapshots.contains(id);
}

const CWindowCapture::SSnapshot* CWindowCapture::get(std::uintptr_t id) const {
    const auto IT = m_snapshots.find(id);

    return IT == m_snapshots.end() ? nullptr : &IT->second;
}

void CWindowCapture::destroySnapshotGL(SSnapshot& snapshot) {
    if (snapshot.bigTex && Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    destroyTexture(snapshot.bigTex);

    // The monitor-sized fb frees its GL objects through the SP.
    snapshot.fb    = nullptr;
    snapshot.texID = 0;
}

void CWindowCapture::release(std::uintptr_t id) {
    if (auto IT = m_snapshots.find(id); IT != m_snapshots.end()) {
        destroySnapshotGL(IT->second);
        m_snapshots.erase(IT);
    }

    m_skirtStates.erase(id);
}

void CWindowCapture::releaseAll() {
    for (auto& [id, snapshot] : m_snapshots)
        destroySnapshotGL(snapshot);

    m_snapshots.clear();
    m_skirtStates.clear();
}

void CWindowCapture::retainOnly(const std::vector<std::uintptr_t>& keep) {
    for (auto IT = m_snapshots.begin(); IT != m_snapshots.end();) {
        const bool FOUND =
            std::find(keep.begin(), keep.end(), IT->first) != keep.end();

        if (FOUND)
            ++IT;
        else {
            destroySnapshotGL(IT->second);
            m_skirtStates.erase(IT->first);
            IT = m_snapshots.erase(IT);
        }
    }
}

void CWindowCapture::shutdownGL() {
    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    releaseAll();
    shutdownCopyGL();
}

} // namespace H3D::Compat

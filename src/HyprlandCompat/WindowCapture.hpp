#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

#include "World/Outline.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace H3D::Compat {

// Owns compositor-rendered snapshots (makeSnapshotFB) of participating windows
// while 3D mode is active. The framebuffer is monitor-sized; the caller uses
// the window's monitor-local box to pick the UV subrect containing the window.
class CWindowCapture {
  public:
    struct SSnapshot {
        SP<Render::IFramebuffer> fb;
        unsigned int             texID  = 0;

        // Composite texture for a window larger than the monitor: it spans
        // the whole decorated box, so the UV subrect is the full [0..1] range
        // and texID stays 0.
        unsigned int             bigTex = 0;
        int                      width  = 0;
        int                      height = 0;

        // Geometry captured WITH the snapshot. The live window box is written
        // after render.pre (the resize path runs at RENDER_LAST_MOMENT), so
        // UVs taken from the live box trail ahead of the captured pixels and
        // smear the content edges during resizes. The quad, the UV subrect and
        // the picking geometry must all follow THIS box instead.
        //
        // fullBox is the window's real box; sampledBox is where the content
        // actually sits INSIDE the captured texture. They differ when the
        // window extends past the monitor viewport: the snapshot pass cannot
        // capture pixels off-screen, so the workspace render offset is warped
        // for the duration of the pass to pull the whole window into view, and
        // sampledBox records the shifted placement.
        CBox     fullBox       = {};
        CBox     sampledBox    = {};
        Vector2D surfaceOffset = {};
        Vector2D surfaceSize   = {};

        // Logical span of the captured texture. It equals the monitor size
        // unless the window is larger than the monitor and the monitor was
        // temporarily enlarged for the snapshot -- the UV subrect must divide
        // by THIS, not by the live monitor size.
        Vector2D texSpan       = {};

        // Downsampled alpha mask of the captured content (layer surfaces
        // only): an invisible overlay must not swallow the crosshair, so
        // picking falls through pixels whose alpha is ~0 to the surface
        // actually underneath.
        std::vector<unsigned char> alphaMask;
        int  alphaW     = 0;
        int  alphaH     = 0;
        bool alphaValid = false;
        unsigned int maskAge = 0;

        // Depth-slab silhouette: closed outlines of the captured alpha in
        // normalized box coords (row 0 = top). Windows: the snapshot
        // texture is drawn into a small owned FBO first (the GPU samples
        // the snapshot's alpha correctly -- glReadPixels on the snapshot FB
        // itself returns garbage) and the mask is traced from that.
        // Layers: traced straight from the picking alphaMask. The outline
        // itself is OWNED by the per-window cadence state (it must survive
        // the snapshot object being recreated on every content update);
        // this pointer is refreshed into each new snapshot.
        std::shared_ptr<const std::vector<H3D::SOutlineLoop>> outlines;

        // Diagnostics for the status dump (the mask bytes and cadence live
        // in the per-window state below).
        int  skirtW     = 0;
        int  skirtH     = 0;
        bool skirtValid = false;
        // Last refresh failure: 0 ok, 1 no texture, 2 FBO/shader setup
        // failed, 3 empty region, 4 GL error.
        int  skirtError = 0;

        // Identity of the client's last-committed buffer at snapshot time:
        // the cheap "did the content change" signal between frames.
        std::uintptr_t lastBuffer = 0;
    };

    // Per-window silhouette state: the traced outline + the cadence
    // bookkeeping. Lives across snapshot recreations (the snapshot object
    // is recreated on every content update); entries die with
    // release/retainOnly/releaseAll, like the snapshots themselves.
    struct SSkirtState {
        int  w = 0, h = 0;       // mask size the outline was traced from
        int  boxW = 0, boxH = 0; // logical box size it was built for
        int  lastBoxW = -1, lastBoxH = -1; // box on the previous snapshot
        bool valid = false;
        unsigned int age = 0;
        // Refresh throttle: during a drag-resize the box changes every
        // frame, and a native-resolution copy per frame tanks the FPS --
        // so mid-drag refreshes run at a reduced mask cap and no more
        // often than this interval. The full-resolution trace is saved
        // for the frame the box settles (stopped changing while stale).
        std::chrono::steady_clock::time_point lastRefresh{};
        bool hasTime = false;
        std::shared_ptr<const std::vector<H3D::SOutlineLoop>> outlines;
    };

    // Renders a fresh snapshot of `window` (uses the focused monitor's output).
    // Returns true and updates the stored snapshot on success. Without `force`
    // the snapshot is skipped when neither the client's committed buffer nor
    // the window's box changed since the last one -- the snapshot pass is the
    // heaviest per-frame cost, and idle windows do not need it.
    bool makeSnapshot(const PHLWINDOW& window, const PHLMONITOR& monitor, bool force = false);

    // Same for a layer-shell surface (panel/bar): monitor-sized capture at
    // the layer's monitor-local box, no decorations, no off-screen pull.
    bool makeSnapshotLayer(const PHLLS& layer, const PHLMONITOR& monitor, bool force = false);

    // A window's popup (menu, tooltip), captured at its own box; `box` is
    // its monitor-local box.
    bool makeSnapshotPopup(const WP<Desktop::View::CPopup>& popup, const CBox& box,
                           const PHLMONITOR& monitor, bool force = false);

    // While set, the depth-slab silhouette traces are postponed entirely:
    // the view morph resizes boxes every frame and a mid-flight trace is
    // wasted work. The analytic fallback outlines cover the visuals; the
    // stale/settle cadence re-traces at full resolution on the first steady
    // frames after the flag clears.
    void setSkirtDeferred(bool on) {
        m_skirtDeferred = on;
    }

  private:
    // Oversized windows: monitor-sized tiles, composited into one texture.
    bool makeTiledSnapshot(
        const PHLWINDOW&  window,
        const PHLMONITOR& monitor,
        const CBox&       fullBox,
        const Vector2D&   surfOffset,
        const Vector2D&   surfSize,
        std::uintptr_t    id,
        bool              force
    );

    // (Re)traces the depth-slab silhouette for a captured snapshot: draws
    // the snapshot texture's box region into a small owned FBO (a GPU-side
    // copy -- the only reliable way to see the snapshot's real alpha) and
    // traces the outline. Rare by design: on a box size change or every
    // 64th snapshot -- the cadence state and the traced outline live in a
    // per-window map, because the snapshot object itself is recreated on
    // every content update and would otherwise reset the cadence to "now"
    // (a native-resolution readback on every frame of an animating window).
    void refreshSkirtMask(std::uintptr_t id, SSnapshot& snapshot,
                          const PHLMONITOR& monitor);
    void refreshSkirtMaskTiled(std::uintptr_t id, SSnapshot& snapshot);

    void destroySnapshotGL(SSnapshot& snapshot);

  public:

    // True if this id currently has a live snapshot.
    bool has(std::uintptr_t id) const;

    // The stored snapshot info (fb may still be null if never snapshotted).
    const SSnapshot* get(std::uintptr_t id) const;

    // Drop the snapshot for one window (window closed / removed from 3D).
    void release(std::uintptr_t id);

    // Drop everything (exiting 3D mode).
    void releaseAll();

    // Keep only the ids present in `keep`; release the rest.
    void retainOnly(const std::vector<std::uintptr_t>& keep);

    void shutdownGL();

  private:
    std::unordered_map<std::uintptr_t, SSnapshot> m_snapshots;
    std::unordered_map<std::uintptr_t, SSkirtState> m_skirtStates;
    bool m_skirtDeferred = false;
};

} // namespace H3D::Compat

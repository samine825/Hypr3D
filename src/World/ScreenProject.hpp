#pragma once

#include "Camera.hpp"

namespace H3D::ScreenProject {

// Back-projection of a 2D monitor rectangle into the room: the pose at which
// a window quad, rendered from the given camera, covers its rectangle on the
// screen exactly. This is the same construction the fullscreen passthrough
// uses for the single monitor-sized quad, generalized to any rectangle --
// the building block of the 2D<->3D view morph (windows take off from their
// 2D spots and land in their room spots without a fade).
struct SScreenPose {
    Vec3  center{};
    float yaw   = 0.f;
    float pitch = 0.f;
};

// camPos/camYaw/camPitch: the camera pose (radians). mirror: F5 front view
// (the render looks backward). zoom: render magnification (1 = base fov).
// baseFovRad: the unzoomed vertical fov. monW/monH: monitor size in logical
// px. boxCenterX/Y: the rectangle's centre in monitor-local px (x right,
// y DOWN). stackOffset: extra world units TOWARD the eye -- coplanar
// rectangles keep a stacking order instead of z-fighting (a tiny projection
// error by design). The rectangle's SIZE does not affect the pose; the quad
// size comes from widthWorld/heightWorld.
//
// Roll is deliberately ignored: the 2D desktop renders unrolled, and the
// walk-bob roll is gated off during the morph.
SScreenPose project(
    const Vec3& camPos,
    float camYaw, float camPitch, bool mirror,
    float zoom, float baseFovRad,
    float monW, float monH,
    float boxCenterX, float boxCenterY,
    float stackOffset);

// The rectangle's quad size in world units (LOGICAL_PX_PER_UNIT).
inline float widthWorld(float boxW)  { return boxW / 100.0f; }
inline float heightWorld(float boxH) { return boxH / 100.0f; }

} // namespace H3D::ScreenProject

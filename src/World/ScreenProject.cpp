#include "World/ScreenProject.hpp"

#include <cmath>

namespace H3D::ScreenProject {

SScreenPose project(
    const Vec3& camPos,
    float camYaw, float camPitch, bool mirror,
    float zoom, float baseFovRad,
    float monW, float monH,
    float boxCenterX, float boxCenterY,
    float stackOffset) {

    SScreenPose out;

    // The distance where a monitor-sized plane subtends the full frustum:
    // d = (worldHeight / 2) / tan(vfov / 2). Zoom narrows the render fov,
    // so the effective tan-half scales down by the magnification -- the
    // same relation GLScene's projection uses.
    const float Z = zoom > 0.01f ? zoom : 0.01f;
    const float TAN_HALF = std::tan(baseFovRad * 0.5f) / Z;
    const float D = (monH * 0.5f / 100.0f) / TAN_HALF - stackOffset;

    // Camera basis (Camera::forward/right conventions), screen axes of the
    // rendered view. MirrorView flips the look direction: the screen right
    // flips with it, the screen up does not (cross(-r, -f) = cross(r, f)).
    const float CP = std::cos(camPitch);
    Vec3 fwd{std::sin(camYaw) * CP, std::sin(camPitch),
             -std::cos(camYaw) * CP};

    Vec3 right{std::cos(camYaw), 0.f, std::sin(camYaw)};

    if (mirror)
        fwd = fwd * -1.0f, right = right * -1.0f;

    const Vec3 up = cross(right, fwd); // unit: right and fwd are orthonormal

    // The rectangle's centre offset from the monitor centre, in world units
    // on the frustum plane. Screen y grows down; world y grows up.
    const float DX = (boxCenterX - monW * 0.5f) / 100.0f;
    const float DY = (monH * 0.5f - boxCenterY) / 100.0f;

    out.center = camPos + fwd * D + right * DX + up * DY;

    // Face the camera. The quad model maps its normal through
    // normal(yaw, pitch) = (sin yaw cos p, -sin p, cos yaw cos p) and its
    // local +X through (cos yaw, 0, -sin yaw).
    //   Plain view: the eye sits along +fwd, so the normal must equal -fwd
    //   (yaw = -camYaw, pitch = +camPitch -- the fullscreen passthrough
    //   convention) and that same yaw puts +X exactly on the camera's right
    //   vector: content lands unmirrored.
    //   MirrorView (F5): the eye looks along -fwd, the normal must equal
    //   +fwd and the screen right is cross(-fwd, up) = -right -- both are
    //   satisfied by yaw = pi - camYaw, pitch = -camPitch.
    if (mirror) {
        out.yaw   = 3.14159265358979f - camYaw;
        out.pitch = -camPitch;
    } else {
        out.yaw   = -camYaw;
        out.pitch = camPitch;
    }

    return out;
}

} // namespace H3D::ScreenProject

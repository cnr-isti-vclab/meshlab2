#pragma once

// A MeshLab camera (vcg::Shot) as the OpenCV pinhole camera TextureConsolidation reads.
//
// The two conventions differ in three ways that each still produce a plausible picture
// when they are got wrong, so they are written down here and checked by
// FilterTests::textureConsolidationCameraMatchesShotProjection rather than trusted:
//
//  - Camera axes. vcg::Shot::ConvertWorldToCameraCoordinates gives R (p - C) with z
//    negated, so points in front have positive z and y points up. OpenCV's y points
//    down, so its camera frame is diag(1, -1, -1) applied to R (p - C).
//  - Image rows. A vcg pixel y counts from the bottom of the image -- MeshLab samples a
//    raster at row ih - 1 - int(y) -- while OpenCV counts rows from the top.
//  - Pixel centres. vcg pixel k spans [k, k+1), so its centre is k + 0.5; OpenCV, and
//    BackProjection's bilinear lookup, put pixel centres on the integers. Hence the
//    half-pixel shifts in cx and cy.
//
// `worldScale` multiplies world coordinates, matching a mesh exported at that scale;
// rotations and intrinsics are unaffected. `imageSize` is the size of the image actually
// written, which may differ from the shot's viewport: the intrinsics are rescaled to it.

#include <vcg/math/shot.h>

#include <array>

namespace texture_consolidation {

struct PinholeCamera
{
    int width = 0;
    int height = 0;
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    std::array<std::array<double, 4>, 4> w2c{}; // row-major world-to-camera
};

template <class ShotType>
PinholeCamera toPinholeCamera(const ShotType &shot, double worldScale, int imageWidth, int imageHeight)
{
    const auto &in = shot.Intrinsics;
    const double viewW = double(in.ViewportPx[0]);
    const double viewH = double(in.ViewportPx[1]);
    const double sx = double(imageWidth) / viewW;
    const double sy = double(imageHeight) / viewH;

    PinholeCamera cam;
    cam.width = imageWidth;
    cam.height = imageHeight;
    cam.fx = double(in.FocalMm) / double(in.PixelSizeMm[0]) * sx;
    cam.fy = double(in.FocalMm) / double(in.PixelSizeMm[1]) * sy;
    cam.cx = double(in.CenterPx[0]) * sx - 0.5;
    cam.cy = (viewH - double(in.CenterPx[1])) * sy - 0.5;

    vcg::Matrix44<typename ShotType::ScalarType> rot;
    shot.Extrinsics.Rot().ToMatrix(rot);
    const auto c = shot.GetViewPoint();
    const double flip[3] = {1.0, -1.0, -1.0};
    for (int r = 0; r < 3; ++r) {
        double t = 0.0;
        for (int k = 0; k < 3; ++k) {
            cam.w2c[size_t(r)][size_t(k)] = flip[r] * double(rot[r][k]);
            t -= cam.w2c[size_t(r)][size_t(k)] * double(c[k]);
        }
        cam.w2c[size_t(r)][3] = t * worldScale;
    }
    cam.w2c[3] = {0.0, 0.0, 0.0, 1.0};
    return cam;
}

} // namespace texture_consolidation

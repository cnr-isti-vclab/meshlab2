#pragma once

#include "vcgmesh.h"

#include <QMatrix4x4>
#include <QString>
#include <vector>

namespace open3d::geometry {
class PointCloud;
class TriangleMesh;
} // namespace open3d::geometry

namespace meshlab::open3d {

// VCGMesh <-> Open3D. The shape follows libiglmeshadapter and
// geogrammeshadapter: an index mapping back to the source so every filter
// treats deleted vertices identically, and one place where the layer matrix is
// applied.
//
// Open3D point clouds are three parallel std::vectors of Eigen::Vector3d, so
// unlike GEO::Mesh there is no container model to reconcile -- the work here is
// the mapping and the attribute round-trip, not the structure.
struct CloudMapping
{
    // Open3D row -> index in the source VCGMesh vertex vector.
    std::vector<int> vertexToSourceIndex;
    bool hadNormals = false;
    bool hadColors = false;
};

// Vertices only: a MeshLab point-cloud layer is a VCGMesh with no faces, and
// these filters ignore faces even when a layer has them. Normals and colors
// travel when the layer carries them.
bool meshToCloud(
    const VCGMesh &in,
    ::open3d::geometry::PointCloud &out,
    CloudMapping &mapping,
    QString &error,
    const QMatrix4x4 *transform = nullptr);

// Writes a cloud back as a new vertex-only VCGMesh. Used by the filters that
// produce a layer rather than modifying one.
bool cloudToMesh(
    const ::open3d::geometry::PointCloud &in,
    VCGMesh &out,
    QString &error);

bool triangleMeshToMesh(
    const ::open3d::geometry::TriangleMesh &in,
    VCGMesh &out,
    QString &error);

} // namespace meshlab::open3d

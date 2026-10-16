#include "open3dmeshadapter.h"

#include "open3d/geometry/PointCloud.h"
#include "open3d/geometry/TriangleMesh.h"

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>

#include <QObject>
#include <QVector3D>

namespace meshlab::open3d {
namespace {

vcg::Point3f transformedPoint(const vcg::Point3f &p, const QMatrix4x4 *transform)
{
    if (!transform)
        return p;
    const QVector3D q = transform->map(QVector3D(p.X(), p.Y(), p.Z()));
    return vcg::Point3f(q.x(), q.y(), q.z());
}

} // namespace

bool meshToCloud(
    const VCGMesh &in,
    ::open3d::geometry::PointCloud &out,
    CloudMapping &mapping,
    QString &error,
    const QMatrix4x4 *transform)
{
    out.Clear();
    mapping = CloudMapping();

    if (in.VN() <= 0) {
        error = QObject::tr("The layer has no vertices.");
        return false;
    }

    // VCGMesh always allocates its normals, so HasPerVertexNormal is true for
    // every layer and says nothing: a layer that never had normals holds zeros.
    // "Has normals" therefore means that some live vertex carries a non-zero one,
    // which is what every refusal below -- "needs normals, run Compute Point Cloud
    // Normals" -- depends on. Colors are the same, but cannot be told apart this
    // way: an unset color is a valid one, so hadColors stays a statement about
    // the component rather than the data.
    mapping.hadNormals = false;
    mapping.hadColors = vcg::tri::HasPerVertexColor(in);

    mapping.vertexToSourceIndex.reserve(size_t(in.VN()));
    out.points_.reserve(size_t(in.VN()));

    for (size_t i = 0; i < in.vert.size(); ++i) {
        const VCGVertex &v = in.vert[i];
        if (v.IsD())
            continue;
        if (v.cN().SquaredNorm() > 0.0f)
            mapping.hadNormals = true;
        const vcg::Point3f p = transformedPoint(v.cP(), transform);
        out.points_.emplace_back(double(p.X()), double(p.Y()), double(p.Z()));
        mapping.vertexToSourceIndex.push_back(int(i));
    }

    if (out.points_.empty()) {
        error = QObject::tr("The layer has no live vertices.");
        return false;
    }

    // Normals rotate with the layer matrix but must not translate, so they go
    // through the matrix's linear part rather than mapping a point.
    if (mapping.hadNormals) {
        out.normals_.reserve(out.points_.size());
        for (int sourceIndex : mapping.vertexToSourceIndex) {
            vcg::Point3f n = in.vert[size_t(sourceIndex)].cN();
            if (transform) {
                const QVector3D q =
                    transform->mapVector(QVector3D(n.X(), n.Y(), n.Z()));
                n = vcg::Point3f(q.x(), q.y(), q.z());
            }
            out.normals_.emplace_back(double(n.X()), double(n.Y()), double(n.Z()));
        }
    }

    if (mapping.hadColors) {
        out.colors_.reserve(out.points_.size());
        for (int sourceIndex : mapping.vertexToSourceIndex) {
            const vcg::Color4b &c = in.vert[size_t(sourceIndex)].cC();
            out.colors_.emplace_back(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
        }
    }

    return true;
}

bool cloudToMesh(
    const ::open3d::geometry::PointCloud &in,
    VCGMesh &out,
    QString &error)
{
    out.Clear();
    if (in.points_.empty()) {
        error = QObject::tr("Open3D returned an empty point cloud.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::AddVertices(out, int(in.points_.size()));
    for (size_t i = 0; i < in.points_.size(); ++i) {
        const auto &p = in.points_[i];
        out.vert[i].P() = vcg::Point3f(float(p.x()), float(p.y()), float(p.z()));
    }

    if (in.normals_.size() == in.points_.size()) {
        for (size_t i = 0; i < in.normals_.size(); ++i) {
            const auto &n = in.normals_[i];
            out.vert[i].N() = vcg::Point3f(float(n.x()), float(n.y()), float(n.z()));
        }
    }

    if (in.colors_.size() == in.points_.size()) {
        out.vert.EnableColor();
        for (size_t i = 0; i < in.colors_.size(); ++i) {
            const auto &c = in.colors_[i];
            out.vert[i].C() = vcg::Color4b(
                (unsigned char)std::clamp(c.x() * 255.0, 0.0, 255.0),
                (unsigned char)std::clamp(c.y() * 255.0, 0.0, 255.0),
                (unsigned char)std::clamp(c.z() * 255.0, 0.0, 255.0),
                255);
        }
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(out);
    return true;
}

bool triangleMeshToMesh(
    const ::open3d::geometry::TriangleMesh &in,
    VCGMesh &out,
    QString &error)
{
    out.Clear();
    if (in.vertices_.empty() || in.triangles_.empty()) {
        error = QObject::tr("Open3D returned an empty mesh.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::AddVertices(out, int(in.vertices_.size()));
    for (size_t i = 0; i < in.vertices_.size(); ++i) {
        const auto &p = in.vertices_[i];
        out.vert[i].P() = vcg::Point3f(float(p.x()), float(p.y()), float(p.z()));
    }

    if (in.vertex_colors_.size() == in.vertices_.size()) {
        out.vert.EnableColor();
        for (size_t i = 0; i < in.vertex_colors_.size(); ++i) {
            const auto &c = in.vertex_colors_[i];
            out.vert[i].C() = vcg::Color4b(
                (unsigned char)std::clamp(c.x() * 255.0, 0.0, 255.0),
                (unsigned char)std::clamp(c.y() * 255.0, 0.0, 255.0),
                (unsigned char)std::clamp(c.z() * 255.0, 0.0, 255.0),
                255);
        }
    }

    const int vertexCount = int(in.vertices_.size());
    for (const auto &t : in.triangles_) {
        if (t(0) < 0 || t(1) < 0 || t(2) < 0
            || t(0) >= vertexCount || t(1) >= vertexCount || t(2) >= vertexCount) {
            error = QObject::tr("Open3D returned a face with an invalid vertex reference.");
            return false;
        }
        if (t(0) == t(1) || t(1) == t(2) || t(2) == t(0))
            continue;
        vcg::tri::Allocator<VCGMesh>::AddFace(out, t(0), t(1), t(2));
    }

    if (out.FN() <= 0) {
        error = QObject::tr("Open3D returned no usable triangles.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(out);
    vcg::tri::UpdateBounding<VCGMesh>::Box(out);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(out);
    return true;
}

} // namespace meshlab::open3d

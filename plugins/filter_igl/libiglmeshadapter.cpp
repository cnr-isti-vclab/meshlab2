#include "libiglmeshadapter.h"

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>
#include <QObject>
#include <QVector4D>
#include <array>

namespace meshlab::libigl {
namespace {

vcg::Point3f transformedPoint(const vcg::Point3f &p, const QMatrix4x4 *transform)
{
    if (!transform)
        return p;

    const QVector4D q = (*transform) * QVector4D(p.X(), p.Y(), p.Z(), 1.0f);
    return vcg::Point3f(q.x(), q.y(), q.z());
}

} // namespace

bool meshToEigen(
    const VCGMesh &mesh,
    EigenMesh &out,
    QString &error,
    const QMatrix4x4 *transform)
{
    out = EigenMesh();
    if (mesh.VN() <= 0 || mesh.FN() <= 0) {
        error = QObject::tr("The mesh must contain vertices and triangular faces.");
        return false;
    }

    std::vector<bool> referenced(mesh.vert.size(), false);
    const VCGVertex *vertexBase = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    for (const VCGFace &face : mesh.face) {
        if (face.IsD())
            continue;
        for (int k = 0; k < 3; ++k) {
            const VCGVertex *vertex = face.cV(k);
            if (!vertex || !vertexBase)
                continue;
            const ptrdiff_t raw = vertex - vertexBase;
            if (raw >= 0 && size_t(raw) < referenced.size())
                referenced[size_t(raw)] = true;
        }
    }

    std::vector<int> sourceToEigen(mesh.vert.size(), -1);
    out.vertexToSourceIndex.reserve(size_t(std::max(0, mesh.VN())));
    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &vertex = mesh.vert[i];
        if (vertex.IsD() || !referenced[i])
            continue;
        sourceToEigen[i] = int(out.vertexToSourceIndex.size());
        out.vertexToSourceIndex.push_back(int(i));
    }

    if (out.vertexToSourceIndex.empty()) {
        error = QObject::tr("The mesh has no face-referenced vertices.");
        return false;
    }

    out.vertices.resize(Eigen::Index(out.vertexToSourceIndex.size()), 3);
    for (Eigen::Index row = 0; row < out.vertices.rows(); ++row) {
        const int sourceIndex = out.vertexToSourceIndex[size_t(row)];
        const vcg::Point3f p = transformedPoint(mesh.vert[size_t(sourceIndex)].cP(), transform);
        out.vertices(row, 0) = double(p.X());
        out.vertices(row, 1) = double(p.Y());
        out.vertices(row, 2) = double(p.Z());
    }

    std::vector<std::array<int, 3>> faces;
    faces.reserve(size_t(std::max(0, mesh.FN())));
    out.faceToSourceIndex.reserve(size_t(std::max(0, mesh.FN())));
    for (size_t faceIndex = 0; faceIndex < mesh.face.size(); ++faceIndex) {
        const VCGFace &face = mesh.face[faceIndex];
        if (face.IsD())
            continue;

        std::array<int, 3> tri{};
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
            const VCGVertex *vertex = face.cV(k);
            if (!vertex || !vertexBase) {
                valid = false;
                break;
            }
            const ptrdiff_t raw = vertex - vertexBase;
            if (raw < 0 || size_t(raw) >= sourceToEigen.size()) {
                valid = false;
                break;
            }
            const int mapped = sourceToEigen[size_t(raw)];
            if (mapped < 0) {
                valid = false;
                break;
            }
            tri[size_t(k)] = mapped;
        }

        if (!valid || tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0]) {
            ++out.skippedFaces;
            continue;
        }

        faces.push_back(tri);
        out.faceToSourceIndex.push_back(int(faceIndex));
    }

    if (faces.empty()) {
        error = QObject::tr("The mesh has no valid triangular faces.");
        return false;
    }

    out.faces.resize(Eigen::Index(faces.size()), 3);
    for (Eigen::Index row = 0; row < out.faces.rows(); ++row) {
        const std::array<int, 3> &tri = faces[size_t(row)];
        out.faces(row, 0) = tri[0];
        out.faces(row, 1) = tri[1];
        out.faces(row, 2) = tri[2];
    }

    return true;
}

bool meshVerticesToEigen(
    const VCGMesh &mesh,
    VertexMatrix &vertices,
    std::vector<int> &vertexToSourceIndex,
    QString &error,
    const QMatrix4x4 *transform)
{
    vertexToSourceIndex.clear();
    vertexToSourceIndex.reserve(size_t(std::max(0, mesh.VN())));
    for (size_t i = 0; i < mesh.vert.size(); ++i)
        if (!mesh.vert[i].IsD())
            vertexToSourceIndex.push_back(int(i));
    if (vertexToSourceIndex.empty()) {
        error = QObject::tr("The mesh has no vertices.");
        return false;
    }

    vertices.resize(Eigen::Index(vertexToSourceIndex.size()), 3);
    for (Eigen::Index row = 0; row < vertices.rows(); ++row) {
        const vcg::Point3f p = transformedPoint(
            mesh.vert[size_t(vertexToSourceIndex[size_t(row)])].cP(), transform);
        vertices.row(row) << double(p.X()), double(p.Y()), double(p.Z());
    }
    return true;
}

bool eigenToMesh(
    const VertexMatrix &vertices,
    const FaceMatrix &faces,
    VCGMesh &out,
    QString &error)
{
    out.Clear();
    if (vertices.rows() <= 0 || faces.rows() <= 0) {
        error = QObject::tr("The output mesh is empty.");
        return false;
    }

    if (vertices.cols() != 3 || faces.cols() != 3) {
        error = QObject::tr("The output mesh matrices must have three columns.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::AddVertices(out, int(vertices.rows()));
    for (Eigen::Index i = 0; i < vertices.rows(); ++i) {
        out.vert[size_t(i)].P() = vcg::Point3f(
            float(vertices(i, 0)),
            float(vertices(i, 1)),
            float(vertices(i, 2)));
    }

    for (Eigen::Index i = 0; i < faces.rows(); ++i) {
        const int a = faces(i, 0);
        const int b = faces(i, 1);
        const int c = faces(i, 2);
        if (a < 0 || b < 0 || c < 0
            || a >= vertices.rows() || b >= vertices.rows() || c >= vertices.rows()) {
            error = QObject::tr("The output mesh contains an invalid face vertex reference.");
            return false;
        }
        if (a == b || b == c || c == a)
            continue;
        vcg::tri::Allocator<VCGMesh>::AddFace(out, a, b, c);
    }

    if (out.FN() <= 0) {
        error = QObject::tr("The output mesh has no valid triangular faces.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(out);
    vcg::tri::UpdateBounding<VCGMesh>::Box(out);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(out);
    return true;
}

bool selectedVertexRows(
    const VCGMesh &mesh,
    const EigenMesh &source,
    Eigen::VectorXi &rows,
    QString &error)
{
    std::vector<int> selected;
    selected.reserve(source.vertexToSourceIndex.size());
    for (size_t row = 0; row < source.vertexToSourceIndex.size(); ++row) {
        const int sourceIndex = source.vertexToSourceIndex[row];
        if (sourceIndex >= 0 && size_t(sourceIndex) < mesh.vert.size()
            && mesh.vert[size_t(sourceIndex)].IsS()) {
            selected.push_back(int(row));
        }
    }
    if (selected.empty()) {
        error = QObject::tr("Select at least one face-referenced vertex.");
        return false;
    }

    rows.resize(Eigen::Index(selected.size()));
    for (Eigen::Index i = 0; i < rows.size(); ++i)
        rows(i) = selected[size_t(i)];
    return true;
}

bool writeVertexScalars(
    VCGMesh &mesh,
    const EigenMesh &source,
    const Eigen::VectorXd &values,
    QString &error)
{
    return writeVertexScalars(mesh, source.vertexToSourceIndex, values, error);
}

bool writeVertexScalars(
    VCGMesh &mesh,
    const std::vector<int> &vertexToSourceIndex,
    const Eigen::VectorXd &values,
    QString &error)
{
    if (values.size() != Eigen::Index(vertexToSourceIndex.size()) || !values.allFinite()) {
        error = QObject::tr("libigl returned invalid vertex scalar values.");
        return false;
    }

    for (VCGVertex &vertex : mesh.vert)
        if (!vertex.IsD())
            vertex.Q() = 0.0f;
    for (Eigen::Index row = 0; row < values.size(); ++row) {
        const int sourceIndex = vertexToSourceIndex[size_t(row)];
        if (sourceIndex >= 0 && size_t(sourceIndex) < mesh.vert.size())
            mesh.vert[size_t(sourceIndex)].Q() = float(values(row));
    }
    return true;
}

bool writeVertexCurvature(
    VCGMesh &mesh,
    const EigenMesh &source,
    const Eigen::MatrixXd &maximumDirections,
    const Eigen::MatrixXd &minimumDirections,
    const Eigen::VectorXd &maximumValues,
    const Eigen::VectorXd &minimumValues,
    QString &error)
{
    const Eigen::Index count = source.vertices.rows();
    if (maximumDirections.rows() != count || minimumDirections.rows() != count
        || maximumDirections.cols() != 3 || minimumDirections.cols() != 3
        || maximumValues.size() != count || minimumValues.size() != count
        || !maximumDirections.allFinite() || !minimumDirections.allFinite()
        || !maximumValues.allFinite() || !minimumValues.allFinite()) {
        error = QObject::tr("libigl returned invalid principal curvature data.");
        return false;
    }

    mesh.vert.EnableCurvatureDir();
    for (VCGVertex &vertex : mesh.vert) {
        if (vertex.IsD())
            continue;
        vertex.PD1().SetZero();
        vertex.PD2().SetZero();
        vertex.K1() = vertex.K2() = 0.0f;
    }
    for (Eigen::Index row = 0; row < count; ++row) {
        const int sourceIndex = source.vertexToSourceIndex[size_t(row)];
        if (sourceIndex < 0 || size_t(sourceIndex) >= mesh.vert.size())
            continue;
        VCGVertex &vertex = mesh.vert[size_t(sourceIndex)];
        vertex.PD1() = vcg::Point3f(float(maximumDirections(row, 0)),
                                    float(maximumDirections(row, 1)),
                                    float(maximumDirections(row, 2)));
        vertex.PD2() = vcg::Point3f(float(minimumDirections(row, 0)),
                                    float(minimumDirections(row, 1)),
                                    float(minimumDirections(row, 2)));
        vertex.K1() = float(maximumValues(row));
        vertex.K2() = float(minimumValues(row));
    }
    return true;
}

} // namespace meshlab::libigl

#include "iglbooleans.h"

#include "document.h"
#include "filterparam.h"
#include "libiglmeshadapter.h"
#include "meshfilterpluginmanager.h"

#include <igl/copyleft/cgal/mesh_boolean.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <QObject>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <vector>

namespace {

constexpr QLatin1StringView kIntersection("mesh_intersection_libigl");
constexpr QLatin1StringView kUnion("mesh_union_libigl");
constexpr QLatin1StringView kDifference("mesh_difference_libigl");
constexpr QLatin1StringView kXor("mesh_symmetric_difference_libigl");
using Mask = vcg::tri::io::Mask;
namespace IglAdapter = meshlab::libigl;

struct Operand
{
    const Document::MeshEntry *entry = nullptr;
    IglAdapter::EigenMesh eigen;
};

struct BirthVertex
{
    int operand = -1;
    int sourceVertexIndex = -1;
};

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult success(const QStringList &info, int newMeshIndex)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    result.newMeshIndices.push_back(newMeshIndex);
    return result;
}

bool operationForFilter(const QString &filterId, igl::MeshBooleanType &operation, QString &name)
{
    if (filterId == QString::fromLatin1(kIntersection)) {
        operation = igl::MESH_BOOLEAN_TYPE_INTERSECT;
        name = QObject::tr("intersection");
        return true;
    }
    if (filterId == QString::fromLatin1(kUnion)) {
        operation = igl::MESH_BOOLEAN_TYPE_UNION;
        name = QObject::tr("union");
        return true;
    }
    if (filterId == QString::fromLatin1(kDifference)) {
        operation = igl::MESH_BOOLEAN_TYPE_MINUS;
        name = QObject::tr("difference");
        return true;
    }
    if (filterId == QString::fromLatin1(kXor)) {
        operation = igl::MESH_BOOLEAN_TYPE_XOR;
        name = QObject::tr("xor");
        return true;
    }
    return false;
}

int birthOperandIndex(
    int birthFaceIndex,
    const Operand &first,
    const Operand &second,
    int &localFaceRow)
{
    if (birthFaceIndex < 0)
        return -1;
    if (birthFaceIndex < first.eigen.faces.rows()) {
        localFaceRow = birthFaceIndex;
        return 0;
    }
    birthFaceIndex -= int(first.eigen.faces.rows());
    if (birthFaceIndex < second.eigen.faces.rows()) {
        localFaceRow = birthFaceIndex;
        return 1;
    }
    return -1;
}

const Operand *operandByIndex(const Operand &first, const Operand &second, int operandIndex)
{
    if (operandIndex == 0)
        return &first;
    if (operandIndex == 1)
        return &second;
    return nullptr;
}

double resultCoordinateScale(const IglAdapter::VertexMatrix &vertices)
{
    double maxAbs = 1.0;
    for (Eigen::Index row = 0; row < vertices.rows(); ++row)
        for (Eigen::Index col = 0; col < vertices.cols(); ++col)
            maxAbs = std::max(maxAbs, std::abs(vertices(row, col)));
    return maxAbs;
}

double squaredDistance(
    const IglAdapter::VertexMatrix &a,
    Eigen::Index rowA,
    const IglAdapter::VertexMatrix &b,
    Eigen::Index rowB)
{
    double d2 = 0.0;
    for (Eigen::Index col = 0; col < 3; ++col) {
        const double d = a(rowA, col) - b(rowB, col);
        d2 += d * d;
    }
    return d2;
}

void transferFaceAttributes(
    VCGMesh &resultMesh,
    int &ioMask,
    const Eigen::VectorXi &birthFaces,
    const Operand &first,
    const Operand &second,
    bool transferQuality,
    bool transferColor)
{
    if (!transferQuality && !transferColor)
        return;

    if (transferQuality)
        ioMask |= Mask::IOM_FACEQUALITY;
    if (transferColor)
        ioMask |= Mask::IOM_FACECOLOR;

    for (int faceRow = 0; faceRow < resultMesh.FN() && faceRow < birthFaces.size(); ++faceRow) {
        VCGFace &dstFace = resultMesh.face[size_t(faceRow)];
        if (transferQuality)
            dstFace.Q() = 0.0f;
        if (transferColor)
            dstFace.C() = vcg::Color4b(128, 128, 128, 255);

        int localFaceRow = -1;
        const int operandIndex = birthOperandIndex(birthFaces(faceRow), first, second, localFaceRow);
        const Operand *operand = operandByIndex(first, second, operandIndex);
        if (!operand || localFaceRow < 0 || localFaceRow >= int(operand->eigen.faceToSourceIndex.size()))
            continue;

        const int sourceFaceIndex = operand->eigen.faceToSourceIndex[size_t(localFaceRow)];
        if (sourceFaceIndex < 0 || size_t(sourceFaceIndex) >= operand->entry->mesh.face.size())
            continue;

        const VCGFace &sourceFace = operand->entry->mesh.face[size_t(sourceFaceIndex)];
        if (transferQuality && (operand->entry->ioMask & Mask::IOM_FACEQUALITY) != 0)
            dstFace.Q() = sourceFace.cQ();
        if (transferColor && (operand->entry->ioMask & Mask::IOM_FACECOLOR) != 0)
            dstFace.C() = sourceFace.cC();
    }
}

std::vector<BirthVertex> directBirthVertices(
    const IglAdapter::VertexMatrix &resultVertices,
    const IglAdapter::FaceMatrix &resultFaces,
    const Eigen::VectorXi &birthFaces,
    const Operand &first,
    const Operand &second)
{
    std::vector<BirthVertex> birth(size_t(resultVertices.rows()));
    const double eps = std::max(1e-10, resultCoordinateScale(resultVertices) * 1e-8);
    const double eps2 = eps * eps;

    for (Eigen::Index faceRow = 0; faceRow < resultFaces.rows() && faceRow < birthFaces.size(); ++faceRow) {
        int localFaceRow = -1;
        const int operandIndex = birthOperandIndex(birthFaces(faceRow), first, second, localFaceRow);
        const Operand *operand = operandByIndex(first, second, operandIndex);
        if (!operand || localFaceRow < 0 || localFaceRow >= operand->eigen.faces.rows())
            continue;

        for (Eigen::Index corner = 0; corner < 3; ++corner) {
            const int resultVertexRow = resultFaces(faceRow, corner);
            if (resultVertexRow < 0 || size_t(resultVertexRow) >= birth.size())
                continue;
            if (birth[size_t(resultVertexRow)].operand >= 0)
                continue;

            for (Eigen::Index sourceCorner = 0; sourceCorner < 3; ++sourceCorner) {
                const int sourceVertexRow = operand->eigen.faces(localFaceRow, sourceCorner);
                if (sourceVertexRow < 0 || sourceVertexRow >= operand->eigen.vertices.rows())
                    continue;

                if (squaredDistance(resultVertices, resultVertexRow, operand->eigen.vertices, sourceVertexRow) <= eps2) {
                    birth[size_t(resultVertexRow)].operand = operandIndex;
                    birth[size_t(resultVertexRow)].sourceVertexIndex =
                        operand->eigen.vertexToSourceIndex[size_t(sourceVertexRow)];
                    break;
                }
            }
        }
    }

    return birth;
}

void transferDirectVertexAttributes(
    VCGMesh &resultMesh,
    int &ioMask,
    const std::vector<BirthVertex> &birth,
    const Operand &first,
    const Operand &second,
    bool transferQuality,
    bool transferColor)
{
    if (!transferQuality && !transferColor)
        return;

    if (transferQuality)
        ioMask |= Mask::IOM_VERTQUALITY;
    if (transferColor)
        ioMask |= Mask::IOM_VERTCOLOR;

    for (int vertexIndex = 0; vertexIndex < resultMesh.VN(); ++vertexIndex) {
        VCGVertex &dstVertex = resultMesh.vert[size_t(vertexIndex)];
        if (transferQuality)
            dstVertex.Q() = 0.0f;
        if (transferColor)
            dstVertex.C() = vcg::Color4b(128, 128, 128, 255);

        if (size_t(vertexIndex) >= birth.size())
            continue;
        const BirthVertex &birthVertex = birth[size_t(vertexIndex)];
        const Operand *operand = operandByIndex(first, second, birthVertex.operand);
        if (!operand || birthVertex.sourceVertexIndex < 0
            || size_t(birthVertex.sourceVertexIndex) >= operand->entry->mesh.vert.size()) {
            continue;
        }

        const VCGVertex &sourceVertex = operand->entry->mesh.vert[size_t(birthVertex.sourceVertexIndex)];
        if (transferQuality && (operand->entry->ioMask & Mask::IOM_VERTQUALITY) != 0)
            dstVertex.Q() = sourceVertex.cQ();
        if (transferColor && (operand->entry->ioMask & Mask::IOM_VERTCOLOR) != 0)
            dstVertex.C() = sourceVertex.cC();
    }
}

void averageNewVertexAttributes(
    VCGMesh &resultMesh,
    const IglAdapter::FaceMatrix &resultFaces,
    const std::vector<BirthVertex> &birth,
    bool transferQuality,
    bool transferColor)
{
    if (!transferQuality && !transferColor)
        return;

    std::vector<std::vector<int>> neighbors(size_t(resultMesh.VN()));
    for (Eigen::Index faceRow = 0; faceRow < resultFaces.rows(); ++faceRow) {
        const std::array<int, 3> tri = {
            resultFaces(faceRow, 0),
            resultFaces(faceRow, 1),
            resultFaces(faceRow, 2)
        };
        for (int i = 0; i < 3; ++i) {
            const int center = tri[size_t(i)];
            if (center < 0 || center >= resultMesh.VN())
                continue;
            for (int j = 0; j < 3; ++j) {
                if (i == j)
                    continue;
                const int adjacent = tri[size_t(j)];
                if (adjacent >= 0 && adjacent < resultMesh.VN())
                    neighbors[size_t(center)].push_back(adjacent);
            }
        }
    }

    for (int vertexIndex = 0; vertexIndex < resultMesh.VN(); ++vertexIndex) {
        if (size_t(vertexIndex) < birth.size() && birth[size_t(vertexIndex)].operand >= 0)
            continue;

        int count = 0;
        float qualitySum = 0.0f;
        int red = 0;
        int green = 0;
        int blue = 0;
        int alpha = 0;
        for (int adjacent : neighbors[size_t(vertexIndex)]) {
            if (size_t(adjacent) >= birth.size() || birth[size_t(adjacent)].operand < 0)
                continue;
            const VCGVertex &source = resultMesh.vert[size_t(adjacent)];
            ++count;
            if (transferQuality)
                qualitySum += source.cQ();
            if (transferColor) {
                red += source.cC()[0];
                green += source.cC()[1];
                blue += source.cC()[2];
                alpha += source.cC()[3];
            }
        }

        if (count <= 0)
            continue;
        VCGVertex &target = resultMesh.vert[size_t(vertexIndex)];
        if (transferQuality)
            target.Q() = qualitySum / float(count);
        if (transferColor) {
            target.C() = vcg::Color4b(
                uchar(red / count),
                uchar(green / count),
                uchar(blue / count),
                uchar(alpha / count));
        }
    }
}

void transferVertexAttributes(
    VCGMesh &resultMesh,
    int &ioMask,
    const IglAdapter::VertexMatrix &resultVertices,
    const IglAdapter::FaceMatrix &resultFaces,
    const Eigen::VectorXi &birthFaces,
    const Operand &first,
    const Operand &second,
    bool transferQuality,
    bool transferColor)
{
    if (!transferQuality && !transferColor)
        return;

    const std::vector<BirthVertex> birth =
        directBirthVertices(resultVertices, resultFaces, birthFaces, first, second);
    transferDirectVertexAttributes(
        resultMesh,
        ioMask,
        birth,
        first,
        second,
        transferQuality,
        transferColor);
    averageNewVertexAttributes(resultMesh, resultFaces, birth, transferQuality, transferColor);
}

} // namespace

MeshFilterRunResult runIglBooleanFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    igl::MeshBooleanType operation = igl::MESH_BOOLEAN_TYPE_UNION;
    QString operationName;
    if (!operationForFilter(filterId, operation, operationName))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int firstIndex = params.getMesh(QStringLiteral("first_mesh"), doc.currentMeshIndex());
    const int secondIndex = params.getMesh(QStringLiteral("second_mesh"), doc.currentMeshIndex());
    if (firstIndex < 0 || firstIndex >= doc.meshCount()
        || secondIndex < 0 || secondIndex >= doc.meshCount()) {
        return fail(QObject::tr("Boolean operation requires two valid mesh operands."));
    }
    if (firstIndex == secondIndex)
        return fail(QObject::tr("Boolean operation requires two different mesh operands."));

    const bool transferFaceQuality = params.getBool(QStringLiteral("transfer_face_quality"));
    const bool transferFaceColor = params.getBool(QStringLiteral("transfer_face_color"));
    const bool transferVertexQuality = params.getBool(QStringLiteral("transfer_vert_quality"));
    const bool transferVertexColor = params.getBool(QStringLiteral("transfer_vert_color"));

    Operand first;
    first.entry = &doc.mesh(firstIndex);
    Operand second;
    second.entry = &doc.mesh(secondIndex);

    QString error;
    if (!IglAdapter::meshToEigen(first.entry->mesh, first.eigen, error, &first.entry->transform))
        return fail(QObject::tr("First mesh: %1").arg(error));
    if (!IglAdapter::meshToEigen(second.entry->mesh, second.eigen, error, &second.entry->transform))
        return fail(QObject::tr("Second mesh: %1").arg(error));

    doc.beginFilterProgress(QObject::tr("Boolean %1").arg(operationName));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Running libigl/CGAL boolean operation...");

    IglAdapter::VertexMatrix resultVertices;
    IglAdapter::FaceMatrix resultFaces;
    Eigen::VectorXi birthFaces;
    try {
        const bool ok = igl::copyleft::cgal::mesh_boolean(
            first.eigen.vertices,
            first.eigen.faces,
            second.eigen.vertices,
            second.eigen.faces,
            operation,
            resultVertices,
            resultFaces,
            birthFaces);
        if (!ok) {
            error = QObject::tr(
                "Mesh inputs must induce a piecewise constant winding number field. "
                "Make sure both input meshes are watertight and consistently oriented.");
            doc.finishFilterProgress(false, error);
            return fail(error);
        }
    } catch (const std::exception &e) {
        error = QObject::tr("libigl/CGAL boolean operation failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("libigl/CGAL boolean operation failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(80, "Converting boolean output mesh...");

    VCGMesh output;
    if (!IglAdapter::eigenToMesh(resultVertices, resultFaces, output, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    int outputMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    transferFaceAttributes(
        output,
        outputMask,
        birthFaces,
        first,
        second,
        transferFaceQuality,
        transferFaceColor);
    transferVertexAttributes(
        output,
        outputMask,
        resultVertices,
        resultFaces,
        birthFaces,
        first,
        second,
        transferVertexQuality,
        transferVertexColor);

    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);

    const QString resultName = QObject::tr("Boolean %1").arg(operationName);
    const int newMeshIndex = doc.addMesh(output, resultName, outputMask);
    if (newMeshIndex < 0) {
        error = QObject::tr("Failed to add the boolean result mesh to the document.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    doc.finishFilterProgress(true, QObject::tr("Generated boolean result mesh."));

    QStringList info;
    info << QObject::tr("Operation: %1").arg(operationName)
         << QObject::tr("First mesh: '%1' (%2 faces).").arg(first.entry->name).arg(first.eigen.faces.rows())
         << QObject::tr("Second mesh: '%1' (%2 faces).").arg(second.entry->name).arg(second.eigen.faces.rows())
         << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    if (first.eigen.skippedFaces > 0)
        info << QObject::tr("Skipped %1 invalid or degenerate face(s) from the first mesh.").arg(first.eigen.skippedFaces);
    if (second.eigen.skippedFaces > 0)
        info << QObject::tr("Skipped %1 invalid or degenerate face(s) from the second mesh.").arg(second.eigen.skippedFaces);

    return success(info, newMeshIndex);
}


bool isIglBooleanFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kIntersection)
        || filterId == QString::fromLatin1(kUnion)
        || filterId == QString::fromLatin1(kDifference)
        || filterId == QString::fromLatin1(kXor);
}

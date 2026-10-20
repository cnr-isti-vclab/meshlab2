#include "poissonrecon_adapter.h"

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>

#include <algorithm>

namespace {

template<class Scalar>
vcg::Point3<Scalar> qMatrixMapPoint(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &point)
{
    const QVector4D mapped = matrix * QVector4D(point[0], point[1], point[2], 1.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

template<class Scalar>
vcg::Point3<Scalar> qMatrixMapDirection(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &direction)
{
    const QVector4D mapped = matrix * QVector4D(direction[0], direction[1], direction[2], 0.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

void appendToMeshImpl(const std::vector<ScreenedPoisson::VertexRecord> &vertices, const std::vector<ScreenedPoisson::Face> &faces, VCGMesh &mesh)
{
    const int baseVertexIndex = mesh.VN();
    for (const ScreenedPoisson::VertexRecord &vertex : vertices) {
        vcg::tri::Allocator<VCGMesh>::AddVertex(
            mesh,
            vcg::Point3f(vertex.position[0], vertex.position[1], vertex.position[2]));
        mesh.vert.back().Q() = vertex.density;
        mesh.vert.back().N() = vcg::Point3f(vertex.gradient[0], vertex.gradient[1], vertex.gradient[2]);
    }

    for (const auto &face : faces) {
        if (face.size() != 3)
            continue;
        const int i0 = baseVertexIndex + static_cast<int>(face[0]);
        const int i1 = baseVertexIndex + static_cast<int>(face[1]);
        const int i2 = baseVertexIndex + static_cast<int>(face[2]);
        vcg::tri::Allocator<VCGMesh>::AddFace(mesh, &mesh.vert[i0], &mesh.vert[i1], &mesh.vert[i2]);
    }
}

void appendToMeshImpl(const std::vector<ScreenedPoisson::VertexColorRecord> &vertices, const std::vector<ScreenedPoisson::Face> &faces, VCGMesh &mesh)
{
    const int baseVertexIndex = mesh.VN();
    for (const ScreenedPoisson::VertexColorRecord &vertex : vertices) {
        vcg::tri::Allocator<VCGMesh>::AddVertex(
            mesh,
            vcg::Point3f(vertex.position[0], vertex.position[1], vertex.position[2]));
        mesh.vert.back().Q() = vertex.density;
        mesh.vert.back().N() = vcg::Point3f(vertex.gradient[0], vertex.gradient[1], vertex.gradient[2]);
        mesh.vert.back().C()[0] = static_cast<unsigned char>(std::clamp(vertex.color[0], 0.0f, 255.0f));
        mesh.vert.back().C()[1] = static_cast<unsigned char>(std::clamp(vertex.color[1], 0.0f, 255.0f));
        mesh.vert.back().C()[2] = static_cast<unsigned char>(std::clamp(vertex.color[2], 0.0f, 255.0f));
        mesh.vert.back().C()[3] = 255;
    }

    for (const auto &face : faces) {
        if (face.size() != 3)
            continue;
        const int i0 = baseVertexIndex + static_cast<int>(face[0]);
        const int i1 = baseVertexIndex + static_cast<int>(face[1]);
        const int i2 = baseVertexIndex + static_cast<int>(face[2]);
        vcg::tri::Allocator<VCGMesh>::AddFace(mesh, &mesh.vert[i0], &mesh.vert[i1], &mesh.vert[i2]);
    }
}

bool nextSampleImpl(
    const Document &doc,
    const std::vector<int> &meshIndices,
    const ScreenedPoisson::SelectionOptions &options,
    std::size_t &meshCursor,
    std::size_t &vertexCursor,
    ScreenedPoisson::Position &p,
    ScreenedPoisson::Normal &n,
    ScreenedPoisson::Color *color)
{
    while (meshCursor < meshIndices.size()) {
        const Document::MeshEntry &entry = doc.mesh(meshIndices[meshCursor]);
        const auto &vertices = entry.mesh.vert;
        while (vertexCursor < vertices.size()) {
            const VCGVertex &vertex = vertices[vertexCursor++];
            if (!ScreenedPoisson::hasUsableNormal(vertex))
                continue;

            const vcg::Point3<float> pos = qMatrixMapPoint<float>(entry.transform, vcg::Point3<float>(vertex.cP()));
            vcg::Point3<float> normal = qMatrixMapDirection<float>(entry.transform, vcg::Point3<float>(vertex.cN()));
            // A unit normal, so that the confidence is the only thing that scales it.
            const float length = normal.Norm();
            if (length > 0.0f)
                normal /= length;
            if (options.confidenceFromQuality)
                normal *= vertex.cQ();

            for (unsigned int i = 0; i < ScreenedPoisson::Dim; ++i) {
                p[i] = pos[i];
                n[i] = normal[i];
            }

            if (color) {
                (*color)[0] = static_cast<float>(vertex.C()[0]);
                (*color)[1] = static_cast<float>(vertex.C()[1]);
                (*color)[2] = static_cast<float>(vertex.C()[2]);
            }
            return true;
        }
        ++meshCursor;
        vertexCursor = 0;
    }
    return false;
}

} // namespace

namespace ScreenedPoisson
{

std::vector<int> selectedMeshIndices(const Document &doc, bool mergeVisible)
{
    std::vector<int> indices;
    if (!mergeVisible) {
        const int currentIndex = doc.currentMeshIndex();
        if (currentIndex >= 0 && currentIndex < doc.meshCount())
            indices.push_back(currentIndex);
        return indices;
    }

    indices.reserve(doc.meshCount());
    for (int i = 0; i < doc.meshCount(); ++i)
        if (doc.mesh(i).visible)
            indices.push_back(i);
    return indices;
}

bool allSelectedMeshesHaveVertexColor(const Document &doc, const std::vector<int> &meshIndices)
{
    using Mask = vcg::tri::io::Mask;
    if (meshIndices.empty())
        return false;
    for (int meshIndex : meshIndices) {
        if (meshIndex < 0 || meshIndex >= doc.meshCount())
            return false;
        if ((doc.mesh(meshIndex).ioMask & Mask::IOM_VERTCOLOR) == 0)
            return false;
    }
    return true;
}

qsizetype countPointsWithoutNormal(const Document &doc, const std::vector<int> &meshIndices)
{
    qsizetype count = 0;
    for (int meshIndex : meshIndices) {
        if (meshIndex < 0 || meshIndex >= doc.meshCount())
            continue;
        for (const VCGVertex &vertex : doc.mesh(meshIndex).mesh.vert)
            count += hasUsableNormal(vertex) ? 0 : 1;
    }
    return count;
}

qsizetype countInputSamples(const Document &doc, const std::vector<int> &meshIndices)
{
    qsizetype count = 0;
    for (int meshIndex : meshIndices) {
        if (meshIndex >= 0 && meshIndex < doc.meshCount())
            count += doc.mesh(meshIndex).mesh.VN();
    }
    return count - countPointsWithoutNormal(doc, meshIndices);
}

DocumentOrientedPointStream::DocumentOrientedPointStream(const Document &doc, std::vector<int> meshIndices, SelectionOptions options)
    : m_doc(doc)
    , m_meshIndices(std::move(meshIndices))
    , m_options(options)
{
}

void DocumentOrientedPointStream::reset(void)
{
    m_meshCursor = 0;
    m_vertexCursor = 0;
}

bool DocumentOrientedPointStream::read(Position &p, Normal &n)
{
    return nextSample(p, n, nullptr);
}

bool DocumentOrientedPointStream::nextSample(Position &p, Normal &n, Color *color)
{
    return nextSampleImpl(m_doc, m_meshIndices, m_options, m_meshCursor, m_vertexCursor, p, n, color);
}

DocumentOrientedPointColorStream::DocumentOrientedPointColorStream(const Document &doc, std::vector<int> meshIndices, SelectionOptions options)
    : m_doc(doc)
    , m_meshIndices(std::move(meshIndices))
    , m_options(options)
{
}

void DocumentOrientedPointColorStream::reset(void)
{
    m_meshCursor = 0;
    m_vertexCursor = 0;
}

bool DocumentOrientedPointColorStream::read(Position &p, Normal &n, Color &c)
{
    return nextSample(p, n, &c);
}

bool DocumentOrientedPointColorStream::nextSample(Position &p, Normal &n, Color *color)
{
    return nextSampleImpl(m_doc, m_meshIndices, m_options, m_meshCursor, m_vertexCursor, p, n, color);
}

size_t VectorLevelSetVertexStream::write(const Position &p, const Gradient &g, const Weight &w)
{
    const size_t idx = m_vertices.size();
    VertexRecord record;
    record.position = p;
    record.gradient = g;
    record.density = w;
    m_vertices.push_back(record);
    return idx;
}

size_t VectorLevelSetVertexColorStream::write(const Position &p, const Gradient &g, const Weight &w, const Color &c)
{
    const size_t idx = m_vertices.size();
    VertexColorRecord record;
    record.position = p;
    record.gradient = g;
    record.density = w;
    record.color = c;
    m_vertices.push_back(record);
    return idx;
}

size_t VectorFaceStream::write(const Face &f)
{
    const size_t idx = m_faces.size();
    m_faces.push_back(f);
    return idx;
}

void appendToMesh(const std::vector<VertexRecord> &vertices, const std::vector<Face> &faces, VCGMesh &mesh)
{
    appendToMeshImpl(vertices, faces, mesh);
}

void appendToMesh(const std::vector<VertexColorRecord> &vertices, const std::vector<Face> &faces, VCGMesh &mesh)
{
    appendToMeshImpl(vertices, faces, mesh);
}

} // namespace ScreenedPoisson

#include "geogrammeshadapter.h"

#include <geogram/basic/common.h>
#include <geogram/basic/attributes.h>
#include <geogram/basic/command_line.h>
#include <geogram/basic/command_line_args.h>
#include <geogram/basic/logger.h>

#include <vcg/complex/algorithms/mesh_to_matrix.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>

#include <QObject>
#include <QVector4D>
#include <array>
#include <mutex>

namespace meshlab::geogram {
namespace {

thread_local QStringList *tlsLogSink = nullptr;

// Geogram talks to one process-wide Logger. Forwarding straight to a Document
// would need a global pointer to whichever filter is running; a thread-local
// sink that LogCapture swaps in for the duration of one call does the same job
// without the lifetime question.
class SinkLoggerClient final : public ::GEO::LoggerClient
{
public:
    void div(const std::string &) override {}
    void out(const std::string &str) override { append(str); }
    void warn(const std::string &str) override { append(QStringLiteral("warning: "), str); }
    void err(const std::string &str) override { append(QStringLiteral("error: "), str); }
    void status(const std::string &) override {}

private:
    static void append(const std::string &str) { append(QString(), str); }

    static void append(const QString &prefix, const std::string &str)
    {
        if (!tlsLogSink)
            return;
        const QString text = QString::fromStdString(str).trimmed();
        if (text.isEmpty())
            return;
        *tlsLogSink << prefix + text;
    }
};

vcg::Point3f transformedPoint(const vcg::Point3f &p, const QMatrix4x4 *transform)
{
    if (!transform)
        return p;

    const QVector4D q = (*transform) * QVector4D(p.X(), p.Y(), p.Z(), 1.0f);
    return vcg::Point3f(q.x(), q.y(), q.z());
}

} // namespace

void ensureInitialized()
{
    static std::once_flag once;
    std::call_once(once, [] {
        ::GEO::initialize(::GEO::GEOGRAM_INSTALL_NONE);
        // initialize() constructs the CmdLine environment but declares no
        // variables in it, and geogram reads its own defaults back out of that
        // environment: Delaunay::create() asks for "algo:delaunay" and asserts
        // when it is missing, which takes down the whole process rather than
        // failing the call. Anything reaching CVT -- chart segmentation, and
        // the CVT remesher later -- goes through that path. Declaring the
        // group only installs defaults; it parses no command line.
        ::GEO::CmdLine::import_arg_group("algo");
        // Leaked on purpose: GEO::Logger outlives every filter run, and the
        // client must stay valid for as long as it is registered.
        ::GEO::Logger::instance()->register_client(new SinkLoggerClient());
        ::GEO::Logger::instance()->set_quiet(true);
    });
}

LogCapture::LogCapture(QStringList &sink)
    : m_previous(tlsLogSink)
{
    tlsLogSink = &sink;
}

LogCapture::~LogCapture()
{
    tlsLogSink = m_previous;
}

bool meshToGeo(
    const VCGMesh &in,
    GeoMesh &out,
    QString &error,
    const QMatrix4x4 *transform,
    int dimension)
{
    out.mesh.clear();
    out.vertexToSourceIndex.clear();
    out.faceToSourceIndex.clear();
    out.skippedFaces = 0;

    if (dimension < 3) {
        error = QObject::tr("A geogram mesh needs at least three coordinates per vertex.");
        return false;
    }
    if (in.VN() <= 0 || in.FN() <= 0) {
        error = QObject::tr("The mesh must contain vertices and triangular faces.");
        return false;
    }

    std::vector<bool> referenced(in.vert.size(), false);
    const VCGVertex *vertexBase = in.vert.empty() ? nullptr : &in.vert.front();
    for (const VCGFace &face : in.face) {
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

    std::vector<int> sourceToGeo(in.vert.size(), -1);
    out.vertexToSourceIndex.reserve(size_t(std::max(0, in.VN())));
    for (size_t i = 0; i < in.vert.size(); ++i) {
        if (in.vert[i].IsD() || !referenced[i])
            continue;
        sourceToGeo[i] = int(out.vertexToSourceIndex.size());
        out.vertexToSourceIndex.push_back(int(i));
    }

    if (out.vertexToSourceIndex.empty()) {
        error = QObject::tr("The mesh has no face-referenced vertices.");
        return false;
    }

    out.mesh.vertices.set_dimension(::GEO::index_t(dimension));
    out.mesh.vertices.create_vertices(::GEO::index_t(out.vertexToSourceIndex.size()));
    for (size_t row = 0; row < out.vertexToSourceIndex.size(); ++row) {
        const int sourceIndex = out.vertexToSourceIndex[row];
        const vcg::Point3f p = transformedPoint(in.vert[size_t(sourceIndex)].cP(), transform);
        double *coords = out.mesh.vertices.point_ptr(::GEO::index_t(row));
        coords[0] = double(p.X());
        coords[1] = double(p.Y());
        coords[2] = double(p.Z());
        for (int c = 3; c < dimension; ++c)
            coords[c] = 0.0;
    }

    std::vector<std::array<int, 3>> faces;
    faces.reserve(size_t(std::max(0, in.FN())));
    out.faceToSourceIndex.reserve(size_t(std::max(0, in.FN())));
    for (size_t faceIndex = 0; faceIndex < in.face.size(); ++faceIndex) {
        const VCGFace &face = in.face[faceIndex];
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
            if (raw < 0 || size_t(raw) >= sourceToGeo.size() || sourceToGeo[size_t(raw)] < 0) {
                valid = false;
                break;
            }
            tri[size_t(k)] = sourceToGeo[size_t(raw)];
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

    out.mesh.facets.create_triangles(::GEO::index_t(faces.size()));
    for (size_t row = 0; row < faces.size(); ++row) {
        for (int k = 0; k < 3; ++k) {
            out.mesh.facets.set_vertex(
                ::GEO::index_t(row),
                ::GEO::index_t(k),
                ::GEO::index_t(faces[row][size_t(k)]));
        }
    }

    return true;
}

bool geoToMesh(const ::GEO::Mesh &in, VCGMesh &out, QString &error)
{
    out.Clear();

    const ::GEO::index_t vertexCount = in.vertices.nb();
    const ::GEO::index_t facetCount = in.facets.nb();
    if (vertexCount == 0 || facetCount == 0) {
        error = QObject::tr("The output mesh is empty.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::AddVertices(out, int(vertexCount));
    for (::GEO::index_t v = 0; v < vertexCount; ++v) {
        const double *coords = in.vertices.point_ptr(v);
        out.vert[size_t(v)].P() = vcg::Point3f(
            float(coords[0]),
            float(coords[1]),
            float(coords[2]));
    }

    // Geogram's boolean output can carry polygons where coplanar facets were
    // merged, so fan-triangulate rather than assuming triangles.
    for (::GEO::index_t f = 0; f < facetCount; ++f) {
        const ::GEO::index_t corners = in.facets.nb_vertices(f);
        if (corners < 3)
            continue;
        const ::GEO::index_t v0 = in.facets.vertex(f, 0);
        for (::GEO::index_t k = 1; k + 1 < corners; ++k) {
            const ::GEO::index_t v1 = in.facets.vertex(f, k);
            const ::GEO::index_t v2 = in.facets.vertex(f, k + 1);
            if (v0 >= vertexCount || v1 >= vertexCount || v2 >= vertexCount) {
                error = QObject::tr("The output mesh contains an invalid face vertex reference.");
                return false;
            }
            if (v0 == v1 || v1 == v2 || v2 == v0)
                continue;
            vcg::tri::Allocator<VCGMesh>::AddFace(out, int(v0), int(v1), int(v2));
        }
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

bool readVertexTexCoords(
    const ::GEO::Mesh &in,
    std::vector<std::array<float, 2>> &uv,
    QString &error,
    const char *attributeName)
{
    uv.clear();

    ::GEO::Attribute<double> texCoord;
    if (!texCoord.bind_if_is_defined(
            const_cast<::GEO::Mesh &>(in).vertices.attributes(), attributeName)) {
        error = QObject::tr("geogram did not produce a '%1' vertex attribute.")
                    .arg(QString::fromLatin1(attributeName));
        return false;
    }
    if (texCoord.dimension() != 2) {
        error = QObject::tr("The '%1' vertex attribute has %2 components, expected 2.")
                    .arg(QString::fromLatin1(attributeName))
                    .arg(texCoord.dimension());
        return false;
    }

    const ::GEO::index_t vertexCount = in.vertices.nb();
    uv.resize(size_t(vertexCount));
    for (::GEO::index_t v = 0; v < vertexCount; ++v) {
        uv[size_t(v)][0] = float(texCoord[2 * v]);
        uv[size_t(v)][1] = float(texCoord[2 * v + 1]);
    }
    return true;
}

bool readCornerTexCoords(
    const ::GEO::Mesh &in,
    std::vector<std::array<float, 2>> &uv,
    QString &error,
    const char *attributeName)
{
    uv.clear();

    ::GEO::Attribute<double> texCoord;
    if (!texCoord.bind_if_is_defined(
            const_cast<::GEO::Mesh &>(in).facet_corners.attributes(), attributeName)) {
        error = QObject::tr("geogram did not produce a '%1' facet corner attribute.")
                    .arg(QString::fromLatin1(attributeName));
        return false;
    }
    if (texCoord.dimension() != 2) {
        error = QObject::tr("The '%1' facet corner attribute has %2 components, expected 2.")
                    .arg(QString::fromLatin1(attributeName))
                    .arg(texCoord.dimension());
        return false;
    }

    const ::GEO::index_t cornerCount = in.facet_corners.nb();
    uv.resize(size_t(cornerCount));
    for (::GEO::index_t c = 0; c < cornerCount; ++c) {
        uv[size_t(c)][0] = float(texCoord[2 * c]);
        uv[size_t(c)][1] = float(texCoord[2 * c + 1]);
    }
    return true;
}

bool writeWedgeTexCoordsToGeo(
    const VCGMesh &in,
    GeoMesh &out,
    QString &error,
    const char *attributeName)
{
    if (!vcg::tri::HasPerWedgeTexCoord(in)) {
        error = QObject::tr("The mesh has no per-wedge texture coordinates to pack.");
        return false;
    }

    ::GEO::Attribute<double> texCoord;
    texCoord.create_vector_attribute(
        out.mesh.facet_corners.attributes(), attributeName, 2);

    const ::GEO::index_t facetCount = out.mesh.facets.nb();
    for (::GEO::index_t f = 0; f < facetCount; ++f) {
        const int sourceFaceIndex = out.faceToSourceIndex[size_t(f)];
        if (sourceFaceIndex < 0 || size_t(sourceFaceIndex) >= in.face.size())
            continue;
        const VCGFace &face = in.face[size_t(sourceFaceIndex)];
        for (int corner = 0; corner < 3; ++corner) {
            const ::GEO::index_t c = out.mesh.facets.corner(f, ::GEO::index_t(corner));
            texCoord[2 * c] = double(face.cWT(corner).U());
            texCoord[2 * c + 1] = double(face.cWT(corner).V());
        }
    }
    return true;
}

bool readFacetCharts(
    const ::GEO::Mesh &in,
    std::vector<int> &charts,
    QString &error,
    const char *attributeName)
{
    charts.clear();

    ::GEO::Attribute<::GEO::index_t> chart;
    if (!chart.bind_if_is_defined(
            const_cast<::GEO::Mesh &>(in).facets.attributes(), attributeName)) {
        error = QObject::tr("geogram did not produce a '%1' facet attribute.")
                    .arg(QString::fromLatin1(attributeName));
        return false;
    }

    const ::GEO::index_t facetCount = in.facets.nb();
    charts.resize(size_t(facetCount));
    for (::GEO::index_t f = 0; f < facetCount; ++f)
        charts[size_t(f)] = int(chart[f]);
    return true;
}

} // namespace meshlab::geogram

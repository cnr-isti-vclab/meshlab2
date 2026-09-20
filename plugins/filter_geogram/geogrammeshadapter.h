#pragma once

#include "vcgmesh.h"

#include <geogram/mesh/mesh.h>

#include <QMatrix4x4>
#include <QString>
#include <QStringList>
#include <array>
#include <vector>

namespace meshlab::geogram {

// VCGMesh <-> GEO::Mesh. The shape deliberately mirrors libiglmeshadapter: an
// index mapping back to the source mesh so every filter treats deleted and
// unreferenced vertices identically, and one place where the layer matrix is
// applied.
struct GeoMesh
{
    GeoMesh() = default;
    GeoMesh(const GeoMesh &) = delete;
    GeoMesh &operator=(const GeoMesh &) = delete;

    ::GEO::Mesh mesh;
    std::vector<int> vertexToSourceIndex;
    std::vector<int> faceToSourceIndex;
    int skippedFaces = 0;
};

// Calls GEO::initialize(GEOGRAM_INSTALL_NONE) exactly once per process and
// installs the logger client. GEOGRAM_INSTALL_NONE matters: the other flags
// install signal handlers, force the POSIX locale and enable FP exceptions,
// all of which are process-wide and hostile to a running Qt application.
void ensureInitialized();

// Redirects GEO::Logger away from stdout and into `sink` for as long as it is
// alive. Geogram's logger is process-wide while a filter run is not, so the
// sink is thread-local and scoped rather than a global Document pointer.
class LogCapture
{
public:
    explicit LogCapture(QStringList &sink);
    ~LogCapture();
    LogCapture(const LogCapture &) = delete;
    LogCapture &operator=(const LogCapture &) = delete;

private:
    QStringList *m_previous = nullptr;
};

// Always builds a 3-dimensional mesh. Anisotropic remeshing works in 6
// dimensions, but it must get there through GEO::set_anisotropy, which raises
// the dimension *and* fills coordinates 3-5 with the normals -- and which
// skips that work entirely if the mesh already has dimension 6. Handing it a
// 6-dimensional mesh would leave it normalizing zero vectors.
bool meshToGeo(
    const VCGMesh &in,
    GeoMesh &out,
    QString &error,
    const QMatrix4x4 *transform = nullptr);

bool geoToMesh(const ::GEO::Mesh &in, VCGMesh &out, QString &error);

// Reads back a two-component vertex attribute, which is how geogram's
// flatteners return a parametrization ("tex_coord"). Indexed by GEO vertex
// row, so the caller maps through GeoMesh::vertexToSourceIndex.
bool readVertexTexCoords(
    const ::GEO::Mesh &in,
    std::vector<std::array<float, 2>> &uv,
    QString &error,
    const char *attributeName = "tex_coord");

// The facet-corner counterpart: geogram's atlas pipeline and its packers store
// UVs per corner rather than per vertex, which maps to MeshLab's per-wedge
// texture coordinates. Indexed by GEO corner index, so the caller walks facets
// through faceToSourceIndex.
bool readCornerTexCoords(
    const ::GEO::Mesh &in,
    std::vector<std::array<float, 2>> &uv,
    QString &error,
    const char *attributeName = "tex_coord");

// The other direction: copies the mesh's existing per-wedge UVs into the
// facet-corner "tex_coord" attribute geogram's packers expect to find. A
// packer rearranges an atlas it is given, so without this there is nothing
// for it to pack.
bool writeWedgeTexCoordsToGeo(
    const VCGMesh &in,
    GeoMesh &out,
    QString &error,
    const char *attributeName = "tex_coord");

// Reads back an index_t facet attribute, which is how mesh_segment and
// mesh_make_atlas report the chart each face landed in ("chart").
bool readFacetCharts(
    const ::GEO::Mesh &in,
    std::vector<int> &charts,
    QString &error,
    const char *attributeName = "chart");

} // namespace meshlab::geogram

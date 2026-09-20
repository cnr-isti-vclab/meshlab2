#pragma once

#include "vcgmesh.h"

#include <geogram/mesh/mesh.h>

#include <QMatrix4x4>
#include <QString>
#include <QStringList>
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

// `dimension` is 3 for ordinary use. Anisotropic remeshing needs 6, where
// coordinates 3-5 carry the scaled normals GEO::set_anisotropy writes.
bool meshToGeo(
    const VCGMesh &in,
    GeoMesh &out,
    QString &error,
    const QMatrix4x4 *transform = nullptr,
    int dimension = 3);

bool geoToMesh(const ::GEO::Mesh &in, VCGMesh &out, QString &error);

} // namespace meshlab::geogram

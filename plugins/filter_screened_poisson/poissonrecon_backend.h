#ifndef MESHLAB2_SCREENED_POISSON_POISSONRECON_BACKEND_H
#define MESHLAB2_SCREENED_POISSON_POISSONRECON_BACKEND_H

#include "meshfilterplugin.h"

#include <QString>
#include <QStringList>

#include <vcg/space/point3.h>

class Document;

namespace ScreenedPoisson
{

struct BackendStatus
{
    bool vendoredSourcesPresent = false;
    QString sourceRoot;
    QString summary;
    QStringList keyEntryPoints;
};

BackendStatus inspectBackend();
QString placeholderErrorMessage();
bool isEnabledByEnvironment();
MeshFilterRunResult runScreenedPoissonFilter(
    Document &doc,
    const std::vector<int> &meshIndices,
    bool mergeVisible,
    const MeshFilterParameterValues &parameters);
MeshFilterRunResult runSSDReconFilter(
    Document &doc,
    const std::vector<int> &meshIndices,
    bool mergeVisible,
    const MeshFilterParameterValues &parameters);
MeshFilterRunResult runSurfaceTrimmerFilter(
    Document &doc,
    int meshIndex,
    const MeshFilterParameterValues &parameters);

// Trims with an oriented plane instead of the mesh's own scalar, on the same code: the
// signed distance to the plane drives the split, and the per-vertex scalar rides along as
// an interpolated channel so the cut does not destroy it.
//
// `planeNormal` is resolved by the caller, which has a FilterParams to decode a point3f
// with; it need not be unit length. Where the plane sits along it comes from the
// `relativeTo` and `planeOffset` parameters.
MeshFilterRunResult runTrimSurfaceByPlaneFilter(
    Document &doc,
    int meshIndex,
    const vcg::Point3f &planeNormal,
    const MeshFilterParameterValues &parameters);

} // namespace ScreenedPoisson

#endif

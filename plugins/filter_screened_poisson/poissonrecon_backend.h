#ifndef MESHLAB2_SCREENED_POISSON_POISSONRECON_BACKEND_H
#define MESHLAB2_SCREENED_POISSON_POISSONRECON_BACKEND_H

#include "meshfilterplugin.h"

#include <QString>
#include <QStringList>

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

} // namespace ScreenedPoisson

#endif

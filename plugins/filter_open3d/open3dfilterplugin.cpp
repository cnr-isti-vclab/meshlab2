#include "open3dfilterplugin.h"

#include "meshfilterpluginmanager.h"
#include "open3dpointcloud.h"
#include "open3dregistration.h"
#include "open3dreconstruction.h"

#include <memory>

QString Open3DFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.open3d");
}

QString Open3DFilterPlugin::name() const
{
    return QObject::tr("Open3D Scan Processing");
}

MeshFilterRunResult Open3DFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (isOpen3DPointCloudFilter(filterId))
        return runOpen3DPointCloudFilter(filterId, params, doc);
    if (isOpen3DReconstructionFilter(filterId))
        return runOpen3DReconstructionFilter(filterId, params, doc);
    if (isOpen3DRegistrationFilter(filterId))
        return runOpen3DRegistrationFilter(filterId, params, doc);

    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = QObject::tr("Unknown filter id: %1").arg(filterId);
    return result;
}

void registerOpen3DFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<Open3DFilterPlugin>());
}

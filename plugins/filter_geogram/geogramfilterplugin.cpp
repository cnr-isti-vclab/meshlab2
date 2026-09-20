#include "geogramfilterplugin.h"

#include "geogramatlas.h"
#include "geogrambooleans.h"
#include "geogramparametrization.h"
#include "meshfilterpluginmanager.h"

#include <memory>

QString GeogramFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.geogram");
}

QString GeogramFilterPlugin::name() const
{
    return QObject::tr("geogram Filters");
}

MeshFilterRunResult GeogramFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (isGeogramBooleanFilter(filterId))
        return runGeogramBooleanFilter(filterId, params, doc);
    if (isGeogramParametrizationFilter(filterId))
        return runGeogramParametrizationFilter(filterId, params, doc);
    if (isGeogramAtlasFilter(filterId))
        return runGeogramAtlasFilter(filterId, params, doc);

    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = QObject::tr("Unknown filter id: %1").arg(filterId);
    return result;
}

void registerGeogramFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<GeogramFilterPlugin>());
}

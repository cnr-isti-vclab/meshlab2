#pragma once

#include "meshfilterplugin.h"

class MeshFilterPluginManager;

// All Open3D-backed filters. One optional dependency means one plugin, so this
// spans several categories - the categories, not the plugin, do the
// user-facing organizing.
class Open3DFilterPlugin final : public MeshFilterPlugin
{
public:
    QString pluginId() const override;
    QString name() const override;

    MeshFilterRunResult runFilter(
        const QString &filterId,
        const FilterParams &params,
        Document &doc) const override;
};

void registerOpen3DFilterPlugin(MeshFilterPluginManager &pluginManager);

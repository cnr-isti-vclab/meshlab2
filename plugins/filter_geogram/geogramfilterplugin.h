#pragma once

#include "meshfilterplugin.h"

class MeshFilterPluginManager;

// All geogram-backed filters. One optional dependency means one plugin, so
// this spans several categories (booleans now, parametrization, atlas and
// remeshing to come) - the categories, not the plugin, do the user-facing
// organizing.
class GeogramFilterPlugin final : public MeshFilterPlugin
{
public:
    QString pluginId() const override;
    QString name() const override;

    MeshFilterRunResult runFilter(
        const QString &filterId,
        const FilterParams &params,
        Document &doc) const override;
};

void registerGeogramFilterPlugin(MeshFilterPluginManager &pluginManager);

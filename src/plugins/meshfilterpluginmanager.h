#pragma once

#include "meshfilterplugin.h"
#include <QVariant>
#include <memory>
#include <optional>
#include <vector>

class Document;

class MeshFilterPluginManager
{
public:
    struct FilterInfo {
        QString key;
        QString pluginId;
        QString pluginName;
        MeshFilterDescriptor descriptor;
        bool applicable = true;
        QString applicabilityError;
    };

    void registerPlugin(std::unique_ptr<MeshFilterPlugin> plugin);

    std::vector<FilterInfo> filterInfos(const Document &doc) const;
    std::optional<FilterInfo> filterInfo(const QString &filterKey, const Document &doc) const;
    bool validateFilterInvocation(
        const QString &filterKey,
        const MeshFilterParameterValues &parameters,
        const Document &doc,
        QString &errorMessage) const;

    MeshFilterRunResult runFilter(
        const QString &filterKey,
        const MeshFilterParameterValues &parameters,
        Document &doc) const;

private:
    static QString buildFilterKey(const QString &pluginId, const QString &filterId);
    static bool splitFilterKey(
        const QString &filterKey,
        QString &pluginId,
        QString &filterId);

    bool validateDomain(
        const MeshFilterDescriptor &descriptor,
        const Document &doc,
        QString &errorMessage) const;
    bool normalizeAndValidateParameters(
        const MeshFilterDescriptor &descriptor,
        const MeshFilterParameterValues &inputParameters,
        const Document &doc,
        MeshFilterParameterValues &normalizedParameters,
        QString &errorMessage) const;
    static bool convertParameterValue(
        const MeshFilterParameterDescriptor &parameter,
        const QVariant &inputValue,
        QVariant &outputValue,
        QString &errorMessage);

    std::vector<std::unique_ptr<MeshFilterPlugin>> m_plugins;
};

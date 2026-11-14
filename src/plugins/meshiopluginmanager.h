#pragma once

#include "meshioplugin.h"
#include <memory>
#include <QHash>
#include <QStringList>
#include <vector>

// Registry of MeshIOPlugin instances.
// Selection honors user preference for the file extension first, then falls
// back to the first registered plugin that canLoad().
class MeshIOPluginManager
{
public:
    struct PluginInfo {
        QString id;
        QString name;
        QStringList extensions;
        QStringList saveExtensions;
        QHash<QString, MeshIOCapabilities> loadCapabilities;
        QHash<QString, MeshIOCapabilities> saveCapabilities;
    };

    MeshIOPluginManager();

    void registerPlugin(std::unique_ptr<MeshIOPlugin> plugin);

    // Returns preferred plugin for extension when valid, otherwise first
    // registered plugin that can handle filename, or nullptr.
    const MeshIOPlugin *pluginFor(const QString &filename) const;
    // Returns preferred plugin for extension when valid, otherwise first
    // registered plugin that can save filename, or nullptr.
    const MeshIOPlugin *pluginForSave(const QString &filename) const;

    // Builds the combined Qt file dialog filter from all registered plugins.
    QString openDialogFilter() const;
    // Builds the combined Qt file dialog filter for save.
    QString saveDialogFilter() const;

    // Returns plugin metadata for UI.
    std::vector<PluginInfo> pluginInfos() const;

    // Returns all supported extensions (lowercase, without dot), deduplicated.
    QStringList supportedExtensions() const;
    // Returns all save-supported extensions (lowercase, without dot), deduplicated.
    QStringList savableExtensions() const;

    // Preferred plugin id for a given extension (lowercase, without dot), empty if not set.
    QString preferredPluginForExtension(const QString &extension) const;

    // Set preferred plugin for extension. Empty pluginId removes preference.
    void setPreferredPluginForExtension(const QString &extension, const QString &pluginId);

private:
    std::vector<std::unique_ptr<MeshIOPlugin>> m_plugins;
    QStringList m_openableExtensions;
    QStringList m_saveableExtensions;
    QHash<QString, QString> m_preferredPluginByExtension;
};

#include "meshiopluginmanager.h"
#include <QFileInfo>
#include <QObject>
#include <QSettings>
#include <QStringList>

namespace {
QString normalizeExtension(const QString &extension)
{
    QString ext = extension.trimmed().toLower();
    if (ext.startsWith(QStringLiteral("*.")))
        ext = ext.mid(2);
    else if (ext.startsWith(QLatin1Char('.')))
        ext = ext.mid(1);
    return ext;
}

const QString kSettingsGroup = QStringLiteral("importPluginPreferences");
}

MeshIOPluginManager::MeshIOPluginManager()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    const QStringList keys = settings.childKeys();
    for (const QString &key : keys) {
        const QString normalizedExt = normalizeExtension(key);
        const QString preferredPluginId = settings.value(key).toString().trimmed();
        if (!normalizedExt.isEmpty() && !preferredPluginId.isEmpty())
            m_preferredPluginByExtension.insert(normalizedExt, preferredPluginId);
    }
    settings.endGroup();
}

void MeshIOPluginManager::registerPlugin(std::unique_ptr<MeshIOPlugin> plugin)
{
    if (!plugin)
        return;

    const QStringList pluginExtensions = plugin->supportedExtensions();
    for (const QString &rawExt : pluginExtensions) {
        const QString ext = normalizeExtension(rawExt);
        if (!ext.isEmpty()) {
            m_openableExtensions << ext;
            const QString probeFileName = QStringLiteral("dummy.%1").arg(ext);
            if (plugin->canSave(probeFileName))
                m_saveableExtensions << ext;
        }
    }
    m_openableExtensions.removeDuplicates();
    m_saveableExtensions.removeDuplicates();

    m_plugins.push_back(std::move(plugin));
}

const MeshIOPlugin *MeshIOPluginManager::pluginFor(const QString &filename) const
{
    const QString ext = normalizeExtension(QFileInfo(filename).suffix());
    const QString preferredPluginId = preferredPluginForExtension(ext);
    if (!preferredPluginId.isEmpty()) {
        for (const auto &plugin : m_plugins) {
            if (plugin->pluginId() == preferredPluginId && plugin->canLoad(filename))
                return plugin.get();
        }
    }

    for (const auto &plugin : m_plugins) {
        if (plugin->canLoad(filename))
            return plugin.get();
    }
    return nullptr;
}

const MeshIOPlugin *MeshIOPluginManager::pluginForSave(const QString &filename) const
{
    const QString ext = normalizeExtension(QFileInfo(filename).suffix());
    const QString preferredPluginId = preferredPluginForExtension(ext);
    if (!preferredPluginId.isEmpty()) {
        for (const auto &plugin : m_plugins) {
            if (plugin->pluginId() == preferredPluginId && plugin->canSave(filename))
                return plugin.get();
        }
    }

    for (const auto &plugin : m_plugins) {
        if (plugin->canSave(filename))
            return plugin.get();
    }
    return nullptr;
}

QString MeshIOPluginManager::openDialogFilter() const
{
    QStringList filters;

    if (!m_openableExtensions.isEmpty()) {
        QStringList wildcardExtensions;
        wildcardExtensions.reserve(m_openableExtensions.size());
        for (const QString &ext : m_openableExtensions)
            wildcardExtensions << QStringLiteral("*.%1").arg(ext);

        filters << QObject::tr("All Supported Mesh Files (%1)")
            .arg(wildcardExtensions.join(QLatin1Char(' ')));
    }

    for (const auto &plugin : m_plugins)
        filters << plugin->filterString();
    filters << QObject::tr("All Files (*)");
    return filters.join(QStringLiteral(";;"));
}

QString MeshIOPluginManager::saveDialogFilter() const
{
    QStringList filters;

    if (!m_saveableExtensions.isEmpty()) {
        QStringList wildcardExtensions;
        wildcardExtensions.reserve(m_saveableExtensions.size());
        for (const QString &ext : m_saveableExtensions)
            wildcardExtensions << QStringLiteral("*.%1").arg(ext);

        filters << QObject::tr("All Savable Mesh Files (%1)")
                       .arg(wildcardExtensions.join(QLatin1Char(' ')));
    }

    for (const auto &plugin : m_plugins) {
        const QString saveFilter = plugin->saveFilterString().trimmed();
        if (!saveFilter.isEmpty())
            filters << saveFilter;
    }
    filters << QObject::tr("All Files (*)");
    return filters.join(QStringLiteral(";;"));
}

std::vector<MeshIOPluginManager::PluginInfo> MeshIOPluginManager::pluginInfos() const
{
    std::vector<PluginInfo> infos;
    infos.reserve(m_plugins.size());
    for (const auto &plugin : m_plugins) {
        PluginInfo info;
        info.id = plugin->pluginId();
        info.name = plugin->name();

        const QStringList rawExts = plugin->supportedExtensions();
        for (const QString &rawExt : rawExts) {
            const QString ext = normalizeExtension(rawExt);
            if (!ext.isEmpty()) {
                info.extensions << ext;
                const QString probeFileName = QStringLiteral("dummy.%1").arg(ext);
                info.loadCapabilities.insert(ext, plugin->loadCapabilities(probeFileName));
                if (plugin->canSave(probeFileName)) {
                    info.saveExtensions << ext;
                    info.saveCapabilities.insert(ext, plugin->saveCapabilities(probeFileName));
                }
            }
        }
        info.extensions.removeDuplicates();
        info.saveExtensions.removeDuplicates();
        infos.push_back(std::move(info));
    }
    return infos;
}

QStringList MeshIOPluginManager::supportedExtensions() const
{
    return m_openableExtensions;
}

QStringList MeshIOPluginManager::savableExtensions() const
{
    return m_saveableExtensions;
}

QString MeshIOPluginManager::preferredPluginForExtension(const QString &extension) const
{
    const QString ext = normalizeExtension(extension);
    if (ext.isEmpty())
        return QString();
    return m_preferredPluginByExtension.value(ext);
}

void MeshIOPluginManager::setPreferredPluginForExtension(const QString &extension, const QString &pluginId)
{
    const QString ext = normalizeExtension(extension);
    if (ext.isEmpty())
        return;

    const QString trimmedPluginId = pluginId.trimmed();
    if (trimmedPluginId.isEmpty()) {
        m_preferredPluginByExtension.remove(ext);
    } else {
        bool pluginExists = false;
        bool pluginSupportsExtension = false;
        for (const auto &plugin : m_plugins) {
            if (plugin->pluginId() != trimmedPluginId)
                continue;
            pluginExists = true;
            const QStringList exts = plugin->supportedExtensions();
            for (const QString &rawExt : exts) {
                if (normalizeExtension(rawExt) == ext) {
                    pluginSupportsExtension = true;
                    break;
                }
            }
            break;
        }
        if (!pluginExists || !pluginSupportsExtension)
            return;
        m_preferredPluginByExtension.insert(ext, trimmedPluginId);
    }

    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    if (trimmedPluginId.isEmpty())
        settings.remove(ext);
    else
        settings.setValue(ext, trimmedPluginId);
    settings.endGroup();
}

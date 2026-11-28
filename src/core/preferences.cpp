#include "preferences.h"

#include "filterdescriptorloader.h"

#include <QDebug>
#include <QSettings>

namespace {

constexpr auto kResourcePath = ":/resources/preferences.json";
constexpr auto kSettingsGroup = "preferences";
constexpr auto kLoadOptionsGroup = "loadOptions";

// Settings that moved elsewhere, carried over once: the STL merge was a preference until it
// became io_vcg's load option (docs/design/proposals/load_options.md §5).
void migrateSettings()
{
    QSettings settings;
    const QString stlMerge = QStringLiteral("%1/document.mergeStlDuplicateVertices").arg(QString::fromLatin1(kSettingsGroup));
    if (!settings.contains(stlMerge))
        return;
    settings.setValue(QStringLiteral("%1/io_vcg/mergeDuplicateVertices").arg(QString::fromLatin1(kLoadOptionsGroup)),
        settings.value(stlMerge).toBool());
    settings.remove(stlMerge);
}

} // namespace

Preferences::Preferences() = default;

Preferences &Preferences::instance()
{
    static Preferences preferences;
    return preferences;
}

void Preferences::ensureLoaded() const
{
    if (m_loaded)
        return;
    m_loaded = true;
    migrateSettings();

    QString error;
    m_descriptors = FilterDescriptorLoader::loadParameters(QString::fromLatin1(kResourcePath), error);
    if (!error.isEmpty()) {
        qWarning() << "Preferences:" << error;
        return;
    }
    // Read on the frame path, so looked up by hash rather than by walking the list.
    for (int i = 0; i < int(m_descriptors.size()); ++i)
        m_descriptorIndex.insert(m_descriptors[size_t(i)].id, i);

    // Stored values are only adopted for ids that are still declared, so removing a
    // preference from the JSON does not leave a stale entry behind.
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    for (const auto &descriptor : m_descriptors) {
        if (!settings.contains(descriptor.id))
            continue;
        const QVariant stored = settings.value(descriptor.id);
        // A stored copy of the default, as the old Restore Defaults wrote, would pin
        // today's default against a later change to it; it says nothing, so drop it.
        if (sameParameterValue(stored, descriptor.defaultValue))
            settings.remove(descriptor.id);
        else
            m_values.insert(descriptor.id, stored);
    }
    settings.endGroup();
}

const std::vector<MeshFilterParameterDescriptor> &Preferences::descriptors() const
{
    ensureLoaded();
    return m_descriptors;
}

const MeshFilterParameterDescriptor *Preferences::descriptor(const QString &id) const
{
    ensureLoaded();
    const auto it = m_descriptorIndex.constFind(id);
    return it == m_descriptorIndex.constEnd() ? nullptr : &m_descriptors[size_t(it.value())];
}

QVariant Preferences::value(const QString &id) const
{
    ensureLoaded();
    const auto it = m_values.constFind(id);
    if (it != m_values.constEnd() && it.value().isValid())
        return it.value();
    if (const MeshFilterParameterDescriptor *d = descriptor(id))
        return d->defaultValue;
    qWarning() << "Preferences: unknown preference" << id;
    return {};
}

bool Preferences::boolValue(const QString &id) const { return value(id).toBool(); }
int Preferences::intValue(const QString &id) const { return value(id).toInt(); }
double Preferences::doubleValue(const QString &id) const { return value(id).toDouble(); }
QString Preferences::stringValue(const QString &id) const { return value(id).toString(); }

QColor Preferences::colorValue(const QString &id) const
{
    const QVariant v = value(id);
    return v.userType() == QMetaType::QColor ? v.value<QColor>() : QColor(v.toString());
}

void Preferences::setValue(const QString &id, const QVariant &newValue)
{
    ensureLoaded();
    const MeshFilterParameterDescriptor *d = descriptor(id);
    if (!d) {
        qWarning() << "Preferences: refusing to set undeclared preference" << id;
        return;
    }
    if (sameParameterValue(newValue, d->defaultValue)) {
        resetToDefault(id);
        return;
    }
    // Compare against the effective value, not the stored one: setting a preference
    // back to its default must still count as a change when it had been overridden.
    if (sameParameterValue(value(id), newValue))
        return;

    m_values.insert(id, newValue);

    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.setValue(id, newValue);
    settings.endGroup();

    emit changed(id, newValue);
}

void Preferences::setValues(const MeshFilterParameterValues &newValues)
{
    for (auto it = newValues.constBegin(); it != newValues.constEnd(); ++it)
        setValue(it.key(), it.value());
}

MeshFilterParameterValues Preferences::values() const
{
    ensureLoaded();
    MeshFilterParameterValues out;
    for (const auto &d : m_descriptors)
        out.insert(d.id, value(d.id));
    return out;
}

void Preferences::resetToDefault(const QString &id)
{
    ensureLoaded();
    const MeshFilterParameterDescriptor *d = descriptor(id);
    if (!d) {
        qWarning() << "Preferences: refusing to reset undeclared preference" << id;
        return;
    }
    const bool moves = !sameParameterValue(value(id), d->defaultValue);
    m_values.remove(id);
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.remove(id);
    settings.endGroup();
    if (moves)
        emit changed(id, d->defaultValue);
}

MeshFilterParameterValues Preferences::loadOptions(const QString &pluginId) const
{
    ensureLoaded();
    QSettings settings;
    settings.beginGroup(QStringLiteral("%1/%2").arg(QString::fromLatin1(kLoadOptionsGroup), pluginId));
    MeshFilterParameterValues values;
    for (const QString &key : settings.childKeys())
        values.insert(key, settings.value(key));
    settings.endGroup();
    return values;
}

void Preferences::setLoadOptions(const QString &pluginId, const MeshFilterParameterValues &values)
{
    ensureLoaded();
    QSettings settings;
    settings.beginGroup(QStringLiteral("%1/%2").arg(QString::fromLatin1(kLoadOptionsGroup), pluginId));
    settings.remove(QString());
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        settings.setValue(it.key(), it.value());
    settings.endGroup();
}

void Preferences::resetToDefaults()
{
    ensureLoaded();
    for (const auto &d : m_descriptors)
        resetToDefault(d.id);
}

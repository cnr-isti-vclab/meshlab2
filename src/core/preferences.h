#pragma once

#include "meshfilterplugin.h"

#include <QObject>
#include <QString>
#include <QVariant>

#include <vector>

// Application-wide settings, declared in resources/preferences.json with the same
// parameter schema as every plugin's filters.json. Adding a preference means adding
// a JSON entry and reading it back here — no UI code, because the same
// ParameterFormBuilder that renders the filter panel renders the dialog.
//
// Values live in QSettings under the "preferences" group and are written as soon as
// they change, so nothing is lost if the app does not exit cleanly. Reading a
// preference that has never been set returns the descriptor's default, which means
// call sites can be written as a plain replacement for the constant they had.
//
//   const int chunks = Preferences::instance().intValue("advanced.rayCallbackChunks");
//
// Guidance on what belongs here is in resources/preferences.json itself.
class Preferences : public QObject
{
    Q_OBJECT
public:
    static Preferences &instance();

    // Declared preferences, in file order. Feed straight to ParameterFormBuilder.
    const std::vector<MeshFilterParameterDescriptor> &descriptors() const;
    const MeshFilterParameterDescriptor *descriptor(const QString &id) const;

    // Current value, falling back to the declared default when unset or when the id
    // is unknown (so a typo degrades to a sane value rather than a null QVariant).
    QVariant value(const QString &id) const;
    bool boolValue(const QString &id) const;
    int intValue(const QString &id) const;
    double doubleValue(const QString &id) const;
    QString stringValue(const QString &id) const;

    // No-ops when the value is unchanged, so wiring a signal to a widget that echoes
    // it back cannot loop. Only a value that differs from the default is stored; setting
    // the default is the same as resetToDefault().
    void setValue(const QString &id, const QVariant &value);
    void setValues(const MeshFilterParameterValues &values);

    MeshFilterParameterValues values() const;
    // Forget the stored value, so the preference follows its declared default again --
    // including a later change to that default, which a stored copy of today's default
    // would have hidden. Emits changed() when the effective value moves.
    void resetToDefault(const QString &id);
    void resetToDefaults();

signals:
    // Emitted after the new value is stored and persisted.
    void changed(const QString &id, const QVariant &value);

private:
    Preferences();
    void ensureLoaded() const;

    mutable bool m_loaded = false;
    mutable std::vector<MeshFilterParameterDescriptor> m_descriptors;
    mutable MeshFilterParameterValues m_values;
};

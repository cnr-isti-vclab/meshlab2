// The preferences registry: descriptors come from resources/preferences.json using
// the filter parameter schema, values persist in QSettings, and every read falls
// back to the declared default so a call site reads like the constant it replaced.
#include "preferences.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTest>

class PreferencesTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void declaresParameters();
    void parameterlessFiltersRunImmediatelyByDefault();
    void unsetValuesFallBackToDefaults();
    void setValuePersistsAndSignals();
    void settingTheSameValueIsNotAChange();
    void unknownIdsAreRefused();
    void resetRestoresEveryDefault();
    void restoringOneDefaultForgetsTheStoredValue();
    void settingTheDefaultStoresNothing();
    void valuesCoverEveryDescriptor();
};

void PreferencesTests::initTestCase()
{
    // Keep the developer's real settings out of it.
    QCoreApplication::setOrganizationName(QStringLiteral("MeshLab2Test"));
    QCoreApplication::setApplicationName(QStringLiteral("PreferencesTests"));
}

void PreferencesTests::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    Preferences::instance().resetToDefaults();
}

void PreferencesTests::declaresParameters()
{
    const auto &descriptors = Preferences::instance().descriptors();
    QVERIFY2(!descriptors.empty(), "preferences.json failed to load from the resource");

    // Ids are namespaced by group and must be unique, since they double as the
    // QSettings keys.
    QSet<QString> ids;
    for (const auto &d : descriptors) {
        QVERIFY2(!d.id.trimmed().isEmpty(), "preference without an id");
        QVERIFY2(!ids.contains(d.id), qPrintable(QStringLiteral("duplicate id %1").arg(d.id)));
        QVERIFY2(d.defaultValue.isValid(), qPrintable(QStringLiteral("%1 has no default").arg(d.id)));
        ids.insert(d.id);
    }
}

// Pinned because flipping it changes what a click in the layer context menu does, and
// four of the five filters it governs are removals.
void PreferencesTests::parameterlessFiltersRunImmediatelyByDefault()
{
    const QString id = QStringLiteral("document.runParameterlessFiltersImmediately");
    const MeshFilterParameterDescriptor *d = Preferences::instance().descriptor(id);
    QVERIFY2(d != nullptr, "the preference is not declared");
    QCOMPARE(d->defaultValue.toBool(), true);
    QCOMPARE(Preferences::instance().boolValue(id), true);
}

void PreferencesTests::unsetValuesFallBackToDefaults()
{
    QSettings settings;
    settings.clear();
    settings.sync();

    Preferences &preferences = Preferences::instance();
    for (const auto &d : preferences.descriptors())
        QCOMPARE(preferences.value(d.id), d.defaultValue);
}

void PreferencesTests::setValuePersistsAndSignals()
{
    Preferences &preferences = Preferences::instance();
    const auto *descriptor = preferences.descriptor(QStringLiteral("view.axisGizmoSize"));
    QVERIFY(descriptor);
    const int changed = descriptor->defaultValue.toInt() + 17;

    QSignalSpy spy(&preferences, &Preferences::changed);
    preferences.setValue(QStringLiteral("view.axisGizmoSize"), changed);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("view.axisGizmoSize"));
    QCOMPARE(preferences.intValue(QStringLiteral("view.axisGizmoSize")), changed);

    // Written through immediately, so an unclean exit cannot lose it.
    QSettings settings;
    settings.beginGroup(QStringLiteral("preferences"));
    QCOMPARE(settings.value(QStringLiteral("view.axisGizmoSize")).toInt(), changed);
}

void PreferencesTests::settingTheSameValueIsNotAChange()
{
    Preferences &preferences = Preferences::instance();
    const QVariant current = preferences.value(QStringLiteral("input.dragThreshold"));

    QSignalSpy spy(&preferences, &Preferences::changed);
    preferences.setValue(QStringLiteral("input.dragThreshold"), current);
    // A consumer that echoes the value back into the registry must not loop.
    QCOMPARE(spy.count(), 0);
}

void PreferencesTests::unknownIdsAreRefused()
{
    Preferences &preferences = Preferences::instance();
    QSignalSpy spy(&preferences, &Preferences::changed);
    preferences.setValue(QStringLiteral("not.a.preference"), 5);
    QCOMPARE(spy.count(), 0);
    QVERIFY(!preferences.value(QStringLiteral("not.a.preference")).isValid());
}

void PreferencesTests::resetRestoresEveryDefault()
{
    Preferences &preferences = Preferences::instance();
    preferences.setValue(QStringLiteral("view.axisGizmoSize"), 200);
    preferences.setValue(QStringLiteral("input.dragThreshold"), 30);

    preferences.resetToDefaults();
    for (const auto &d : preferences.descriptors())
        QCOMPARE(preferences.value(d.id), d.defaultValue);
    // Forgotten, not written back as copies of today's defaults.
    QSettings settings;
    settings.beginGroup(QStringLiteral("preferences"));
    QVERIFY2(settings.childKeys().isEmpty(), qPrintable(settings.childKeys().join(QStringLiteral(", "))));
}

// Restoring one preference touches only that one, and forgets its stored value, so it
// follows the declared default from then on -- a stored copy of the default would keep
// the old value alive through a later change to it.
void PreferencesTests::restoringOneDefaultForgetsTheStoredValue()
{
    Preferences &preferences = Preferences::instance();
    const QString gizmo = QStringLiteral("view.axisGizmoSize");
    const QString drag = QStringLiteral("input.dragThreshold");
    preferences.setValue(gizmo, 200);
    preferences.setValue(drag, 30);

    QSignalSpy spy(&preferences, &Preferences::changed);
    preferences.resetToDefault(gizmo);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), gizmo);
    QCOMPARE(preferences.value(gizmo), preferences.descriptor(gizmo)->defaultValue);
    QCOMPARE(preferences.intValue(drag), 30);

    QSettings settings;
    settings.beginGroup(QStringLiteral("preferences"));
    QVERIFY(!settings.contains(gizmo));
    QCOMPARE(settings.value(drag).toInt(), 30);

    // Restoring what is already the default changes nothing and says nothing.
    preferences.resetToDefault(gizmo);
    QCOMPARE(spy.count(), 1);
}

// Choosing the default by hand is the same as restoring it: nothing is stored.
void PreferencesTests::settingTheDefaultStoresNothing()
{
    Preferences &preferences = Preferences::instance();
    const QString id = QStringLiteral("view.axisGizmoSize");
    const QVariant defaultValue = preferences.descriptor(id)->defaultValue;
    preferences.setValue(id, defaultValue.toInt() + 5);

    QSignalSpy spy(&preferences, &Preferences::changed);
    preferences.setValue(id, defaultValue.toInt());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(preferences.value(id).toInt(), defaultValue.toInt());

    QSettings settings;
    settings.beginGroup(QStringLiteral("preferences"));
    QVERIFY(!settings.contains(id));
}

// values() is what seeds the dialog's form, so it has to name every declared id.
void PreferencesTests::valuesCoverEveryDescriptor()
{
    Preferences &preferences = Preferences::instance();
    const MeshFilterParameterValues values = preferences.values();
    QCOMPARE(int(values.size()), int(preferences.descriptors().size()));
    for (const auto &d : preferences.descriptors())
        QVERIFY2(values.contains(d.id), qPrintable(d.id));
}

QTEST_MAIN(PreferencesTests)
#include "test_preferences.moc"

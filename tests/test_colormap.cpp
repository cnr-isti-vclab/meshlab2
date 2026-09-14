#include <QtTest/QtTest>

#include "colormap.h"
#include "src/render/qualityrange.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>

namespace {
constexpr const char *kEnvColorMapDirs = "MESHLAB2_COLORMAP_DIRS";

QString makeTestColormapJson(
    const QString &id,
    const QString &name,
    const QString &stopsJson)
{
    return QStringLiteral(
               "{\n"
               "  \"id\": \"%1\",\n"
               "  \"name\": \"%2\",\n"
               "  \"stops\": %3\n"
               "}\n")
        .arg(id, name, stopsJson);
}

bool writeTextFile(const QString &path, const QString &text)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    return f.write(text.toUtf8()) == text.toUtf8().size();
}
}

class ColorMapRegistryTests : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void bundledMapsAreAvailable();
    void externalMapIsLoadedFromEnvOverride();
    void externalMapCanOverrideBundledMap();
    void invalidExternalMapIsIgnored();
    void invalidExternalMapIsReported();
    void qualityRangeUsesPercentileCrop();
    void qualityNormalizationClampsToRange();
};

void ColorMapRegistryTests::init()
{
    qunsetenv(kEnvColorMapDirs);
    ColorMapRegistry::instance().reload();
}

void ColorMapRegistryTests::cleanup()
{
    qunsetenv(kEnvColorMapDirs);
    ColorMapRegistry::instance().reload();
}

void ColorMapRegistryTests::bundledMapsAreAvailable()
{
    ColorMapRegistry &registry = ColorMapRegistry::instance();
    const QStringList ids = registry.mapIds();
    QVERIFY(!ids.isEmpty());
    QVERIFY(ids.contains(QStringLiteral("rainbow")));
    QVERIFY(ids.contains(QStringLiteral("viridis")));
    QVERIFY(ids.contains(QStringLiteral("gray")));
    QCOMPARE(registry.fallbackMapId(), QStringLiteral("rainbow"));
}

void ColorMapRegistryTests::externalMapIsLoadedFromEnvOverride()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString path = QDir(tmp.path()).filePath(QStringLiteral("sunset.json"));
    QVERIFY(writeTextFile(
        path,
        makeTestColormapJson(
            QStringLiteral("sunset"),
            QStringLiteral("Sunset"),
            QStringLiteral(
                "[{\"x\":0.0,\"rgb\":[0,0,0]},"
                " {\"x\":1.0,\"rgb\":[255,128,0]}]"))));

    QVERIFY(qputenv(kEnvColorMapDirs, tmp.path().toUtf8()));
    ColorMapRegistry &registry = ColorMapRegistry::instance();
    registry.reload();

    QVERIFY(registry.hasMap(QStringLiteral("sunset")));
    QCOMPARE(registry.displayName(QStringLiteral("sunset")), QStringLiteral("Sunset"));

    const QVector3D c0 = registry.sampleRgb(QStringLiteral("sunset"), 0.0f);
    const QVector3D c1 = registry.sampleRgb(QStringLiteral("sunset"), 1.0f);
    QCOMPARE(c0, QVector3D(0.0f, 0.0f, 0.0f));
    QVERIFY(std::abs(c1.x() - 1.0f) < 1e-6f);
    QVERIFY(std::abs(c1.y() - (128.0f / 255.0f)) < 1e-6f);
    QVERIFY(std::abs(c1.z() - 0.0f) < 1e-6f);
}

void ColorMapRegistryTests::externalMapCanOverrideBundledMap()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString path = QDir(tmp.path()).filePath(QStringLiteral("rainbow_override.json"));
    QVERIFY(writeTextFile(
        path,
        makeTestColormapJson(
            QStringLiteral("rainbow"),
            QStringLiteral("Rainbow Override"),
            QStringLiteral(
                "[{\"x\":0.0,\"rgb\":[0,0,0]},"
                " {\"x\":1.0,\"rgb\":[255,255,255]}]"))));

    QVERIFY(qputenv(kEnvColorMapDirs, tmp.path().toUtf8()));
    ColorMapRegistry &registry = ColorMapRegistry::instance();
    registry.reload();

    QCOMPARE(registry.displayName(QStringLiteral("rainbow")), QStringLiteral("Rainbow Override"));
    const QVector3D mid = registry.sampleRgb(QStringLiteral("rainbow"), 0.5f);
    QVERIFY(std::abs(mid.x() - 0.5f) < 0.02f);
    QVERIFY(std::abs(mid.y() - 0.5f) < 0.02f);
    QVERIFY(std::abs(mid.z() - 0.5f) < 0.02f);
}

void ColorMapRegistryTests::invalidExternalMapIsIgnored()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString path = QDir(tmp.path()).filePath(QStringLiteral("broken.json"));
    QVERIFY(writeTextFile(
        path,
        makeTestColormapJson(
            QStringLiteral("broken"),
            QStringLiteral("Broken"),
            QStringLiteral("[{\"x\":0.0,\"rgb\":[0,0,0]}]")))); // only one stop => invalid

    QVERIFY(qputenv(kEnvColorMapDirs, tmp.path().toUtf8()));
    ColorMapRegistry &registry = ColorMapRegistry::instance();
    registry.reload();

    QVERIFY(!registry.hasMap(QStringLiteral("broken")));
    QVERIFY(registry.hasMap(registry.fallbackMapId()));
}

// Ignoring a bad map silently was the bug: the user drops their own colormap in the
// folder, mistypes it, and nothing anywhere says so. The issue is queued for whoever
// owns a log.
void ColorMapRegistryTests::invalidExternalMapIsReported()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString path = QDir(tmp.path()).filePath(QStringLiteral("broken.json"));
    QVERIFY(writeTextFile(
        path,
        makeTestColormapJson(
            QStringLiteral("broken"),
            QStringLiteral("Broken"),
            QStringLiteral("[{\"x\":0.0,\"rgb\":[0,0,0]}]"))));

    QVERIFY(qputenv(kEnvColorMapDirs, tmp.path().toUtf8()));
    ColorMapRegistry &registry = ColorMapRegistry::instance();
    registry.reload();

    const QVector<ColorMapLoadIssue> issues = registry.takeLoadIssues();
    QCOMPARE(issues.size(), 1);
    QVERIFY(!issues.front().bundled);
    QVERIFY2(issues.front().message.contains(path), qPrintable(issues.front().message));

    // Drained, so a second reader does not report the same problem again.
    QVERIFY(registry.takeLoadIssues().isEmpty());
}

void ColorMapRegistryTests::qualityRangeUsesPercentileCrop()
{
    const RenderQualityRange range = sampledRenderQualityRange(
        { -100.0f, 0.0f, 1.0f, 2.0f, 100.0f },
        false,
        0.25f);

    QVERIFY(range.valid);
    QCOMPARE(range.minV, 0.0f);
    QCOMPARE(range.maxV, 2.0f);
}

void ColorMapRegistryTests::qualityNormalizationClampsToRange()
{
    const RenderQualityRange range = fixedRenderQualityRange(10.0f, 20.0f);

    QCOMPARE(normalizedRenderQuality(5.0f, range), 0.0f);
    QCOMPARE(normalizedRenderQuality(25.0f, range), 1.0f);
    QCOMPARE(normalizedRenderQuality(15.0f, range), 0.5f);
}

QTEST_MAIN(ColorMapRegistryTests)
#include "test_colormap.moc"

#include "colormap.h"

#include <QCoreApplication>
#include <QObject>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>
#include <cmath>

namespace {

QString stopPathJoin(const QString &base, const QString &fileName)
{
    if (base.endsWith('/'))
        return base + fileName;
    return base + QLatin1Char('/') + fileName;
}

bool parseFiniteFloat(const QJsonValue &value, float &outValue)
{
    if (!value.isDouble())
        return false;
    outValue = static_cast<float>(value.toDouble());
    return std::isfinite(outValue);
}

bool normalizeRgbComponent(float &v)
{
    if (!std::isfinite(v))
        return false;
    if (v > 1.0f)
        v /= 255.0f;
    v = std::clamp(v, 0.0f, 1.0f);
    return true;
}

bool parseRgbArray(const QJsonValue &value, QVector3D &outRgb)
{
    if (!value.isArray())
        return false;
    const QJsonArray arr = value.toArray();
    if (arr.size() != 3)
        return false;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (!parseFiniteFloat(arr.at(0), r) || !parseFiniteFloat(arr.at(1), g)
        || !parseFiniteFloat(arr.at(2), b)) {
        return false;
    }
    if (!normalizeRgbComponent(r) || !normalizeRgbComponent(g) || !normalizeRgbComponent(b))
        return false;
    outRgb = QVector3D(r, g, b);
    return true;
}

bool parseRgbObject(const QJsonObject &obj, QVector3D &outRgb)
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (!parseFiniteFloat(obj.value(QStringLiteral("r")), r)
        || !parseFiniteFloat(obj.value(QStringLiteral("g")), g)
        || !parseFiniteFloat(obj.value(QStringLiteral("b")), b)) {
        return false;
    }
    if (!normalizeRgbComponent(r) || !normalizeRgbComponent(g) || !normalizeRgbComponent(b))
        return false;
    outRgb = QVector3D(r, g, b);
    return true;
}

} // namespace

ColorMapRegistry &ColorMapRegistry::instance()
{
    static ColorMapRegistry registry;
    return registry;
}

void ColorMapRegistry::reload()
{
    m_loaded = false;
    m_fallbackMapId.clear();
    m_order.clear();
    m_maps.clear();
}

QString ColorMapRegistry::fallbackMapId() const
{
    ensureLoaded();
    return m_fallbackMapId;
}

QStringList ColorMapRegistry::mapIds() const
{
    ensureLoaded();
    return m_order;
}

QString ColorMapRegistry::displayName(const QString &mapId) const
{
    ensureLoaded();
    const QString key = normalizedMapId(mapId);
    const auto it = m_maps.constFind(key);
    if (it == m_maps.cend())
        return QString();
    return it->name;
}

bool ColorMapRegistry::hasMap(const QString &mapId) const
{
    ensureLoaded();
    return m_maps.contains(normalizedMapId(mapId));
}

const ColorMapDefinition *ColorMapRegistry::definition(const QString &mapId) const
{
    ensureLoaded();
    const QString key = normalizedMapId(mapId);
    auto it = m_maps.constFind(key);
    if (it == m_maps.cend() && !m_fallbackMapId.isEmpty())
        it = m_maps.constFind(m_fallbackMapId);
    if (it == m_maps.cend() && !m_order.isEmpty())
        it = m_maps.constFind(m_order.front());
    if (it == m_maps.cend())
        return nullptr;
    return &it.value();
}

QVector3D ColorMapRegistry::sampleRgb(const ColorMapDefinition *definition, float t) const
{
    if (!definition)
        return QVector3D(1.0f, 1.0f, 1.0f);
    return interpolateStops(definition->stops, t);
}

QVector3D ColorMapRegistry::sampleRgb(const QString &mapId, float t) const
{
    return sampleRgb(definition(mapId), t);
}

QColor ColorMapRegistry::sampleQColor(
    const ColorMapDefinition *definition, float t, float alpha) const
{
    const QVector3D rgb = sampleRgb(definition, t);
    QColor color;
    color.setRgbF(rgb.x(), rgb.y(), rgb.z(), std::clamp(alpha, 0.0f, 1.0f));
    return color;
}

QColor ColorMapRegistry::sampleQColor(const QString &mapId, float t, float alpha) const
{
    return sampleQColor(definition(mapId), t, alpha);
}

void ColorMapRegistry::ensureLoaded() const
{
    if (m_loaded)
        return;
    const_cast<ColorMapRegistry *>(this)->loadInternal();
}

void ColorMapRegistry::loadInternal()
{
    m_maps.clear();
    m_order.clear();
    m_fallbackMapId.clear();
    m_loadIssues.clear();

    loadBundledResourceMaps();
    loadExternalFolderMaps();
    ensureFallbackMap();

    m_loaded = true;
}

QVector<ColorMapLoadIssue> ColorMapRegistry::takeLoadIssues()
{
    ensureLoaded();
    return std::exchange(m_loadIssues, {});
}

// Still goes to the terminal: a headless run (tests, --generate-docs) has no log panel,
// and that is exactly where a broken color map is easiest to overlook.
void ColorMapRegistry::recordIssue(bool bundled, const QString &message)
{
    qWarning("ColorMapRegistry: %s", qPrintable(message));
    m_loadIssues.push_back(ColorMapLoadIssue{ message, bundled });
}

void ColorMapRegistry::loadBundledResourceMaps()
{
    const QDir resDir(QStringLiteral(":/colormaps"));
    const QStringList files =
        resDir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    for (const QString &fileName : files) {
        const QString path = stopPathJoin(QStringLiteral(":/colormaps"), fileName);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            recordIssue(true, QObject::tr("Built-in color map '%1' could not be opened.")
                                  .arg(fileName));
            continue;
        }
        const QByteArray bytes = file.readAll();
        ColorMapDefinition map;
        QString error;
        if (!parseMapFile(path, bytes, map, error)) {
            recordIssue(true, QObject::tr("Built-in color map '%1' is not valid: %2")
                                  .arg(fileName, error));
            continue;
        }
        if (!registerMap(std::move(map), false, error)) {
            recordIssue(true, QObject::tr("Built-in color map '%1' could not be registered: %2")
                                  .arg(fileName, error));
        }
    }
}

void ColorMapRegistry::loadExternalFolderMaps()
{
    QStringList candidateDirs;
    const QByteArray envDirs = qgetenv("MESHLAB2_COLORMAP_DIRS");
    if (!envDirs.isEmpty()) {
        const QStringList overrideDirs = QString::fromLocal8Bit(envDirs).split(
            QDir::listSeparator(),
            Qt::SkipEmptyParts);
        for (QString dirPath : overrideDirs) {
            dirPath = QDir::fromNativeSeparators(dirPath.trimmed());
            if (!dirPath.isEmpty())
                candidateDirs.push_back(dirPath);
        }
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        candidateDirs.push_back(stopPathJoin(appDir, QStringLiteral("colormaps")));

    const QString appConfigDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!appConfigDir.isEmpty())
        candidateDirs.push_back(stopPathJoin(appConfigDir, QStringLiteral("colormaps")));

    QSet<QString> visited;
    for (const QString &dirPath : candidateDirs) {
        const QString canonicalKey = QDir(dirPath).absolutePath();
        if (visited.contains(canonicalKey))
            continue;
        visited.insert(canonicalKey);

        const QDir dir(dirPath);
        if (!dir.exists())
            continue;

        const QStringList files =
            dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
        for (const QString &fileName : files) {
            const QString path = dir.absoluteFilePath(fileName);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                recordIssue(false, QObject::tr("Color map '%1' could not be opened and was ignored.")
                                       .arg(path));
                continue;
            }
            const QByteArray bytes = file.readAll();
            ColorMapDefinition map;
            QString error;
            if (!parseMapFile(path, bytes, map, error)) {
                recordIssue(false, QObject::tr("Color map '%1' is not valid and was ignored: %2")
                                       .arg(path, error));
                continue;
            }
            if (!registerMap(std::move(map), true, error)) {
                recordIssue(false, QObject::tr("Color map '%1' could not be registered and was ignored: %2")
                                       .arg(path, error));
            }
        }
    }
}

void ColorMapRegistry::ensureFallbackMap()
{
    if (!m_maps.isEmpty()) {
        if (m_fallbackMapId.isEmpty()) {
            if (m_maps.contains(QStringLiteral("rainbow")))
                m_fallbackMapId = QStringLiteral("rainbow");
            else if (!m_order.isEmpty())
                m_fallbackMapId = m_order.front();
        }
        return;
    }

    ColorMapDefinition gray;
    gray.id = QStringLiteral("gray");
    gray.name = QStringLiteral("Gray");
    gray.stops = {
        { 0.0f, QVector3D(0.0f, 0.0f, 0.0f) },
        { 1.0f, QVector3D(1.0f, 1.0f, 1.0f) }
    };
    QString error;
    registerMap(std::move(gray), true, error);
    m_fallbackMapId = QStringLiteral("gray");
}

bool ColorMapRegistry::parseMapFile(
    const QString &path,
    const QByteArray &bytes,
    ColorMapDefinition &outMap,
    QString &error) const
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("json parse error at offset %1: %2")
                    .arg(parseError.offset)
                    .arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QString id = root.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty()) {
        error = QStringLiteral("missing non-empty 'id'");
        return false;
    }
    const QString name = root.value(QStringLiteral("name")).toString().trimmed();
    const QJsonValue stopsValue = root.value(QStringLiteral("stops"));
    if (!stopsValue.isArray()) {
        error = QStringLiteral("missing 'stops' array");
        return false;
    }
    const QJsonArray stopsArray = stopsValue.toArray();
    if (stopsArray.size() < 2) {
        error = QStringLiteral("'stops' must contain at least two entries");
        return false;
    }

    QVector<ColorMapStop> stops;
    stops.reserve(stopsArray.size());
    for (int i = 0; i < stopsArray.size(); ++i) {
        const QJsonValue stopValue = stopsArray.at(i);
        if (!stopValue.isObject()) {
            error = QStringLiteral("stop %1 is not an object").arg(i);
            return false;
        }
        const QJsonObject stopObj = stopValue.toObject();
        float x = 0.0f;
        if (!parseFiniteFloat(stopObj.value(QStringLiteral("x")), x)) {
            error = QStringLiteral("stop %1 has invalid 'x'").arg(i);
            return false;
        }
        x = std::clamp(x, 0.0f, 1.0f);

        QVector3D rgb(1.0f, 1.0f, 1.0f);
        const QJsonValue rgbValue = stopObj.value(QStringLiteral("rgb"));
        const bool hasRgbArray = parseRgbArray(rgbValue, rgb);
        const bool hasRgbObject = parseRgbObject(stopObj, rgb);
        if (!hasRgbArray && !hasRgbObject) {
            error = QStringLiteral("stop %1 has invalid color (expected 'rgb' array or r/g/b)").arg(i);
            return false;
        }

        stops.push_back({ x, rgb });
    }

    std::sort(stops.begin(), stops.end(), [](const ColorMapStop &a, const ColorMapStop &b) {
        return a.x < b.x;
    });

    for (int i = 1; i < stops.size(); ++i) {
        if (stops[i].x <= stops[i - 1].x) {
            error = QStringLiteral("stops must be strictly increasing after normalization");
            return false;
        }
    }

    outMap.id = id;
    outMap.name = name.isEmpty() ? id : name;
    outMap.stops = std::move(stops);
    Q_UNUSED(path);
    return true;
}

bool ColorMapRegistry::registerMap(ColorMapDefinition map, bool allowReplace, QString &error)
{
    const QString key = normalizedMapId(map.id);
    if (key.isEmpty()) {
        error = QStringLiteral("invalid id");
        return false;
    }
    if (map.stops.size() < 2) {
        error = QStringLiteral("map must contain at least two stops");
        return false;
    }

    if (!m_maps.contains(key))
        m_order.push_back(key);
    else if (!allowReplace) {
        error = QStringLiteral("duplicate id '%1'").arg(key);
        return false;
    }

    map.id = key;
    if (map.name.trimmed().isEmpty())
        map.name = key;
    m_maps.insert(key, std::move(map));

    if (m_fallbackMapId.isEmpty() || key == QStringLiteral("rainbow"))
        m_fallbackMapId = key;

    return true;
}

QVector3D ColorMapRegistry::interpolateStops(const QVector<ColorMapStop> &stops, float t)
{
    if (stops.isEmpty())
        return QVector3D(1.0f, 1.0f, 1.0f);

    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= stops.front().x)
        return stops.front().rgb;
    if (t >= stops.back().x)
        return stops.back().rgb;

    for (int i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].x) {
            const float x0 = stops[i - 1].x;
            const float x1 = stops[i].x;
            const float u = (x1 > x0) ? ((t - x0) / (x1 - x0)) : 0.0f;
            return stops[i - 1].rgb * (1.0f - u) + stops[i].rgb * u;
        }
    }
    return stops.back().rgb;
}

QString ColorMapRegistry::normalizedMapId(const QString &id)
{
    QString key = id.trimmed().toLower();
    if (key.isEmpty())
        return QString();
    key.replace(QLatin1Char(' '), QLatin1Char('-'));
    QString cleaned;
    cleaned.reserve(key.size());
    for (QChar ch : key) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('-')
            || ch == QLatin1Char('_')) {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

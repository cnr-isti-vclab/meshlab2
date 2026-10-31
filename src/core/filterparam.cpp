#include "filterparam.h"

#include <QObject>
#include <QRandomGenerator>
#include <QVector3D>

#include <limits>

FilterParams::FilterParams(const MeshFilterParameterValues &values)
    : m_values(values)
{
}

QString RandomSeed::message() const
{
    return generated
        ? QObject::tr("Random seed: %1 (generated — set randomSeed to this value to reproduce)")
              .arg(value)
        : QObject::tr("Random seed: %1").arg(value);
}

RandomSeed FilterParams::getRandomSeed(const QString &id) const
{
    RandomSeed seed;
    const int requested = getInt(id, 0);
    // Negative values cannot come from the UI (the declared minimum is 0) but can
    // from a script; treat them like 0 rather than wrapping into a huge unsigned.
    seed.generated = requested <= 0;
    // QRandomGenerator rather than time(0): a script running two randomized
    // filters back to back, or one filter that seeds two generators, would
    // otherwise get the same seed twice within the same second.
    seed.value = seed.generated
        ? QRandomGenerator::global()->bounded(1u, unsigned(std::numeric_limits<int>::max()))
        : unsigned(requested);
    return seed;
}

bool FilterParams::getBool(const QString &id) const
{
    return getBool(id, false);
}

int FilterParams::getInt(const QString &id) const
{
    return getInt(id, 0);
}

int FilterParams::getMesh(const QString &id) const
{
    return getMesh(id, -1);
}

double FilterParams::getDouble(const QString &id) const
{
    return getDouble(id, 0.0);
}

QString FilterParams::getString(const QString &id) const
{
    return getString(id, QString());
}

QString FilterParams::getFileOpen(const QString &id) const
{
    return getFileOpen(id, QString());
}

QString FilterParams::getFileSave(const QString &id) const
{
    return getFileSave(id, QString());
}

int FilterParams::getTextureRef(const QString &id) const
{
    return getTextureRef(id, 0);
}

TextureOutputRefValue FilterParams::getTextureOutputRef(const QString &id) const
{
    return getTextureOutputRef(id, {});
}

QString FilterParams::getEnum(const QString &id) const
{
    return getEnum(id, QString());
}

QColor FilterParams::getColor(const QString &id) const
{
    return getColor(id, QColor(Qt::white));
}

QVector3D FilterParams::getPoint3f(const QString &id) const
{
    return getPoint3f(id, QVector3D(0.0f, 0.0f, 0.0f));
}

QString FilterParams::getCameraState(const QString &id) const
{
    return getCameraState(id, QString());
}

QString FilterParams::getRenderState(const QString &id) const
{
    return getRenderState(id, QString());
}

bool FilterParams::getBool(const QString &id, bool fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;
    if (it.value().userType() == QMetaType::Bool)
        return it.value().toBool();
    const QString text = it.value().toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1"))
        return true;
    if (text == QStringLiteral("false") || text == QStringLiteral("0"))
        return false;
    return fallback;
}

int FilterParams::getInt(const QString &id, int fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;
    bool ok = false;
    const int v = it.value().toInt(&ok);
    return ok ? v : fallback;
}

int FilterParams::getMesh(const QString &id, int fallback) const
{
    return getInt(id, fallback);
}

double FilterParams::getDouble(const QString &id, double fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;
    bool ok = false;
    const double v = it.value().toDouble(&ok);
    return ok ? v : fallback;
}

QString FilterParams::getString(const QString &id, const QString &fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;
    return it.value().toString();
}

QString FilterParams::getFileOpen(const QString &id, const QString &fallback) const
{
    return getString(id, fallback);
}

QString FilterParams::getFileSave(const QString &id, const QString &fallback) const
{
    return getString(id, fallback);
}

int FilterParams::getTextureRef(const QString &id, int fallback) const
{
    return getInt(id, fallback);
}

TextureOutputRefValue FilterParams::getTextureOutputRef(
    const QString &id,
    const TextureOutputRefValue &fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;

    TextureOutputRefValue out = fallback;
    const QVariantMap map = it.value().toMap();
    if (map.isEmpty()) {
        out.overwriteExisting = false;
        out.textureSlot = -1;
        out.name = it.value().toString().trimmed();
        return out;
    }

    const QString mode = map.value(QStringLiteral("mode")).toString().trimmed().toLower();
    if (mode == QStringLiteral("existing")) {
        bool ok = false;
        const int oneBasedSlot = map.value(QStringLiteral("slot")).toInt(&ok);
        if (ok && oneBasedSlot > 0) {
            out.overwriteExisting = true;
            out.textureSlot = oneBasedSlot - 1;
            out.name.clear();
            return out;
        }
    }

    out.overwriteExisting = false;
    out.textureSlot = -1;
    out.name = map.value(QStringLiteral("name")).toString().trimmed();
    return out;
}

QString FilterParams::getEnum(const QString &id, const QString &fallback) const
{
    return getString(id, fallback);
}

QColor FilterParams::getColor(const QString &id, const QColor &fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;
    if (it.value().userType() == QMetaType::QColor)
        return it.value().value<QColor>();
    const QColor c(it.value().toString().trimmed());
    return c.isValid() ? c : fallback;
}

QVector3D FilterParams::getPoint3f(const QString &id, const QVector3D &fallback) const
{
    const auto it = m_values.constFind(id);
    if (it == m_values.constEnd())
        return fallback;
    if (it.value().userType() == QMetaType::QVector3D)
        return it.value().value<QVector3D>();
    // Accept "x,y,z" string encoding (used in tests / scripting)
    const QStringList parts = it.value().toString().trimmed().split(QLatin1Char(','));
    if (parts.size() == 3) {
        bool okX = false, okY = false, okZ = false;
        const float x = parts[0].trimmed().toFloat(&okX);
        const float y = parts[1].trimmed().toFloat(&okY);
        const float z = parts[2].trimmed().toFloat(&okZ);
        if (okX && okY && okZ)
            return QVector3D(x, y, z);
    }
    return fallback;
}

QString FilterParams::getCameraState(const QString &id, const QString &fallback) const
{
    return getString(id, fallback);
}

QString FilterParams::getRenderState(const QString &id, const QString &fallback) const
{
    return getString(id, fallback);
}

QString filterParamValueToPythonLiteral(const QVariant &v,
                                         MeshFilterParameterType type)
{
    switch (type) {
    case MeshFilterParameterType::Bool:
        return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case MeshFilterParameterType::Int:
    case MeshFilterParameterType::Mesh:
    case MeshFilterParameterType::TextureRef:
        return QString::number(v.toInt());
    case MeshFilterParameterType::Double:
    case MeshFilterParameterType::AbsPerc:
        return QString::number(v.toDouble(), 'g', 10);
    case MeshFilterParameterType::Color: {
        QColor c = v.value<QColor>();
        return QStringLiteral("[%1, %2, %3, %4]")
            .arg(c.redF(), 0, 'g', 10).arg(c.greenF(), 0, 'g', 10)
            .arg(c.blueF(), 0, 'g', 10).arg(c.alphaF(), 0, 'g', 10);
    }
    case MeshFilterParameterType::Point3f: {
        QVector3D p = v.value<QVector3D>();
        return QStringLiteral("[%1, %2, %3]")
            .arg(p.x(), 0, 'g', 10).arg(p.y(), 0, 'g', 10).arg(p.z(), 0, 'g', 10);
    }
    default:
        // String, FileOpen, FileSave, Enum, CameraState, RenderState, etc.
        return QStringLiteral("\"%1\"").arg(
            v.toString().replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
                         .replace(QLatin1Char('"'),  QStringLiteral("\\\"")));
    }
}

QStringList nonDefaultFilterParamsToPython(
    const MeshFilterDescriptor &descriptor,
    const MeshFilterParameterValues &values)
{
    QStringList args;
    for (const MeshFilterParameterDescriptor &param : descriptor.parameters) {
        if (param.id.trimmed().isEmpty()) continue;
        auto it = values.constFind(param.id);
        if (it == values.constEnd()) continue;
        // Skip parameters whose current value equals the default
        if (param.defaultValue.isValid() && it.value() == param.defaultValue)
            continue;
        QString lit = filterParamValueToPythonLiteral(it.value(), param.type);
        args << QStringLiteral("%1=%2").arg(param.id, lit);
    }
    return args;
}

QStringList allFilterParamsToPython(
    const MeshFilterDescriptor &descriptor,
    const MeshFilterParameterValues &values)
{
    QStringList args;
    for (const MeshFilterParameterDescriptor &param : descriptor.parameters) {
        if (param.id.trimmed().isEmpty())
            continue;
        auto it = values.constFind(param.id);
        if (it == values.constEnd())
            continue;
        const QString lit = filterParamValueToPythonLiteral(it.value(), param.type);
        args << QStringLiteral("%1=%2").arg(param.id, lit);
    }
    return args;
}

QString filterCallToPython(
    const MeshFilterDescriptor &descriptor,
    const MeshFilterParameterValues &values,
    bool includeDefaultParameters)
{
    const QStringList args = includeDefaultParameters
        ? allFilterParamsToPython(descriptor, values)
        : nonDefaultFilterParamsToPython(descriptor, values);
    return QStringLiteral("ms.%1(%2)").arg(
        descriptor.effectivePythonName(),
        args.join(QStringLiteral(", ")));
}

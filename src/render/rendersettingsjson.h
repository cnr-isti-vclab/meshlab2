#pragma once

#include "renderingsettings.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cmath>

// JSON serialisation for the render settings structs.
//
// Lives in MeshLab2Core rather than next to RenderWidget so it can be tested
// directly (RenderWidget is in the app executable and cannot be linked from a test)
// and so non-UI code can round-trip a render state. Same reasoning as
// ViewTrackball::stateFromJson.
//
// Passing `defaults` writes only the fields that differ from it, which is what keeps
// stored render states small; passing nullptr writes every field, which is what the
// round-trip test relies on.

namespace RenderSettingsJson {

bool fuzzyFloatEqual(float a, float b, float eps = 1e-6f);
bool parseFloatValue(const QJsonValue &value, float &outValue);
QJsonArray colorToJsonArray(const QColor &c);
bool parseColorArray(const QJsonValue &value, QColor &outValue);

template <typename EnumT>
int enumToInt(EnumT e)
{
    return static_cast<int>(e);
}

template <typename EnumT>
bool parseEnumInt(const QJsonObject &obj, const QString &key, EnumT &outValue)
{
    if (!obj.contains(key))
        return true;
    const QJsonValue value = obj.value(key);
    if (!value.isDouble())
        return false;
    outValue = static_cast<EnumT>(value.toInt());
    return true;
}

QJsonObject globalSettingsToJson(
    const GlobalRenderSettings &s,
    const GlobalRenderSettings *defaults = nullptr);
bool parseGlobalSettings(const QJsonObject &obj, GlobalRenderSettings &out, QString *error);

QJsonObject perMeshSettingsToJson(
    const PerMeshRenderSettings &s,
    const PerMeshRenderSettings *defaults = nullptr);
bool parsePerMeshSettings(const QJsonObject &obj, PerMeshRenderSettings &out, QString *error);

// Number of keys each *_ToJson emits when `defaults` is null, i.e. one per declared
// field. The round-trip test pins these so that adding a field to the struct without
// adding it to the serialiser fails a test instead of silently dropping out of every
// saved render state. Update alongside the struct.
constexpr int kGlobalSettingsFieldCount = 41;
constexpr int kPerMeshSettingsFieldCount = 48;

} // namespace RenderSettingsJson

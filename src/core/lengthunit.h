#pragma once

#include <QCoreApplication>
#include <QString>

#include <array>

// The unit a document's coordinates are measured in (Document::unit()). Most mesh formats do not
// say, so a document starts with Unknown; the user sets it, and from then on the lengths the
// program shows can be named in it. It labels the coordinates and does not change them.
enum class LengthUnit { Unknown, Micrometer, Millimeter, Centimeter, Meter, Kilometer, Inch, Foot };

namespace LengthUnits {

struct Info {
    LengthUnit unit;
    const char *symbol;   // what the interface shows and a project file stores
    const char *name;
    double meters;        // how many meters one of it is
};

// The units that can be chosen, smallest first; Unknown is the absence of a choice. The
// symbols are UTF-8 ("\xC2\xB5m" is the micro sign and an m).
inline const std::array<Info, 7> &all()
{
    static const std::array<Info, 7> table{ {
        { LengthUnit::Micrometer, "\xC2\xB5m", QT_TRANSLATE_NOOP("LengthUnit", "Micrometers"), 1e-6 },
        { LengthUnit::Millimeter, "mm", QT_TRANSLATE_NOOP("LengthUnit", "Millimeters"), 1e-3 },
        { LengthUnit::Centimeter, "cm", QT_TRANSLATE_NOOP("LengthUnit", "Centimeters"), 1e-2 },
        { LengthUnit::Meter, "m", QT_TRANSLATE_NOOP("LengthUnit", "Meters"), 1.0 },
        { LengthUnit::Kilometer, "km", QT_TRANSLATE_NOOP("LengthUnit", "Kilometers"), 1e3 },
        { LengthUnit::Inch, "in", QT_TRANSLATE_NOOP("LengthUnit", "Inches"), 0.0254 },
        { LengthUnit::Foot, "ft", QT_TRANSLATE_NOOP("LengthUnit", "Feet"), 0.3048 },
    } };
    return table;
}

// "mm", "m", ...; empty for Unknown.
inline QString symbol(LengthUnit unit)
{
    for (const Info &info : all()) {
        if (info.unit == unit)
            return QString::fromUtf8(info.symbol);
    }
    return {};
}

// "Millimeters", ... in the interface's language; empty for Unknown.
inline QString name(LengthUnit unit)
{
    for (const Info &info : all()) {
        if (info.unit == unit)
            return QCoreApplication::translate("LengthUnit", info.name);
    }
    return {};
}

// How many meters one of the unit is; 0 for Unknown, which converts to nothing.
inline double metersPer(LengthUnit unit)
{
    for (const Info &info : all()) {
        if (info.unit == unit)
            return info.meters;
    }
    return 0.0;
}

// The unit as the Set Unit filter, and so a script, spells it: the symbol in ASCII ("um" for
// µm), and "not_set" for Unknown.
inline QString scriptId(LengthUnit unit)
{
    if (unit == LengthUnit::Unknown)
        return QStringLiteral("not_set");
    return unit == LengthUnit::Micrometer ? QStringLiteral("um") : symbol(unit);
}

// The unit scriptId() spells this way; Unknown for "not_set" and anything else.
inline LengthUnit fromScriptId(const QString &id)
{
    for (const Info &info : all()) {
        if (scriptId(info.unit) == id)
            return info.unit;
    }
    return LengthUnit::Unknown;
}

// The unit with this symbol; Unknown for anything else, so a file from a newer version that
// names a unit this one lacks reads as not set.
inline LengthUnit fromSymbol(const QString &symbol)
{
    for (const Info &info : all()) {
        if (symbol == QString::fromUtf8(info.symbol))
            return info.unit;
    }
    return LengthUnit::Unknown;
}

} // namespace LengthUnits

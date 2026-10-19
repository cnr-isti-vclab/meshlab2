#pragma once

#include <QCoreApplication>
#include <QString>

#include <array>

// The unit a document's coordinates are measured in (Document::unit()). Most mesh formats do not
// say, so a document starts with Unknown; the user sets it, and from then on the lengths the
// program shows can be named in it. It labels the coordinates and does not change them.
enum class LengthUnit { Unknown, Millimeter, Centimeter, Meter, Kilometer, Inch, Foot };

namespace LengthUnits {

struct Info {
    LengthUnit unit;
    const char *symbol;   // what the interface shows and a project file stores
    const char *name;
};

// The units that can be chosen, smallest metric first; Unknown is the absence of a choice.
inline const std::array<Info, 6> &all()
{
    static const std::array<Info, 6> table{ {
        { LengthUnit::Millimeter, "mm", QT_TRANSLATE_NOOP("LengthUnit", "Millimeters") },
        { LengthUnit::Centimeter, "cm", QT_TRANSLATE_NOOP("LengthUnit", "Centimeters") },
        { LengthUnit::Meter, "m", QT_TRANSLATE_NOOP("LengthUnit", "Meters") },
        { LengthUnit::Kilometer, "km", QT_TRANSLATE_NOOP("LengthUnit", "Kilometers") },
        { LengthUnit::Inch, "in", QT_TRANSLATE_NOOP("LengthUnit", "Inches") },
        { LengthUnit::Foot, "ft", QT_TRANSLATE_NOOP("LengthUnit", "Feet") },
    } };
    return table;
}

// "mm", "m", ...; empty for Unknown.
inline QString symbol(LengthUnit unit)
{
    for (const Info &info : all()) {
        if (info.unit == unit)
            return QString::fromLatin1(info.symbol);
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

// The unit with this symbol; Unknown for anything else, so a file from a newer version that
// names a unit this one lacks reads as not set.
inline LengthUnit fromSymbol(const QString &symbol)
{
    for (const Info &info : all()) {
        if (symbol == QLatin1String(info.symbol))
            return info.unit;
    }
    return LengthUnit::Unknown;
}

} // namespace LengthUnits

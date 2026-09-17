#include "filterpresentation.h"

#include <QHash>
#include <QRegularExpression>

namespace FilterPresentation {

QStringList tokenize(const QString &text)
{
    static const QRegularExpression kSpaceRe(QStringLiteral("\\s+"));
    QStringList terms = text.trimmed().toLower().split(kSpaceRe, Qt::SkipEmptyParts);
    terms.removeDuplicates();
    return terms;
}

bool matches(const Document::FilterInfo &info, const QStringList &terms)
{
    if (terms.isEmpty())
        return true;

    const MeshFilterDescriptor &d = info.descriptor;
    // Categories are searchable so a filter can be found by the family it belongs to
    // even though names deliberately do not repeat their category. Tags are searchable
    // because that is the whole point of them: they carry the synonyms a name cannot
    // ("plane" for a grid, "cube" for a hexahedron, "aabb" for a bounding box), and
    // leaving them out of the haystack forced those words into descriptions instead.
    const QString haystack = (QStringList {
                                  d.name,
                                  d.effectivePythonName(),
                                  d.shortDescription,
                                  d.longDescriptionMarkdown,
                                  d.provenance.project,
                                  info.pluginName,
                                  d.categories.join(QLatin1Char(' ')),
                                  d.tags.join(QLatin1Char(' ')),
                              }).join(QLatin1Char('\n')).toLower();

    for (const QString &term : terms) {
        if (!term.isEmpty() && !haystack.contains(term))
            return false;
    }
    return true;
}

bool titleMatchesAll(const Document::FilterInfo &info, const QStringList &terms)
{
    const QString name = info.descriptor.name.toLower();
    for (const QString &term : terms) {
        if (!term.isEmpty() && !name.contains(term))
            return false;
    }
    return true;
}

QStringList modifiedDataLabels(const QStringList &codes)
{
    static const QHash<QString, QString> kLabels = {
        { QStringLiteral("VG"), QObject::tr("vertex geometry") },
        { QStringLiteral("VN"), QObject::tr("vertex normals") },
        { QStringLiteral("VC"), QObject::tr("vertex color") },
        { QStringLiteral("VQ"), QObject::tr("vertex scalar") },
        { QStringLiteral("VT"), QObject::tr("vertex texcoords") },
        { QStringLiteral("VA"), QObject::tr("vertex attributes") },
        { QStringLiteral("VS"), QObject::tr("vertex selection") },
        { QStringLiteral("FV"), QObject::tr("face-vertex connectivity") },
        { QStringLiteral("FN"), QObject::tr("face normals") },
        { QStringLiteral("FC"), QObject::tr("face color") },
        { QStringLiteral("FQ"), QObject::tr("face scalar") },
        { QStringLiteral("FA"), QObject::tr("face attributes") },
        { QStringLiteral("FS"), QObject::tr("face selection") },
        { QStringLiteral("FP"), QObject::tr("face polygon bits") },
        { QStringLiteral("EC"), QObject::tr("edge color") },
        { QStringLiteral("EQ"), QObject::tr("edge scalar") },
        { QStringLiteral("ES"), QObject::tr("edge selection") },
        { QStringLiteral("WT"), QObject::tr("wedge texcoords") },
        { QStringLiteral("TX"), QObject::tr("texture images") },
        { QStringLiteral("TM"), QObject::tr("transform matrix") },
    };
    QStringList out;
    for (const QString &code : codes)
        out << kLabels.value(code, code);
    return out;
}

} // namespace FilterPresentation

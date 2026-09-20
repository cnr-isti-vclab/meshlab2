#include "filtercategories.h"

#include <QHash>
#include <vector>

namespace {

struct Root {
    const char *name;
    QStringList subs;
};

// The ontology. Keep in step with docs/design/vocabulary.md §1 — that document
// carries the definitions and the discriminators for the borderline cases.
const std::vector<Root> &ontology()
{
    static const std::vector<Root> kOntology = {
        { "Meshing",         { QStringLiteral("Remeshing"), QStringLiteral("Simplification"),
                               QStringLiteral("Subdivision"), QStringLiteral("Quad"),
                               QStringLiteral("Boolean"), QStringLiteral("Deletion") } },
        { "Repair",          { QStringLiteral("Duplicates"), QStringLiteral("Topology"),
                               QStringLiteral("Degenerate"), QStringLiteral("Holes and Borders") } },
        { "Geometry",        { QStringLiteral("Transform"), QStringLiteral("Smoothing"),
                               QStringLiteral("Alignment"), QStringLiteral("Deformation") } },
        { "Attribute",       { QStringLiteral("Normal"), QStringLiteral("Scalar"),
                               QStringLiteral("Curvature"), QStringLiteral("Color"),
                               QStringLiteral("Custom") } },
        { "Selection",       { QStringLiteral("by Attribute"), QStringLiteral("by Topology"),
                               QStringLiteral("by Visibility"), QStringLiteral("Set Operations") } },
        { "Creation",        { QStringLiteral("Primitives"), QStringLiteral("Reconstruction"),
                               QStringLiteral("Sampling") } },
        { "Parametrization", { QStringLiteral("UV Creation"), QStringLiteral("UV Conversion"),
                               QStringLiteral("Segmentation"), QStringLiteral("Atlas Packing"),
                               QStringLiteral("Defragmentation") } },
        { "Texture",         { QStringLiteral("Assignment"), QStringLiteral("Conversion"),
                               QStringLiteral("Packing") } },
        { "Transfer",        { QStringLiteral("Within a Mesh"), QStringLiteral("Between Layers"),
                               QStringLiteral("From Rasters") } },
        { "Measurement",     { QStringLiteral("Geometric"), QStringLiteral("Topological"),
                               QStringLiteral("Statistics") } },
        { "Document",        { QStringLiteral("Layer"), QStringLiteral("Camera"),
                               QStringLiteral("Render") } },
    };
    return kOntology;
}

const QHash<QString, QStringList> &bySubs()
{
    static const QHash<QString, QStringList> kMap = [] {
        QHash<QString, QStringList> m;
        for (const Root &r : ontology())
            m.insert(QString::fromLatin1(r.name), r.subs);
        return m;
    }();
    return kMap;
}

} // namespace

namespace FilterCategories {

QStringList roots()
{
    QStringList out;
    out.reserve(int(ontology().size()));
    for (const Root &r : ontology())
        out << QString::fromLatin1(r.name);
    return out;
}

QStringList subcategories(const QString &root)
{
    return bySubs().value(root);
}

bool isValid(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return false;

    const int slash = trimmed.indexOf(QLatin1Char('/'));
    if (slash < 0)
        return bySubs().contains(trimmed);

    const QString root = trimmed.left(slash).trimmed();
    const QString sub = trimmed.mid(slash + 1).trimmed();
    // Only two levels exist, so a second separator is invalid by construction.
    if (sub.contains(QLatin1Char('/')))
        return false;
    const auto it = bySubs().constFind(root);
    return it != bySubs().constEnd() && it->contains(sub);
}

QStringList allPaths()
{
    QStringList out;
    for (const Root &r : ontology()) {
        const QString root = QString::fromLatin1(r.name);
        out << root;
        for (const QString &s : r.subs)
            out << root + QLatin1Char('/') + s;
    }
    return out;
}

} // namespace FilterCategories

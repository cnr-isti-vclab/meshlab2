#include "filterpresentation.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace FilterPresentation {

namespace {

const QRegularExpression &wordSplitter()
{
    static const QRegularExpression re(QStringLiteral("[^\\p{L}\\p{N}]+"));
    return re;
}

QStringList words(const QString &text)
{
    return text.toLower().split(wordSplitter(), Qt::SkipEmptyParts);
}

// A word with one common ending dropped, so that "decimate" reaches "decimation" and
// "smoothing" "smooth"; empty if too little would be left.
QString wordStem(const QString &term)
{
    static const QStringList endings = { QStringLiteral("ing"), QStringLiteral("ion"),
                                         QStringLiteral("es"), QStringLiteral("ed"),
                                         QStringLiteral("e"), QStringLiteral("s") };
    for (const QString &ending : endings) {
        if (term.endsWith(ending) && term.size() - ending.size() >= 4)
            return term.left(term.size() - ending.size());
    }
    return QString();
}

// The words a term also matches through the aliases it is the start of. A term shorter
// than three letters expands to nothing: "de" would reach delete, denoise and decimate.
QStringList aliasTargets(const QString &term)
{
    QStringList targets;
    if (term.size() < 3)
        return targets;
    const QHash<QString, QStringList> &aliases = searchAliases();
    for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
        if (it.key().startsWith(term))
            targets += it.value();
    }
    targets.removeDuplicates();
    return targets;
}

// The position of the first word that starts with one of the candidates, or -1.
int startsAWord(const QStringList &haystack, const QStringList &candidates)
{
    for (int i = 0; i < haystack.size(); ++i) {
        for (const QString &c : candidates) {
            if (!c.isEmpty() && haystack.at(i).startsWith(c))
                return i;
        }
    }
    return -1;
}

} // namespace

QStringList tokenize(const QString &text)
{
    QStringList terms = words(text);
    terms.removeDuplicates();
    return terms;
}

std::optional<SearchRank> rank(const Document::FilterInfo &info, const QStringList &terms)
{
    const MeshFilterDescriptor &d = info.descriptor;
    const QStringList name = words(d.name);
    SearchRank result;
    result.level = 1;
    result.nameWords = int(name.size());
    if (terms.isEmpty())
        return result;

    // Tags carry the synonyms a name cannot ("plane" for a grid, "aabb" for a bounding box),
    // and categories let a filter be found by its family, which names do not repeat.
    const QStringList labels = words(QStringList{ d.tags.join(QLatin1Char(' ')),
                                                  d.categories.join(QLatin1Char(' ')),
                                                  d.effectivePythonName(), info.pluginName,
                                                  d.provenance.project }
                                         .join(QLatin1Char(' ')));
    const QStringList prose = words(d.shortDescription + QLatin1Char(' ') + d.longDescriptionMarkdown);
    const QString raw = QStringList{ d.name, d.effectivePythonName(), d.shortDescription,
                                     d.longDescriptionMarkdown, d.provenance.project,
                                     info.pluginName, d.categories.join(QLatin1Char(' ')),
                                     d.tags.join(QLatin1Char(' ')) }
                            .join(QLatin1Char('\n'))
                            .toLower();

    for (const QString &term : terms) {
        const QString stem = wordStem(term);
        const QStringList targets = aliasTargets(term);
        QStringList any = targets;
        any << term << stem;
        int level = 0, inexact = 1, position = 0;
        if (const int i = startsAWord(name, { term }); i >= 0) {
            level = 1;
            inexact = (name.at(i) == term) ? 0 : 1;
            position = i;
        } else if (const int j = startsAWord(name, { stem }); j >= 0) {
            level = 1;
            inexact = 2;
            position = j;
        } else if (const int k = startsAWord(name, targets); k >= 0) {
            level = 2;
            position = k;
        } else if (startsAWord(labels, any) >= 0) {
            level = 3;
            position = int(name.size());
        } else if (startsAWord(prose, any) >= 0) {
            level = 4;
            position = int(name.size());
        } else if (raw.contains(term)) {
            level = 5;
            position = int(name.size());
        } else {
            return std::nullopt;
        }
        result.level = std::max(result.level, level);
        result.inexact += inexact;
        result.position += position;
    }
    return result;
}

bool matches(const Document::FilterInfo &info, const QStringList &terms)
{
    return rank(info, terms).has_value();
}

const QHash<QString, QStringList> &searchAliases()
{
    static const QHash<QString, QStringList> aliases = [] {
        // typed words, then the words they also match
        const std::pair<const char *, const char *> table[] = {
            // Verbs: the rejected synonyms of vocabulary.md section 3. Left out on purpose,
            // as each also means something else common: copy, map, push, pull, apply, bin.
            { "delete erase discard purge", "remove" },
            { "generate make build", "create" },
            { "rebuild", "reconstruct remesh" },
            { "resample", "sample remesh" },
            { "fix heal clean", "repair" },
            { "weld unify join", "merge" },
            { "pick mark", "select" },
            { "calculate recompute", "compute" },
            { "inspect report analyze analyse info stats", "measure" },
            { "paint colourise colourize", "colorize color" },
            { "bake", "transfer freeze" },
            { "move", "translate transform" },
            { "shift", "translate" },
            { "turn", "rotate" },
            { "resize", "scale" },
            { "deform", "transform displace" },
            { "perturb", "displace" },
            { "commit", "freeze" },
            { "register registration fit", "align" },
            { "decimate decimation reduce", "simplify" },
            { "retriangulate", "remesh" },
            { "denoise fair blur relax", "smooth" },
            { "unwrap parameterize parameterization", "parametrize parametrization" },
            { "flatten", "parametrize transfer" },
            { "layout", "pack" },
            { "assign", "set" },
            { "cast", "convert" },
            { "clone", "duplicate" },
            { "separate detach", "split extract" },
            { "reorient", "orient" },
            { "fill patch cap", "close" },
            { "load read", "import" },
            { "save write", "export" },
            { "relabel", "rename" },
            { "draw snapshot screenshot", "render" },
            { "insert imprint conform", "embed" },
            // Nouns and spellings: vocabulary.md, "Search aliases".
            { "position positions", "coordinates" },
            { "quality", "scalar" },
            { "texcoord texcoords uvs", "uv" },
            { "colour colours", "color" },
            { "triangle triangles tri", "face" },
            { "photo photograph picture", "raster image" },
            { "image", "raster" },
            { "model object", "mesh" },
            { "bbox aabb", "bounding box" },
            { "pointcloud points", "point cloud" },
            { "isoline contour", "isocontour" },
            { "hole holes", "boundary" },
        };
        QHash<QString, QStringList> out;
        for (const auto &[typed, targets] : table) {
            const QStringList to = QString::fromLatin1(targets).split(QLatin1Char(' '));
            for (const QString &word : QString::fromLatin1(typed).split(QLatin1Char(' ')))
                out[word] += to;
        }
        return out;
    }();
    return aliases;
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
        { QStringLiteral("FES"), QObject::tr("face-edge selection") },
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

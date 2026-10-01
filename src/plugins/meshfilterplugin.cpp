#include "meshfilterplugin.h"
#include "filterdescriptorloader.h"

#include <algorithm>
#include <cmath>

namespace {

QString bibValue(QString value)
{
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar character : value) {
        if (character == u'\\')
            escaped += QStringLiteral("{\\textbackslash}");
        else if (QStringLiteral("{}&%#_").contains(character))
            escaped += u'\\' + character;
        else
            escaped += character;
    }
    return escaped;
}

QString authorText(const MeshFilterReferenceAuthor &author)
{
    if (author.given.isEmpty())
        return author.family;
    if (author.family.isEmpty())
        return author.given;
    return author.given + u' ' + author.family;
}


// One implementation of the bibliography: field order, punctuation and which links
// to emit. Markdown and HTML differ only in markup, so they pass different styles
// rather than each assembling the citation themselves — that divergence previously
// left the filter panel showing a citation missing volume, issue, pages and publisher.
struct CitationStyle
{
    QString (*bold)(const QString &);
    QString (*italic)(const QString &);
    QString (*link)(const QString &text, const QString &url);
    QString (*plain)(const QString &);
};

QString buildCitation(const MeshFilterReference &r, const CitationStyle &style)
{
    QStringList names;
    for (const MeshFilterReferenceAuthor &author : r.authors)
        names << style.plain(authorText(author));

    QString out;
    if (!names.isEmpty())
        out += names.join(QStringLiteral(", ")) + QStringLiteral(". ");
    if (!r.title.trimmed().isEmpty())
        out += style.bold(style.plain(r.title)) + QStringLiteral(". ");
    if (!r.containerTitle.trimmed().isEmpty())
        out += style.italic(style.plain(r.containerTitle));

    // volume(issue):pages — journal style.
    QString locator;
    if (!r.volume.trimmed().isEmpty()) {
        locator = style.plain(r.volume);
        if (!r.issue.trimmed().isEmpty())
            locator += QStringLiteral("(%1)").arg(style.plain(r.issue));
    }
    if (!r.page.trimmed().isEmpty())
        locator += (locator.isEmpty() ? QString() : QStringLiteral(":")) + style.plain(r.page);
    if (!locator.isEmpty())
        out += QStringLiteral(", %1").arg(locator);
    if (!r.edition.trimmed().isEmpty())
        out += QObject::tr(", %1 ed.").arg(style.plain(r.edition));
    if (!r.publisher.trimmed().isEmpty())
        out += QStringLiteral(". %1").arg(style.plain(r.publisher));
    if (r.year > 0)
        out += QStringLiteral(" (%1)").arg(r.year);
    if (!out.trimmed().endsWith(QLatin1Char('.')))
        out += QLatin1Char('.');

    QStringList links;
    if (!r.doiUrl().isEmpty())
        links << style.link(QStringLiteral("doi:%1").arg(style.plain(r.doi)), r.doiUrl());
    // webUrl() already suppresses a URL that merely repeats the DOI.
    if (!r.webUrl().isEmpty())
        links << style.link(QObject::tr("web"), r.webUrl());
    // A book has no DOI, so the ISBN is the identifier worth showing.
    if (!r.isbn.trimmed().isEmpty())
        links << style.plain(QStringLiteral("ISBN %1").arg(r.isbn));
    if (!links.isEmpty())
        out += QStringLiteral(" %1").arg(links.join(QStringLiteral(" · ")));
    return out;
}

} // namespace

QString MeshFilterReference::label() const
{
    QString result;
    if (!authors.empty())
        result = authors.front().family + (authors.size() > 1 ? QStringLiteral(" et al.") : QString());
    if (year > 0)
        result += (result.isEmpty() ? QString() : QStringLiteral(", ")) + QString::number(year);
    return result.isEmpty() ? title : result;
}

QString MeshFilterReference::doiUrl() const
{
    if (doi.isEmpty())
        return {};
    return QStringLiteral("https://doi.org/%1").arg(doi);
}

QString MeshFilterReference::webUrl() const
{
    if (url.isEmpty())
        return {};
    QString normalizedUrl = url;
    QString normalizedDoi = doiUrl();
    normalizedUrl.remove(u'/');
    normalizedDoi.remove(u'/');
    if (!normalizedDoi.isEmpty()
        && normalizedUrl.compare(normalizedDoi, Qt::CaseInsensitive) == 0)
        return {};
    return url;
}

QString MeshFilterReference::markdownCitation() const
{
    static const CitationStyle style {
        [](const QString &t) { return QStringLiteral("**%1**").arg(t); },
        [](const QString &t) { return QStringLiteral("*%1*").arg(t); },
        [](const QString &t, const QString &u) { return QStringLiteral("[%1](%2)").arg(t, u); },
        [](const QString &t) { return t; },
    };
    return buildCitation(*this, style);
}

QString MeshFilterReference::htmlCitation() const
{
    static const CitationStyle style {
        [](const QString &t) { return QStringLiteral("<b>%1</b>").arg(t); },
        [](const QString &t) { return QStringLiteral("<i>%1</i>").arg(t); },
        [](const QString &t, const QString &u) {
            return QStringLiteral("<a href=\"%1\">%2</a>").arg(u.toHtmlEscaped(), t);
        },
        [](const QString &t) { return t.toHtmlEscaped(); },
    };
    return buildCitation(*this, style);
}

QString MeshFilterReference::bibTeX() const
{
    QString entryType = QStringLiteral("misc");
    if (type == QStringLiteral("article-journal"))
        entryType = QStringLiteral("article");
    else if (type == QStringLiteral("paper-conference"))
        entryType = QStringLiteral("inproceedings");
    else if (type == QStringLiteral("book"))
        entryType = QStringLiteral("book");
    else if (type == QStringLiteral("chapter"))
        entryType = QStringLiteral("incollection");

    QStringList fields;
    auto add = [&fields](const QString &name, const QString &value) {
        if (!value.isEmpty())
            fields << QStringLiteral("  %1 = {%2}").arg(name, bibValue(value));
    };
    add(QStringLiteral("title"), title);
    QStringList bibAuthors;
    for (const MeshFilterReferenceAuthor &author : authors) {
        bibAuthors << (author.given.isEmpty()
                           ? author.family
                           : QStringLiteral("%1, %2").arg(author.family, author.given));
    }
    add(QStringLiteral("author"), bibAuthors.join(QStringLiteral(" and ")));
    add(type == QStringLiteral("article-journal") ? QStringLiteral("journal")
                                                  : QStringLiteral("booktitle"),
        containerTitle);
    if (year > 0)
        add(QStringLiteral("year"), QString::number(year));
    add(QStringLiteral("volume"), volume);
    add(QStringLiteral("number"), issue);
    QString bibPages = page;
    bibPages.replace(u'-', QStringLiteral("--"));
    add(QStringLiteral("pages"), bibPages);
    add(QStringLiteral("publisher"), publisher);
    add(QStringLiteral("edition"), edition);
    add(QStringLiteral("doi"), doi);
    add(QStringLiteral("isbn"), isbn);
    add(QStringLiteral("url"), url);
    return QStringLiteral("@%1{%2,\n%3\n}")
        .arg(entryType, id, fields.join(QStringLiteral(",\n")));
}

std::vector<MeshFilterDescriptor> MeshFilterPlugin::filters(const Document &doc) const
{
    const QString resourcePath =
        QStringLiteral(":/filters/%1/filters.json").arg(pluginId());
    QString error;
    auto descriptors = FilterDescriptorLoader::load(resourcePath, error);
    if (!error.isEmpty()) {
        // Plugin did not ship filters.json or it is malformed — return empty.
        return {};
    }
    FilterDescriptorLoader::resolveSymbolicBounds(descriptors, doc);
    return descriptors;
}

bool sameParameterValue(const QVariant &a, const QVariant &b)
{
    const auto isNumber = [](const QVariant &v) {
        switch (v.metaType().id()) {
        case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong:
        case QMetaType::ULongLong: case QMetaType::Double: case QMetaType::Float:
            return true;
        default:
            return false;
        }
    };
    if (isNumber(a) && isNumber(b)) {
        const double x = a.toDouble();
        const double y = b.toDouble();
        return std::abs(x - y) <= 1e-9 * std::max({ 1.0, std::abs(x), std::abs(y) });
    }
    return a == b;
}

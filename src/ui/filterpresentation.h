#pragma once

#include "document.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <tuple>

// Presentation policy shared by every surface that lists or describes filters —
// the filter panel and the Filter Plugins Info dialog. Kept in one place so the two
// cannot drift: they previously carried near-identical search predicates, and the
// panel showed raw `outputModifies` codes while the dialog spelled them out.
namespace FilterPresentation {

// Filter search, as docs/design/proposals/filter_search.md lays it out. A query is split into
// words at anything not a letter or a digit; every word must match, at the start of a word of
// the filter, through a word form or a search alias, or, last of all, inside a word.

// Split a query into lower-cased, de-duplicated terms.
QStringList tokenize(const QString &text);

// How well a filter answers a query. Lower sorts first: the level (1 every term starts a word
// of the name, 2 some only through an alias, 3 in tags, categories, Python name or plugin, 4 in
// the descriptions, 5 only inside a word), then exact words before prefixes before word forms,
// then earlier words in the name, then shorter names.
struct SearchRank
{
    int level = 0;
    int inexact = 0;
    int position = 0;
    int nameWords = 0;
    bool operator<(const SearchRank &o) const
    {
        return std::tie(level, inexact, position, nameWords)
            < std::tie(o.level, o.inexact, o.position, o.nameWords);
    }
};

// The rank of a filter for these terms, or nothing if it does not match all of them.
std::optional<SearchRank> rank(const Document::FilterInfo &info, const QStringList &terms);

// True if the filter matches every term (rank() has a value).
bool matches(const Document::FilterInfo &info, const QStringList &terms);

// The search aliases: a word a user may type, and the words it also matches. One way only:
// "position" reaches "coordinates", not the reverse. Most are the rejected synonyms of
// docs/design/vocabulary.md section 3; the rest are its section "Search aliases".
const QHash<QString, QStringList> &searchAliases();

// Human-readable expansion of the two-letter `outputModifies` codes, e.g.
// {"FQ","VQ"} -> {"face scalar", "vertex scalar"}. Unknown codes pass through.
QStringList modifiedDataLabels(const QStringList &codes);

} // namespace FilterPresentation

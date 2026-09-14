#pragma once

#include <QString>
#include <QStringList>

// The MeshLab filter-category ontology: the single source of truth for how
// filters are classified. Normative description, including the definition and
// discriminator of every category, lives in docs/design/vocabulary.md §1.
//
// Shape: two levels. A category is either a bare root ("Texture") or a
// "Root/Subcategory" pair ("Meshing/Simplification"). Roots are nouns; verbs
// belong in filter names. Backend and library names are never categories.
//
// A filter carries an ordered *set* of categories (first entry primary), so this
// API validates one path at a time.
namespace FilterCategories {

// All roots, in ontology order (not alphabetical).
QStringList roots();

// Declared subcategories of a root; empty if the root is unknown or has none.
QStringList subcategories(const QString &root);

// True if path is a valid category: a known root, or "Root/Subcategory" where
// the subcategory is declared for that root. Leading/trailing spaces around
// either segment are tolerated; matching is case-sensitive, because the ontology
// fixes the spelling.
bool isValid(const QString &path);

// Every valid path, roots and "Root/Subcategory" alike. For diagnostics and tests.
QStringList allPaths();

} // namespace FilterCategories

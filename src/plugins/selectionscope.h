#pragma once

#include <QString>
#include <cstdint>
#include <vector>

class Document;
class VCGMesh;

// A filter that can confine itself to the selection declares, in filters.json,
//
//     "selectionScope": "vertices" | "faces" | "edges"
//
// instead of a toggle of its own. The loader injects the one `selectedOnly` parameter
// from it, the panel shows it as a scope control with the live count, and the manager
// refuses an empty restriction and hands the filter a selection of its own kind. See
// docs/design/adding_a_filter.md.
//
// The contract a scoped filter keeps with `selectedOnly` on: no unselected element of
// its kind changes. For a face scope that means every unselected face survives with its
// vertices where they were -- so a filter that moves vertices moves only the interior
// ones of the selected region, and one that refines splits only edges whose faces are
// all selected. With `selectedOnly` off the selection makes no difference.
enum class SelectionScope { None, Vertices, Faces, Edges };

namespace SelectionScopes {

inline constexpr const char *kParameterId = "selectedOnly";

SelectionScope parse(const QString &text, bool *ok = nullptr);
QString parameterLabel(SelectionScope scope);
QString parameterHelp(SelectionScope scope);

// What a scoped filter is restricted to on a mesh: the selection of its own kind when
// there is one, otherwise the elements lying wholly inside the other kind's -- the
// vertices whose faces are all selected, the faces and edges whose vertices all are.
// Strict both ways, so that deriving the missing kind never reaches past the selection.
struct Restriction
{
    int count = 0;
    bool derived = false; // taken from the other kind of selection
};
Restriction restriction(const VCGMesh &mesh, SelectionScope scope);

// "Only the 1,234 selected faces", "Only the 812 vertices inside the selected faces",
// "No faces selected".
QString describe(SelectionScope scope, const Restriction &restriction);
// Why a run with `selectedOnly` on and nothing to restrict to is refused.
QString emptyRestrictionError(SelectionScope scope);

// Brackets one run of a scoped filter on one mesh. Before it, when the scope's own kind
// has nothing selected, writes the derived selection so the filter only ever has to read
// its own kind. After it, puts the user's selection back -- vertex, face and edge bits
// alike, since several filters convert between kinds internally -- unless the filter
// reported a selection change of its own (Document::markMeshSelectionChanged). Bits of a
// kind whose element count changed cannot be matched by index and are left as the
// filter left them, except a derived kind, which the user never had and is cleared.
class RunGuard
{
public:
    RunGuard(Document &doc, int meshIndex, SelectionScope scope, bool selectedOnly);
    ~RunGuard();
    RunGuard(const RunGuard &) = delete;
    RunGuard &operator=(const RunGuard &) = delete;

    // Idempotent; the destructor calls it for runs that fail or throw.
    void restore();

private:
    Document &m_doc;
    std::uint64_t m_meshId = 0;
    bool m_active = false;
    SelectionScope m_derivedKind = SelectionScope::None;
    std::uint64_t m_selectionRevision = 0;
    std::vector<bool> m_vert;
    std::vector<bool> m_face;
    std::vector<bool> m_edge;
};

} // namespace SelectionScopes

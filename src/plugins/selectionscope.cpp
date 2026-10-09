#include "selectionscope.h"

#include "document.h"
#include "vcgmesh.h"

#include <QObject>

#include <map>
#include <tuple>

namespace SelectionScopes {

namespace {

// Singular and plural nouns of a scope, for the strings below.
QString pluralNoun(SelectionScope scope)
{
    switch (scope) {
    case SelectionScope::Vertices: return QObject::tr("vertices");
    case SelectionScope::Faces: return QObject::tr("faces");
    case SelectionScope::Edges: return QObject::tr("edges");
    case SelectionScope::None: break;
    }
    return {};
}

// The kind a missing selection is derived from: faces for vertices, vertices otherwise.
QString sourceNoun(SelectionScope scope)
{
    return scope == SelectionScope::Vertices ? QObject::tr("faces") : QObject::tr("vertices");
}

template <class Container>
int selectedCount(const Container &elements)
{
    int n = 0;
    for (const auto &e : elements) {
        if (!e.IsD() && e.IsS())
            ++n;
    }
    return n;
}

// The vertices whose faces are all selected: marked by any selected face, unmarked by
// any unselected one. Isolated vertices belong to no face and so to no selection.
std::vector<bool> interiorVerticesOfSelectedFaces(const VCGMesh &mesh)
{
    std::vector<bool> inside(mesh.vert.size(), false);
    std::vector<bool> outside(mesh.vert.size(), false);
    for (const VCGFace &f : mesh.face) {
        if (f.IsD())
            continue;
        std::vector<bool> &mark = f.IsS() ? inside : outside;
        for (int i = 0; i < f.VN(); ++i)
            mark[size_t(vcg::tri::Index(mesh, f.cV(i)))] = true;
    }
    for (size_t i = 0; i < inside.size(); ++i)
        inside[i] = inside[i] && !outside[i];
    return inside;
}

template <class Element>
bool allVerticesSelected(const Element &e, int vertexCount)
{
    for (int i = 0; i < vertexCount; ++i) {
        if (!e.cV(i)->IsS())
            return false;
    }
    return true;
}

// The derived selection of `scope`'s kind, one flag per element slot.
std::vector<bool> derivedSelection(const VCGMesh &mesh, SelectionScope scope)
{
    std::vector<bool> derived;
    switch (scope) {
    case SelectionScope::Vertices:
        derived = interiorVerticesOfSelectedFaces(mesh);
        for (size_t i = 0; i < derived.size(); ++i)
            derived[i] = derived[i] && !mesh.vert[i].IsD();
        break;
    case SelectionScope::Faces:
        derived.resize(mesh.face.size(), false);
        for (size_t i = 0; i < mesh.face.size(); ++i) {
            const VCGFace &f = mesh.face[i];
            derived[i] = !f.IsD() && f.VN() > 0 && allVerticesSelected(f, f.VN());
        }
        break;
    case SelectionScope::Edges:
        derived.resize(mesh.edge.size(), false);
        for (size_t i = 0; i < mesh.edge.size(); ++i) {
            const VCGEdge &e = mesh.edge[i];
            derived[i] = !e.IsD() && allVerticesSelected(e, 2);
        }
        break;
    case SelectionScope::None:
        break;
    }
    return derived;
}

int ownKindCount(const VCGMesh &mesh, SelectionScope scope)
{
    switch (scope) {
    case SelectionScope::Vertices: return selectedCount(mesh.vert);
    case SelectionScope::Faces: return selectedCount(mesh.face);
    case SelectionScope::Edges: return selectedCount(mesh.edge);
    case SelectionScope::None: break;
    }
    return 0;
}

template <class Container>
std::vector<bool> saveBits(const Container &elements)
{
    std::vector<bool> bits(elements.size(), false);
    for (size_t i = 0; i < elements.size(); ++i)
        bits[i] = !elements[i].IsD() && elements[i].IsS();
    return bits;
}

template <class Container>
void writeBits(Container &elements, const std::vector<bool> &bits)
{
    for (size_t i = 0; i < elements.size() && i < bits.size(); ++i) {
        if (elements[i].IsD())
            continue;
        if (bits[i])
            elements[i].SetS();
        else
            elements[i].ClearS();
    }
}

template <class Container>
void clearBits(Container &elements)
{
    for (auto &e : elements) {
        if (!e.IsD())
            e.ClearS();
    }
}

Document::MeshEntry *entryById(Document &doc, std::uint64_t meshId)
{
    for (int i = 0; i < doc.meshCount(); ++i) {
        if (doc.mesh(i).meshId == meshId)
            return &doc.mesh(i);
    }
    return nullptr;
}

} // namespace

SelectionScope parse(const QString &text, bool *ok)
{
    const QString t = text.trimmed().toLower();
    SelectionScope scope = SelectionScope::None;
    bool valid = true;
    if (t == QStringLiteral("vertices"))
        scope = SelectionScope::Vertices;
    else if (t == QStringLiteral("faces"))
        scope = SelectionScope::Faces;
    else if (t == QStringLiteral("edges"))
        scope = SelectionScope::Edges;
    else
        valid = t.isEmpty();
    if (ok)
        *ok = valid;
    return scope;
}

QString parameterLabel(SelectionScope scope)
{
    switch (scope) {
    case SelectionScope::Vertices: return QObject::tr("Only selected vertices");
    case SelectionScope::Faces: return QObject::tr("Only selected faces");
    case SelectionScope::Edges: return QObject::tr("Only selected edges");
    case SelectionScope::None: break;
    }
    return {};
}

QString parameterHelp(SelectionScope scope)
{
    return QObject::tr(
               "Confine the filter to the selected %1: nothing outside the selection "
               "changes. When no %1 are selected but %2 are, it works on the %1 lying "
               "wholly inside the selected %2. On by default whenever there is a "
               "selection to confine it to.")
        .arg(pluralNoun(scope), sourceNoun(scope));
}

Restriction restriction(const VCGMesh &mesh, SelectionScope scope)
{
    Restriction r;
    if (scope == SelectionScope::None)
        return r;
    r.count = ownKindCount(mesh, scope);
    if (r.count > 0)
        return r;
    int derived = 0;
    for (bool b : derivedSelection(mesh, scope))
        derived += b ? 1 : 0;
    r.count = derived;
    r.derived = derived > 0;
    return r;
}

Restriction cachedRestriction(const Document &doc, int meshIndex, SelectionScope scope)
{
    if (scope == SelectionScope::None || meshIndex < 0 || meshIndex >= doc.meshCount())
        return {};
    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    // Content identity, as the undo history uses it, within one document: mesh ids and
    // revisions are numbered per document, so two documents would otherwise answer for each
    // other. Few are ever live at once; the cache is dropped whole when it has seen many,
    // rather than aged.
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, int>;
    static std::map<Key, Restriction> cache;
    const Key key{ doc.instanceId(), entry.meshId, entry.geometryRevision, entry.selectionRevision,
                   int(scope) };
    const auto hit = cache.find(key);
    if (hit != cache.end())
        return hit->second;
    if (cache.size() > 256)
        cache.clear();
    return cache.emplace(key, restriction(entry.mesh, scope)).first->second;
}

QString describe(SelectionScope scope, const Restriction &restriction)
{
    if (scope == SelectionScope::None)
        return {};
    if (restriction.count <= 0)
        return QObject::tr("No %1 selected").arg(pluralNoun(scope));
    if (restriction.derived) {
        return QObject::tr("Only the %1 %2 inside the selected %3")
            .arg(restriction.count)
            .arg(pluralNoun(scope), sourceNoun(scope));
    }
    return QObject::tr("Only the %1 selected %2").arg(restriction.count).arg(pluralNoun(scope));
}

QString emptyRestrictionError(SelectionScope scope)
{
    return QObject::tr("%1 is on, but no %2 are selected, and none lie wholly inside the "
                       "selected %3.")
        .arg(parameterLabel(scope), pluralNoun(scope), sourceNoun(scope));
}

RunGuard::RunGuard(Document &doc, int meshIndex, SelectionScope scope, bool selectedOnly)
    : m_doc(doc)
{
    if (scope == SelectionScope::None || meshIndex < 0 || meshIndex >= doc.meshCount())
        return;
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    m_meshId = entry.meshId;
    m_selectionRevision = entry.selectionRevision;
    m_vert = saveBits(mesh.vert);
    m_face = saveBits(mesh.face);
    m_edge = saveBits(mesh.edge);
    m_active = true;

    if (!selectedOnly || ownKindCount(mesh, scope) > 0)
        return;
    const std::vector<bool> derived = derivedSelection(mesh, scope);
    switch (scope) {
    case SelectionScope::Vertices: writeBits(mesh.vert, derived); break;
    case SelectionScope::Faces: writeBits(mesh.face, derived); break;
    case SelectionScope::Edges: writeBits(mesh.edge, derived); break;
    case SelectionScope::None: break;
    }
    m_derivedKind = scope;
}

RunGuard::~RunGuard()
{
    restore();
}

void RunGuard::restore()
{
    if (!m_active)
        return;
    m_active = false;
    Document::MeshEntry *entry = entryById(m_doc, m_meshId);
    if (!entry)
        return;
    // A filter that changes the selection on purpose says so; that selection is its
    // result, not a side effect to undo.
    if (entry->selectionRevision != m_selectionRevision)
        return;
    VCGMesh &mesh = entry->mesh;
    const auto put = [this](auto &elements, const std::vector<bool> &saved, SelectionScope kind) {
        if (kind == m_derivedKind)
            clearBits(elements);
        else if (elements.size() == saved.size())
            writeBits(elements, saved);
    };
    put(mesh.vert, m_vert, SelectionScope::Vertices);
    put(mesh.face, m_face, SelectionScope::Faces);
    put(mesh.edge, m_edge, SelectionScope::Edges);
}

} // namespace SelectionScopes

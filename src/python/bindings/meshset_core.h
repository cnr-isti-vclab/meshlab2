#pragma once

#ifdef slots
#  pragma push_macro("slots")
#  undef slots
#  define MESHLAB2_RESTORE_QT_SLOTS_MACRO
#endif

#include <nanobind/nanobind.h>

#ifdef MESHLAB2_RESTORE_QT_SLOTS_MACRO
#  pragma pop_macro("slots")
#  undef MESHLAB2_RESTORE_QT_SLOTS_MACRO
#endif

#include <QString>
#include <QVariant>
#include <memory>
#include <string>
#include <vector>

class Document;
#if !defined(PYMESHLAB_STANDALONE)
class HeadlessRenderContext;
#endif
class PyMesh;

struct FilterInfoRecord
{
    std::string key;
    std::string id;
    std::string plugin_id;
    std::string plugin_name;
    std::string name;
    std::string python_name;   // snake_case Python identifier, e.g. "meshing_remove_duplicate_vertices"
    bool applicable = true;
    std::string applicability_error;
};

struct FilterRunRecord
{
    bool success = false;
    bool document_modified = false;
    std::string error_message;
    std::vector<std::string> info_messages;
    std::vector<int> new_mesh_indices;
    QVariantMap output_values;
};

// C++ core that wraps a Document and is exposed to Python as `MeshSet`.
//
// Two construction modes:
//  - MeshSetCore()          — standalone: creates and owns its own Document.
//                             Used by the pymeshlab2 standalone library.
//  - MeshSetCore(Document*) — embedded: borrows a live Document owned by
//                             MainWindow.  Does NOT delete the document on
//                             destruction.  Used by the in-app Python console.
class MeshSetCore
{
public:
    MeshSetCore();
    explicit MeshSetCore(Document *doc);
    ~MeshSetCore();

    int meshCount() const;
    int currentMeshIndex() const;
    int currentMeshId() const;
    void setCurrentMesh(int index);
    bool meshIdExists(int index) const;
    nanobind::object currentMesh() const;
    nanobind::object mesh(int index) const;
    void setCurrentMeshVisibility(bool visible);
    void setMeshVisibility(int index, bool visible);
    bool isCurrentMeshVisible() const;
    bool isMeshVisible(int index) const;

    void loadNewMesh(const std::string &path);
    void saveCurrentMesh(const std::string &path);

    int rasterCount() const;
    int currentRasterIndex() const;
    void setCurrentRaster(int index);
    void loadRasterImage(const std::string &path);

    void clear();
    void loadProject(const std::string &path);
    void saveProject(const std::string &path);

    std::vector<std::string> filterList() const;
    std::vector<FilterInfoRecord> listFilters() const;
    FilterRunRecord applyFilter(const std::string &filterNameOrKey,
                                const nanobind::dict &params) const;

#if !defined(PYMESHLAB_STANDALONE)
    nanobind::bytes renderSnapshot(const std::string &renderStateJson,
                                   int width,
                                   int height);
#endif

private:
    QString resolveFilterKey(const QString &filterNameOrKey) const;

    Document *m_document = nullptr;
    bool m_ownsDocument = false;
#if !defined(PYMESHLAB_STANDALONE)
    mutable std::unique_ptr<HeadlessRenderContext> m_renderContext;
#endif
};

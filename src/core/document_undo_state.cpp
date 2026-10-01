#include "document_internal.h"
#include "documentundomanager.h"

using namespace DocumentInternal;

UndoState Document::captureUndoState() const
{
    UndoState state;
    state.currentMeshIndex = m_currentMeshIndex;
    state.currentRasterIndex = m_currentRasterIndex;
    state.currentLayerKind = m_currentLayerKind;
    state.nextMeshId = m_nextMeshId;
    state.nextRasterId = m_nextRasterId;
    state.meshes.reserve(m_meshes.size());
    for (const auto &entry : m_meshes) {
        if (!entry)
            continue;

        MeshSnapshot snap;
        // Copy all cheap metadata by value; this is O(1) for numeric/bool fields
        // and O(n_textures) for the string lists — negligible compared to geometry.
        snap.meshId             = entry->meshId;
        snap.geometryRevision   = entry->geometryRevision;
        snap.materialRevision   = entry->materialRevision;
        snap.selectionRevision  = entry->selectionRevision;
        snap.transform    = entry->transform;
        snap.name               = entry->name;
        snap.sourcePath         = entry->sourcePath;
        snap.textureFileNames   = entry->textureFileNames;
        snap.textureFilePaths   = entry->textureFilePaths;
        snap.textureAssets      = entry->textureAssets;
        snap.materialSet        = entry->materialSet;
        snap.visible            = entry->visible;
        snap.modified           = entry->modified;
        snap.ioMask             = entry->ioMask;
        snap.polygonFaceCount   = entry->polygonFaceCount;
        // Pointers, not payloads: immutability makes sharing the capture.
        snap.pluginData         = entry->pluginData;

        // Attempt to reuse an already-interned geometry object.
        // Key: (meshId, geometryRevision, selectionRevision) — the full content
        // identity. As long as those revisions haven't changed since the last
        // capture, every subsequent checkpoint shares the same VCGMesh allocation
        // — zero extra deep-copy cost for actions that touch neither.
        const auto key = std::make_tuple(entry->meshId, entry->geometryRevision, entry->selectionRevision);
        auto it = m_undoManager->geometryCache().find(key);
        if (it != m_undoManager->geometryCache().end())
            snap.geometry = it->second.lock(); // null if all checkpoints were evicted

        if (!snap.geometry) {
            // Cache miss (first capture after a geometry change, or after the cached
            // weak_ptr expired). Deep-copy now and intern for future captures.
            auto g = std::make_shared<VCGMesh>();
            deepCopyMesh(entry->mesh, *g);
            snap.geometry = g;
            m_undoManager->geometryCache()[key] = g; // weak_ptr — does not keep g alive on its own
        }

        state.meshes.push_back(std::move(snap));
    }
    state.rasters.reserve(m_rasters.size());
    for (const auto &entry : m_rasters) {
        if (!entry)
            continue;

        RasterSnapshot snap;
        snap.rasterId = entry->rasterId;
        snap.imageRevision = entry->imageRevision;
        snap.cameraRevision = entry->cameraRevision;
        snap.name = entry->name;
        snap.sourcePath = entry->sourcePath;
        snap.visible = entry->visible;
        snap.shot = entry->shot;
        snap.planes = entry->planes;
        snap.currentPlaneIndex = entry->currentPlaneIndex;
        state.rasters.push_back(std::move(snap));
    }
    if (m_captureViewState)
        state.viewState = m_captureViewState();
    return state;
}

void Document::restoreUndoState(const UndoState &state)
{
    const bool prevRestoring = m_undoManager->isRestoring();
    m_undoManager->setRestoring(true);
    {
        qDebug() << "[STATE RESTORE] current=" << m_undoManager->currentNode()
                 << " total nodes=" << static_cast<int>(m_undoManager->nodes().size());
        for (int i = 0; i < static_cast<int>(m_undoManager->nodes().size()); ++i) {
            const auto &n = m_undoManager->nodes()[static_cast<size_t>(i)];
            qDebug() << "  node" << i
                     << "parent=" << n.parentId
                     << "prefChild=" << n.preferredChild
                     << "children=" << QVector<int>(n.children.begin(), n.children.end())
                     << (i == m_undoManager->currentNode() ? "<-- current" : "")
                     << "label=" << (n.label.isEmpty() ? QStringLiteral("(root)") : n.label);
        }
    }

    // Evict geometry cache entries whose revision is strictly greater than the
    // revision we are about to restore.  Because m_nextGeometryRevision is a
    // globally monotonic counter that is never reset during undo/redo, each
    // distinct geometry snapshot always receives a unique (meshId, geometryRevision)
    // key.  Cross-branch cache collisions are therefore impossible.
    // The loop below is kept as a memory-hygiene pass: it removes weak_ptr entries
    // that can never be referenced by any state reachable from the restored node,
    // letting the map stay compact even before the natural weak_ptr expiry of
    // pruned nodes.
    for (const auto &snap : state.meshes) {
        auto it = m_undoManager->geometryCache().lower_bound(
            std::make_tuple(snap.meshId, snap.geometryRevision, snap.selectionRevision + 1));
        while (it != m_undoManager->geometryCache().end() && std::get<0>(it->first) == snap.meshId)
            it = m_undoManager->geometryCache().erase(it);
    }

    {
        QElapsedTimer t; t.start();
        // Suppress per-mesh add/remove signals during undo restoration —
        // LayerWidget and RenderWidget will be rebuilt from the final state
        // via a single meshDataChanged batch below.
        m_undoManager->setRestoring(true);
        for (int i = meshCount() - 1; i >= 0; --i) {
            const std::uint64_t meshId = m_meshes[static_cast<size_t>(i)]->meshId;
            m_meshes.erase(m_meshes.begin() + i);
            purgeMeshGpuResources(meshId);
        }
        m_meshes.clear();
        m_meshes.reserve(state.meshes.size());
        {
            qint64 copyMs = 0;
            for (const auto &snap : state.meshes) {
                auto entry = std::make_unique<MeshEntry>();
                entry->meshId           = snap.meshId;
                entry->geometryRevision = snap.geometryRevision;
                entry->materialRevision = snap.materialRevision;
                entry->selectionRevision = snap.selectionRevision;
                entry->transform  = snap.transform;
                entry->name             = snap.name;
                entry->sourcePath       = snap.sourcePath;
                entry->textureFileNames = snap.textureFileNames;
                entry->textureFilePaths = snap.textureFilePaths;
                entry->textureAssets    = snap.textureAssets;
                entry->materialSet      = snap.materialSet;
                entry->visible          = snap.visible;
                entry->modified         = snap.modified;
                entry->ioMask           = snap.ioMask;
                entry->polygonFaceCount = snap.polygonFaceCount;
                entry->pluginData       = snap.pluginData;
                {
                    QElapsedTimer t2; t2.start();
                    deepCopyMesh(*snap.geometry, entry->mesh);
                    copyMs += t2.elapsed();
                }
                m_meshes.push_back(std::move(entry));
            }
            writeLog(tr("Undo/redo — mesh restore: %1 ms (deepCopy %2)")
                .arg(t.elapsed()).arg(copyMs),
                LogSource::Application, LogLevel::Debug);
        }
        // Notify views once for the entire mesh set.
        for (int i = 0; i < meshCount(); ++i)
            emit meshAdded(i);
    }

    {
        QElapsedTimer t; t.start();
        clearAllGpuResources();
        writeLog(tr("Undo/redo — GPU cache clear: %1 ms").arg(t.elapsed()), LogSource::Application, LogLevel::Debug);
    }

    m_nextMeshId = state.nextMeshId;
    {
        // Compare live rasters against the snapshot.  If rasterId, imageRevision,
        // and cameraRevision all match, the raster is unchanged — skip the
        // expensive destroy/recreate + signal cascade (especially costly when
        // RenderWidget holds large GPU raster texture caches).
        QElapsedTimer t; t.start();
        int skipped = 0;
        int removed = 0;
        for (int i = rasterCount() - 1; i >= 0; --i) {
            const auto &live = m_rasters[static_cast<size_t>(i)];
            const auto *snap = [&]() -> const RasterSnapshot * {
                for (const auto &s : state.rasters)
                    if (s.rasterId == live->rasterId) return &s;
                return nullptr;
            }();
            if (snap
                && snap->imageRevision == live->imageRevision
                && snap->cameraRevision == live->cameraRevision) {
                // Raster unchanged — keep it but update cheap metadata in place.
                live->visible = snap->visible;
                live->name = snap->name;
                live->sourcePath = snap->sourcePath;
                live->currentPlaneIndex = snap->currentPlaneIndex;
                ++skipped;
            } else {
                m_rasters.erase(m_rasters.begin() + i);
                emit rasterRemoved(i);
                ++removed;
            }
        }
        int added = 0;
        for (const auto &snap : state.rasters) {
            bool alreadyLive = false;
            for (const auto &live : m_rasters)
                if (live->rasterId == snap.rasterId) { alreadyLive = true; break; }
            if (alreadyLive) continue;
            auto entry = std::make_unique<RasterEntry>();
            entry->rasterId = snap.rasterId;
            entry->imageRevision = snap.imageRevision;
            entry->cameraRevision = snap.cameraRevision;
            entry->name = snap.name;
            entry->sourcePath = snap.sourcePath;
            entry->visible = snap.visible;
            entry->shot = snap.shot;
            entry->planes = snap.planes;
            entry->currentPlaneIndex = snap.currentPlaneIndex;
            m_rasters.push_back(std::move(entry));
            emit rasterAdded(static_cast<int>(m_rasters.size() - 1));
            ++added;
        }
        writeLog(tr("Undo/redo — rasters: %1 ms (skipped %2, removed %3, added %4)")
            .arg(t.elapsed()).arg(skipped).arg(removed).arg(added),
            LogSource::Application, LogLevel::Debug);
    }

    {
        // Suppress index/layer-change signals during undo restore —
        // the meshAdded batch above already notifies all views.
        qint64 sigMs = 0, visMs = 0, vsMs = 0;
        {
            QElapsedTimer t2; t2.start();
            m_nextRasterId = state.nextRasterId;
            const int normalizedCurrent =
                (state.currentMeshIndex >= 0 && state.currentMeshIndex < meshCount())
                ? state.currentMeshIndex
                : -1;
            m_currentMeshIndex = normalizedCurrent;
            emit currentMeshChanged(m_currentMeshIndex);
            const int normalizedRaster =
                (state.currentRasterIndex >= 0 && state.currentRasterIndex < rasterCount())
                ? state.currentRasterIndex
                : -1;
            m_currentRasterIndex = normalizedRaster;
            emit currentRasterChanged(m_currentRasterIndex);
            CurrentLayerKind normalizedLayerKind = CurrentLayerKind::None;
            switch (state.currentLayerKind) {
            case CurrentLayerKind::Mesh:
                normalizedLayerKind =
                    (m_currentMeshIndex >= 0) ? CurrentLayerKind::Mesh : CurrentLayerKind::None;
                break;
            case CurrentLayerKind::Raster:
                normalizedLayerKind =
                    (m_currentRasterIndex >= 0) ? CurrentLayerKind::Raster : CurrentLayerKind::None;
                break;
            case CurrentLayerKind::None:
                normalizedLayerKind = CurrentLayerKind::None;
                break;
            }
            m_currentLayerKind = normalizedLayerKind;
            const int currentLayerIndex =
                (m_currentLayerKind == CurrentLayerKind::Mesh)
                ? m_currentMeshIndex
                : ((m_currentLayerKind == CurrentLayerKind::Raster) ? m_currentRasterIndex : -1);
            emit currentLayerChanged(m_currentLayerKind, currentLayerIndex);
            sigMs = t2.elapsed();
        }
        {
            QElapsedTimer t2; t2.start();
            for (int i = 0; i < meshCount(); ++i) {
                const MeshEntry &entry = mesh(i);
                if (!entry.visible)
                    emit meshVisibilityChanged(i, false);
            }
            for (int i = 0; i < rasterCount(); ++i) {
                const RasterEntry &entry = raster(i);
                if (!entry.visible)
                    emit rasterVisibilityChanged(i, false);
            }
            visMs = t2.elapsed();
        }
        {
            QElapsedTimer t2; t2.start();
            if (m_restoreViewState)
                m_restoreViewState(state.viewState, m_undoManager->restoreCamera());
            vsMs = t2.elapsed();
        }
        m_undoManager->setRestoring(prevRestoring);
        writeLog(tr("Undo/redo — signals + view restore: %1 ms (index %2, vis %3, viewSt %4)")
            .arg(sigMs + visMs + vsMs).arg(sigMs).arg(visMs).arg(vsMs),
            LogSource::Application, LogLevel::Debug);
    }

    // The signals above are all emitted while the restoring flag is set, so every view
    // that guards on isRestoringUndoRedo() skips them by design — that guard exists to
    // avoid one rebuild per restored mesh. This is the batch they were promised: whatever
    // is derived from mesh contents (dynamic filter bounds, filter applicability) is only
    // correct again from here on.
    emit undoRestoreCompleted();
}

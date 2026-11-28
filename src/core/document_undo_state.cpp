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
    state.visibilitySets = m_visibilitySets;
    state.unit = m_unit;
    state.origin = m_origin;
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
        snap.colorRevision      = entry->colorRevision;
        snap.scalarRevision     = entry->scalarRevision;
        snap.transform    = entry->transform;
        snap.meshColor          = entry->mesh.C();
        snap.name              = entry->name;
        snap.sourcePath         = entry->sourcePath;
        snap.textureFileNames   = entry->textureFileNames;
        snap.textureFilePaths   = entry->textureFilePaths;
        snap.textureAssets      = entry->textureAssets;
        snap.materialSet        = entry->materialSet;
        snap.visible            = entry->visible;
        snap.ioMask             = entry->ioMask;
        snap.loadOptions        = entry->loadOptions;
        snap.polygonFaceCount   = entry->polygonFaceCount;
        // Pointers, not payloads: immutability makes sharing the capture.
        snap.pluginData         = entry->pluginData;

        // Reuse the interned geometry and layers when their revisions have not moved since
        // the last capture: actions that touch none of them cost no copy. A miss (the
        // first capture after a change, or after every state holding it was pruned)
        // copies and interns; the caches hold weak_ptrs and keep nothing alive.
        const DocumentUndoManager::GeometryKey geometryKey(
            entry->meshId, entry->geometryRevision, entry->materialRevision);
        auto &geometryCache = m_undoManager->geometryCache();
        if (auto it = geometryCache.find(geometryKey); it != geometryCache.end()) {
            snap.geometry = it->second.mesh.lock();
            snap.geometryLayerRevisions = it->second.layerRevisions;
        }
        if (!snap.geometry) {
            auto g = std::make_shared<VCGMesh>();
            deepCopyMesh(entry->mesh, *g);
            snap.geometry = g;
            for (size_t k = 0; k < meshAttributeKinds.size(); ++k)
                snap.geometryLayerRevisions[k] = attributeRevision(*entry, k);
            geometryCache[geometryKey] = { g, snap.geometryLayerRevisions };
        }
        // A layer only where the state's selection, colors or scalars are not the copy's.
        for (size_t k = 0; k < meshAttributeKinds.size(); ++k) {
            const std::uint64_t revision = attributeRevision(*entry, k);
            if (revision == snap.geometryLayerRevisions[k])
                continue;
            // A layer is relative to the copy, so the key names the copy's geometry too.
            const DocumentUndoManager::AttributeKey key(entry->meshId, entry->geometryRevision,
                                                        meshAttributeKinds[k], revision);
            auto &layer = m_undoManager->attributeCache()[key];
            snap.layers[k] = layer.lock();
            if (!snap.layers[k]) {
                snap.layers[k] = std::make_shared<const MeshAttributeLayer>(
                    captureAttributeValues(entry->mesh, meshAttributeKinds[k], *snap.geometry));
                layer = snap.layers[k];
            }
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
        snap.lens = entry->lens;
        snap.planes = entry->planes;
        snap.currentPlaneIndex = entry->currentPlaneIndex;
        state.rasters.push_back(std::move(snap));
    }
    if (m_captureViewState)
        state.viewState = m_captureViewState();
    return state;
}

void Document::restoreUndoState(const UndoState &state, bool keepUnchangedMeshes)
{
    const bool prevRestoring = m_undoManager->isRestoring();
    m_undoManager->setRestoring(true);

    // Drop the cache entries no state holds any more, so the maps stay as small as the
    // history. The rest stay, those of the states ahead of this one included: a redo
    // brings their revisions back, and the next capture then finds the copy instead of
    // making another.
    const auto dropExpired = [](auto &cache, auto expired) {
        for (auto it = cache.begin(); it != cache.end();)
            it = expired(it->second) ? cache.erase(it) : std::next(it);
    };
    dropExpired(m_undoManager->geometryCache(),
                [](const DocumentUndoManager::GeometryCopy &c) { return c.mesh.expired(); });
    dropExpired(m_undoManager->attributeCache(), [](const auto &layer) { return layer.expired(); });

    {
        QElapsedTimer t; t.start();
        // Suppress per-mesh add/remove signals during undo restoration —
        // LayerWidget and RenderWidget will be rebuilt from the final state
        // via a single meshDataChanged batch below.
        m_undoManager->setRestoring(true);
        // A live layer whose revisions match the snapshot's already holds that content:
        // every change to a mesh moves one of them, and they are never reused. It is kept,
        // geometry and GPU buffers alike, and only its cheap fields are put back; the
        // others are copied out of the snapshot. Layers the state does not have are dropped.
        std::unordered_map<std::uint64_t, std::unique_ptr<MeshEntry>> live;
        for (auto &entry : m_meshes)
            live.emplace(entry->meshId, std::move(entry));
        m_meshes.clear();
        m_meshes.reserve(state.meshes.size());
        {
            qint64 copyMs = 0;
            int kept = 0;
            for (const auto &snap : state.meshes) {
                std::unique_ptr<MeshEntry> entry;
                bool copied = false;
                const auto it = live.find(snap.meshId);
                if (keepUnchangedMeshes && it != live.end()
                    && it->second->geometryRevision == snap.geometryRevision
                    && it->second->materialRevision == snap.materialRevision) {
                    entry = std::move(it->second);
                    live.erase(it);
                    ++kept;
                } else {
                    entry = std::make_unique<MeshEntry>();
                    QElapsedTimer t2; t2.start();
                    deepCopyMesh(*snap.geometry, entry->mesh);
                    copyMs += t2.elapsed();
                    copied = true;
                }
                // A kept layer may differ from the state in its selection, colors or scalars,
                // and a copy holds those of the state that made it: put back the state's own.
                // Without a layer of its own the state has the copy's values.
                for (size_t k = 0; k < meshAttributeKinds.size(); ++k) {
                    const std::uint64_t held =
                        copied ? snap.geometryLayerRevisions[k] : attributeRevision(*entry, k);
                    if (held != attributeRevision(snap, k))
                        applyAttributeValues(entry->mesh, snap.layers[k]
                            ? *snap.layers[k]
                            : MeshAttributeLayer { meshAttributeKinds[k] }, *snap.geometry);
                    attributeRevision(*entry, k) = attributeRevision(snap, k);
                }
                entry->meshId           = snap.meshId;
                entry->geometryRevision = snap.geometryRevision;
                entry->materialRevision = snap.materialRevision;
                entry->transform  = snap.transform;
                entry->mesh.C()         = snap.meshColor;
                entry->name            = snap.name;
                // The layer's file is a fact about the disk, not the history: a save since
                // this state was captured named another one.
                const auto stored = m_storedMeshes.find(snap.meshId);
                entry->sourcePath       = stored != m_storedMeshes.end() ? stored->second.path : snap.sourcePath;
                // The options read the state's file; a file saved since is read with its own.
                entry->loadOptions      = entry->sourcePath == snap.sourcePath ? snap.loadOptions
                                                                               : MeshFilterParameterValues{};
                entry->textureFileNames = snap.textureFileNames;
                entry->textureFilePaths = snap.textureFilePaths;
                entry->textureAssets    = snap.textureAssets;
                entry->materialSet      = snap.materialSet;
                entry->visible          = snap.visible;
                entry->ioMask           = snap.ioMask;
                entry->polygonFaceCount = snap.polygonFaceCount;
                entry->pluginData       = snap.pluginData;
                m_meshes.push_back(std::move(entry));
            }
            // The GPU cache is keyed by revision, so a copied layer simply misses it; only
            // the layers that are gone have buffers nobody will ask for again.
            for (const auto &dropped : live)
                m_gpuCache->purgeMesh(dropped.first);
            writeLog(tr("Undo/redo — mesh restore: %1 ms (%2 of %3 layers kept, deepCopy %4 ms)")
                .arg(t.elapsed()).arg(kept).arg(meshCount()).arg(copyMs),
                LogSource::Application, LogLevel::Debug);
        }
        // Notify views once for the entire mesh set.
        for (int i = 0; i < meshCount(); ++i)
            emit meshAdded(i);
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
            entry->lens = snap.lens;
            entry->planes = snap.planes;
            entry->currentPlaneIndex = snap.currentPlaneIndex;
            m_rasters.push_back(std::move(entry));
            emit rasterAdded(static_cast<int>(m_rasters.size() - 1));
            ++added;
        }
        // The kept rasters stayed where they were and the added ones went to the end, but the
        // order is part of the state too (moveRaster): put them in the snapshot's.
        {
            std::vector<std::unique_ptr<RasterEntry>> ordered;
            ordered.reserve(m_rasters.size());
            for (const auto &snap : state.rasters) {
                for (auto &live : m_rasters) {
                    if (live && live->rasterId == snap.rasterId) {
                        ordered.push_back(std::move(live));
                        break;
                    }
                }
            }
            if (ordered.size() == m_rasters.size())
                m_rasters = std::move(ordered);
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
            if (m_visibilitySets != state.visibilitySets) {
                m_visibilitySets = state.visibilitySets;
                emit visibilitySetsChanged();
            }
            if (m_unit != state.unit) {
                m_unit = state.unit;
                emit unitChanged();
            }
            if (m_origin != state.origin) {
                m_origin = state.origin;
                emit originChanged();
            }
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

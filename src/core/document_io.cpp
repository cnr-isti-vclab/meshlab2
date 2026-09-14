#include "document_internal.h"

using namespace DocumentInternal;

QString Document::openDialogFilter() const
{
    const QString meshFilters = m_pluginManager ? m_pluginManager->openDialogFilter() : QString();
    QStringList filters = meshFilters.split(QStringLiteral(";;"), Qt::SkipEmptyParts);
    if (filters.isEmpty())
        filters << tr("Mesh and Project Files (*.mlp)");
    else
        filters[0].replace(
            QStringLiteral(")"),
            QStringLiteral(" *.mlp)"));
    const QString projectFilter = tr("MeshLab Project (*.mlp)");
    if (!filters.contains(projectFilter))
        filters.insert(filters.size() > 1 ? filters.size() - 1 : 1, projectFilter);
    if (!filters.contains(tr("All Files (*)")))
        filters << tr("All Files (*)");
    return filters.join(QStringLiteral(";;"));
}

QString Document::saveDialogFilter() const
{
    return m_pluginManager->saveDialogFilter();
}

int Document::saveMaskCapability(const QString &filename) const
{
    const QString normalizedFilename = filename.trimmed();
    if (normalizedFilename.isEmpty())
        return 0;
    const MeshIOPlugin *plugin = m_pluginManager->pluginForSave(normalizedFilename);
    if (!plugin)
        return 0;
    return plugin->saveMaskCapability(normalizedFilename);
}


std::vector<Document::ImportPluginInfo> Document::importPluginInfos() const
{
    std::vector<ImportPluginInfo> infos;
    if (!m_pluginManager)
        return infos;

    const auto pluginInfos = m_pluginManager->pluginInfos();
    infos.reserve(pluginInfos.size());
    for (const auto &info : pluginInfos) {
        ImportPluginInfo out;
        out.id = info.id;
        out.name = info.name;
        out.extensions = info.extensions;
        out.capabilities = info.loadCapabilities;
        infos.push_back(std::move(out));
    }
    return infos;
}

std::vector<Document::ExportPluginInfo> Document::exportPluginInfos() const
{
    std::vector<ExportPluginInfo> infos;
    if (!m_pluginManager)
        return infos;

    const auto pluginInfos = m_pluginManager->pluginInfos();
    infos.reserve(pluginInfos.size());
    for (const auto &info : pluginInfos) {
        ExportPluginInfo out;
        out.id = info.id;
        out.name = info.name;
        out.extensions = info.saveExtensions;
        out.capabilities = info.saveCapabilities;
        infos.push_back(std::move(out));
    }
    return infos;
}

QStringList Document::importSupportedExtensions() const
{
    if (!m_pluginManager)
        return {};
    return m_pluginManager->supportedExtensions();
}

QStringList Document::exportSupportedExtensions() const
{
    if (!m_pluginManager)
        return {};
    return m_pluginManager->savableExtensions();
}

QString Document::preferredImportPluginForExtension(const QString &extension) const
{
    if (!m_pluginManager)
        return {};
    return m_pluginManager->preferredPluginForExtension(extension);
}

void Document::setPreferredImportPluginForExtension(const QString &extension, const QString &pluginId)
{
    if (!m_pluginManager)
        return;
    m_pluginManager->setPreferredPluginForExtension(extension, pluginId);
}

int Document::loadMesh(const QString &filename, QString *errorMessage)
{
    const auto reportError = [errorMessage](const QString &message) {
        if (errorMessage)
            *errorMessage = message;
    };
    if (errorMessage)
        errorMessage->clear();

    const MeshIOPlugin *plugin = m_pluginManager->pluginFor(filename);
    if (!plugin) {
        writeLog(tr("No plugin found for: %1").arg(filename), LogSource::Application);
        reportError(tr("no importer handles .%1 files")
                        .arg(QFileInfo(filename).suffix().toLower()));
        return -1;
    }

    const bool ownUndoStep = !m_undoManager->isRestoring() && !m_undoManager->isStepActive();
    if (ownUndoStep) {
        ScriptAction sa;
        sa.kind = QStringLiteral("load_mesh");
        sa.filePaths = QStringList{filename};
        beginUndoStep(tr("Open Mesh"), sa);
    }

    // Naming the plugin matters because three of them accept .obj and the choice is
    // invisible otherwise: which one ran is the first thing you need when a file that
    // opens elsewhere fails here.
    writeLog(tr("Loading mesh: %1 (%2)").arg(filename, plugin->name()), LogSource::Application);

    auto entry = std::make_unique<MeshEntry>();
    m_lastProgressPos = -1;
    m_loadCallbackCount = 0;
    m_loadProgressEmitCount = 0;
    m_loadProcessEventsCount = 0;
    m_loadProcessEventsNs = 0;
    m_lastProgressEmitMs = -1;
    m_lastProcessEventsMs = -1;
    m_loadCallbackTimer.invalidate();
    m_loadCallbackTimer.start();
    QElapsedTimer loadTimer;
    loadTimer.start();
    emit loadProgressStarted(filename);
    emit loadProgressUpdated(0, tr("Opening %1").arg(QFileInfo(filename).fileName()));

    Document *previousCallbackDocument = g_callbackDocument;
    const CallbackMode previousCallbackMode = m_callbackMode;
    m_callbackMode = CallbackMode::Load;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    g_callbackDocument = this;
    int loadMask = 0;
    MeshIOMaterialSet loadedMaterialSet;
    // Pre-enable storable OCF fields so the importer can write into them.
    entry->mesh.vert.EnableTexCoord();
    entry->mesh.face.EnableWedgeTexCoord();
    int err = plugin->load(filename, entry->mesh, logCallback(), &loadMask, &loadedMaterialSet);
    // Disable storable OCF fields that the importer did not actually populate.
    if (!(loadMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD))  entry->mesh.vert.DisableTexCoord();
    if (!(loadMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD))  entry->mesh.face.DisableWedgeTexCoord();
    g_callbackDocument = previousCallbackDocument;
    m_callbackMode = previousCallbackMode;
    clearProgressLog();
    const qint64 importElapsedMs = loadTimer.elapsed();

    if (err != 0 && plugin->isLoadErrorCritical(filename, err)) {
        emit loadProgressFinished(false, plugin->errorString(err));
        writeLog(tr("Load failed in %1 ms: %2")
            .arg(importElapsedMs)
            .arg(plugin->errorString(err)),
            LogSource::Application);
        reportError(plugin->errorString(err));
        if (ownUndoStep)
            endUndoStep(false);
        return err;
    }
    if (err != 0)
        writeLog(tr("Load warning: %1").arg(plugin->errorString(err)), LogSource::Application, LogLevel::Warning);

    // Framework invariant: meshes entering the document from IO must not
    // retain deleted elements in storage.
    compactMeshStorageInvariant(entry->mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(entry->mesh);
    const bool hasImportedVertexNormals = (loadMask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
    if (!hasImportedVertexNormals && entry->mesh.FN() > 0) {
        // Preserve imported vertex normals exactly as provided by the file.
        // Generate smooth normals only when they are missing.
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry->mesh);
    }
    entry->ioMask = loadMask;
    entry->meshId = m_nextMeshId++;
    entry->geometryRevision = m_nextGeometryRevision++;
    entry->materialRevision = 1;
    entry->transform.setToIdentity();
    entry->name = QFileInfo(filename).fileName();
    entry->sourcePath = filename;
    entry->materialSet = normalizeMaterialSet(filename, loadedMaterialSet, entry->mesh);

    QStringList declaredTextureNames;
    QStringList resolvedTexturePaths;
    std::vector<std::string> normalizedMeshTexturePaths;
    QString selectedTextureName;
    bool selectedExistingTexture = false;
    for (const std::string &rawTextureName : entry->mesh.textures) {
        const QString textureName = QString::fromStdString(rawTextureName).trimmed();
        if (textureName.isEmpty())
            continue;
        declaredTextureNames.append(textureName);
        const QString resolvedTexturePath = resolveTexturePath(filename, textureName);
        resolvedTexturePaths.append(resolvedTexturePath);
        entry->textureFileNames.append(textureName);
        entry->textureFilePaths.append(resolvedTexturePath);
        normalizedMeshTexturePaths.push_back(QDir::toNativeSeparators(resolvedTexturePath).toStdString());
        if (selectedTextureName.isEmpty()) {
            selectedTextureName = textureName;
        }
        if (!selectedExistingTexture && QFileInfo::exists(resolvedTexturePath)) {
            selectedTextureName = textureName;
            selectedExistingTexture = true;
        }
    }
    if (!normalizedMeshTexturePaths.empty())
        entry->mesh.textures = std::move(normalizedMeshTexturePaths);
    entry->textureAssets = buildTextureAssetsFromLegacyAssociation(
        entry->textureFileNames,
        entry->textureFilePaths,
        entry->mesh.textures,
        loadedMaterialSet.textureAssets);

    // Also register any PBR channel textures from materialSet that were not listed in
    // mesh.textures (e.g. glTF normal / occlusion / roughness maps that have no UV slot).
    for (const MeshIOMaterialSlot &slot : entry->materialSet.entries) {
        auto addIfNew = [&](const MeshIOMaterialTextureRef &ref) {
            const QString p = ref.filePath.trimmed();
            if (p.isEmpty() || entry->textureFilePaths.contains(p))
                return;
            entry->textureFileNames.append(QFileInfo(p).fileName());
            entry->textureFilePaths.append(p);
        };
        addIfNew(slot.baseColorTexture);
        addIfNew(slot.normalTexture);
        addIfNew(slot.occlusionTexture);
        addIfNew(slot.roughnessTexture);
    }

    int index = meshCount();
    m_meshes.push_back(std::move(entry));
    refreshMeshPolygonFaceCount(index);

    const qint64 elapsedMs = loadTimer.elapsed();
    const qint64 postProcessElapsedMs = std::max<qint64>(0, elapsedMs - importElapsedMs);
    const MeshEntry &meshEntry = mesh(index);
    writeLog(tr("Loaded mesh '%1' in %2 ms (%3 vertices, %4 faces, %5 edges)")
        .arg(meshEntry.name)
        .arg(elapsedMs)
        .arg(meshEntry.mesh.VN())
        .arg(meshEntry.mesh.FN())
        .arg(meshEntry.mesh.EN()),
        LogSource::Application);
    if (elapsedMs >= 250) {
        writeLog(tr("Load timing '%1': import %2 ms, post %3 ms")
                .arg(meshEntry.name)
                .arg(importElapsedMs)
                .arg(postProcessElapsedMs),
            LogSource::Application, LogLevel::Debug);
    }
    if (m_loadCallbackCount > 0) {
        const float processEventsMs = float(m_loadProcessEventsNs / 1000000.0);
        writeLog(tr("Load callback stats '%1': %2 calls, %3 UI updates, %4 event pumps (%5 ms)")
                .arg(meshEntry.name)
                .arg(m_loadCallbackCount)
                .arg(m_loadProgressEmitCount)
                .arg(m_loadProcessEventsCount)
                .arg(QString::number(processEventsMs, 'f', 2)),
            LogSource::Application, LogLevel::Debug);
    }
    writeLog(tr("File info for '%1': %2 (mask: 0x%3)")
        .arg(meshEntry.name)
        .arg(summarizeLoadMask(loadMask))
        .arg(QString::number(static_cast<quint32>(loadMask), 16).toUpper()),
        LogSource::Application, LogLevel::Debug);
    if (!declaredTextureNames.isEmpty()) {
        int existingTextureFiles = 0;
        for (const QString &path : meshEntry.textureFilePaths) {
            if (QFileInfo::exists(path))
                ++existingTextureFiles;
        }
        writeLog(tr("Texture info for '%1': declared [%2], resolved [%3], selected '%4' (%5/%6 files found)")
            .arg(meshEntry.name)
            .arg(declaredTextureNames.join(QStringLiteral(", ")))
            .arg(resolvedTexturePaths.join(QStringLiteral(", ")))
            .arg(selectedTextureName.isEmpty() ? tr("none") : selectedTextureName)
            .arg(existingTextureFiles)
            .arg(meshEntry.textureFilePaths.size()),
            LogSource::Application, LogLevel::Debug);
    }
    if (!meshEntry.materialSet.empty()) {
        int baseCount = 0;
        int normalCount = 0;
        int aoCount = 0;
        int roughnessCount = 0;
        for (const MeshIOMaterialSlot &slot : meshEntry.materialSet.entries) {
            if (slot.baseColorTexture.isValid())
                ++baseCount;
            if (slot.normalTexture.isValid())
                ++normalCount;
            if (slot.occlusionTexture.isValid())
                ++aoCount;
            if (slot.roughnessTexture.isValid())
                ++roughnessCount;
        }
        writeLog(
            tr("Material info for '%1': %2 slot(s), base=%3, normal=%4, ao=%5, roughness=%6")
                .arg(meshEntry.name)
                .arg(meshEntry.materialSet.entries.size())
                .arg(baseCount)
                .arg(normalCount)
                .arg(aoCount)
                .arg(roughnessCount),
            LogSource::Application);
    }

    emit meshAdded(index);
    setCurrentMeshIndex(index);
    emit loadProgressUpdated(100, tr("Loaded %1").arg(meshEntry.name));
    emit loadProgressFinished(true, tr("Loaded %1").arg(meshEntry.name));
    if (ownUndoStep)
        endUndoStep(true);
    return 0;
}

int Document::reloadMesh(int index)
{
    if (index < 0 || index >= meshCount()) {
        writeLog(tr("Cannot reload: invalid mesh index %1").arg(index), LogSource::Application);
        return -1;
    }

    MeshEntry &entry = mesh(index);
    const QString sourcePath = entry.sourcePath.trimmed();
    if (sourcePath.isEmpty()) {
        writeLog(
            tr("Cannot reload mesh '%1': missing source file path").arg(entry.name),
            LogSource::Application);
        return -2;
    }

    const MeshIOPlugin *plugin = m_pluginManager->pluginFor(sourcePath);
    if (!plugin) {
        writeLog(
            tr("Cannot reload mesh '%1': no plugin found for %2").arg(entry.name, sourcePath),
            LogSource::Application);
        return -3;
    }

    const bool ownUndoStep = !m_undoManager->isRestoring() && !m_undoManager->isStepActive();
    if (ownUndoStep)
        beginUndoStep(tr("Reload Mesh"));

    const QString oldName = entry.name;
    writeLog(
        tr("Reloading mesh '%1' from %2").arg(oldName, sourcePath),
        LogSource::Application);

    m_lastProgressPos = -1;
    m_loadCallbackCount = 0;
    m_loadProgressEmitCount = 0;
    m_loadProcessEventsCount = 0;
    m_loadProcessEventsNs = 0;
    m_lastProgressEmitMs = -1;
    m_lastProcessEventsMs = -1;
    m_loadCallbackTimer.invalidate();
    m_loadCallbackTimer.start();
    QElapsedTimer loadTimer;
    loadTimer.start();
    emit loadProgressStarted(sourcePath);
    emit loadProgressUpdated(0, tr("Reloading %1").arg(QFileInfo(sourcePath).fileName()));

    VCGMesh reloadedMesh;
    Document *previousCallbackDocument = g_callbackDocument;
    const CallbackMode previousCallbackMode = m_callbackMode;
    m_callbackMode = CallbackMode::Load;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    g_callbackDocument = this;
    int loadMask = 0;
    MeshIOMaterialSet loadedMaterialSet;
    // Pre-enable storable OCF fields so the importer can write into them.
    reloadedMesh.vert.EnableTexCoord();
    reloadedMesh.face.EnableWedgeTexCoord();
    const int err = plugin->load(sourcePath, reloadedMesh, logCallback(), &loadMask, &loadedMaterialSet);
    // Disable storable OCF fields that the importer did not actually populate.
    if (!(loadMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD))  reloadedMesh.vert.DisableTexCoord();
    if (!(loadMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD))  reloadedMesh.face.DisableWedgeTexCoord();
    g_callbackDocument = previousCallbackDocument;
    m_callbackMode = previousCallbackMode;
    clearProgressLog();
    const qint64 importElapsedMs = loadTimer.elapsed();

    if (err != 0 && plugin->isLoadErrorCritical(sourcePath, err)) {
        emit loadProgressFinished(false, plugin->errorString(err));
        writeLog(
            tr("Reload failed in %1 ms: %2")
                .arg(importElapsedMs)
                .arg(plugin->errorString(err)),
            LogSource::Application);
        if (ownUndoStep)
            endUndoStep(false);
        return err;
    }
    if (err != 0)
        writeLog(tr("Reload warning: %1").arg(plugin->errorString(err)), LogSource::Application, LogLevel::Warning);

    // Framework invariant: meshes entering the document from IO must not
    // retain deleted elements in storage.
    compactMeshStorageInvariant(reloadedMesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(reloadedMesh);
    const bool hasImportedVertexNormals = (loadMask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
    if (!hasImportedVertexNormals && reloadedMesh.FN() > 0) {
        // Preserve imported vertex normals exactly as provided by the file.
        // Generate smooth normals only when they are missing.
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(reloadedMesh);
    }

    QStringList declaredTextureNames;
    QStringList resolvedTexturePaths;
    QStringList textureFileNames;
    QStringList textureFilePaths;
    std::vector<std::string> normalizedMeshTexturePaths;
    QString selectedTextureName;
    bool selectedExistingTexture = false;
    for (const std::string &rawTextureName : reloadedMesh.textures) {
        const QString textureName = QString::fromStdString(rawTextureName).trimmed();
        if (textureName.isEmpty())
            continue;
        declaredTextureNames.append(textureName);
        const QString resolvedTexturePath = resolveTexturePath(sourcePath, textureName);
        resolvedTexturePaths.append(resolvedTexturePath);
        textureFileNames.append(textureName);
        textureFilePaths.append(resolvedTexturePath);
        normalizedMeshTexturePaths.push_back(QDir::toNativeSeparators(resolvedTexturePath).toStdString());
        if (selectedTextureName.isEmpty())
            selectedTextureName = textureName;
        if (!selectedExistingTexture && QFileInfo::exists(resolvedTexturePath)) {
            selectedTextureName = textureName;
            selectedExistingTexture = true;
        }
    }
    if (!normalizedMeshTexturePaths.empty())
        reloadedMesh.textures = std::move(normalizedMeshTexturePaths);

    deepCopyMesh(reloadedMesh, entry.mesh);
    entry.ioMask = loadMask;
    entry.name = QFileInfo(sourcePath).fileName();
    entry.sourcePath = sourcePath;
    entry.textureFileNames = textureFileNames;
    entry.textureFilePaths = textureFilePaths;
    entry.textureAssets = buildTextureAssetsFromLegacyAssociation(
        entry.textureFileNames,
        entry.textureFilePaths,
        reloadedMesh.textures,
        loadedMaterialSet.textureAssets);
    entry.materialSet = normalizeMaterialSet(sourcePath, loadedMaterialSet, reloadedMesh);
    entry.geometryRevision = m_nextGeometryRevision++;
    ++entry.materialRevision;
    entry.modified = false;
    refreshMeshPolygonFaceCount(index);

    const qint64 elapsedMs = loadTimer.elapsed();
    const qint64 postProcessElapsedMs = std::max<qint64>(0, elapsedMs - importElapsedMs);
    writeLog(tr("Reloaded mesh '%1' in %2 ms (%3 vertices, %4 faces, %5 edges)")
        .arg(entry.name)
        .arg(elapsedMs)
        .arg(entry.mesh.VN())
        .arg(entry.mesh.FN())
        .arg(entry.mesh.EN()),
        LogSource::Application);
    if (elapsedMs >= 250) {
        writeLog(tr("Reload timing '%1': import %2 ms, post %3 ms")
                .arg(entry.name)
                .arg(importElapsedMs)
                .arg(postProcessElapsedMs),
            LogSource::Application, LogLevel::Debug);
    }
    if (m_loadCallbackCount > 0) {
        const float processEventsMs = float(m_loadProcessEventsNs / 1000000.0);
        writeLog(tr("Reload callback stats '%1': %2 calls, %3 UI updates, %4 event pumps (%5 ms)")
                .arg(entry.name)
                .arg(m_loadCallbackCount)
                .arg(m_loadProgressEmitCount)
                .arg(m_loadProcessEventsCount)
                .arg(QString::number(processEventsMs, 'f', 2)),
            LogSource::Application, LogLevel::Debug);
    }
    writeLog(tr("File info for '%1': %2 (mask: 0x%3)")
        .arg(entry.name)
        .arg(summarizeLoadMask(loadMask))
        .arg(QString::number(static_cast<quint32>(loadMask), 16).toUpper()),
        LogSource::Application, LogLevel::Debug);
    if (!declaredTextureNames.isEmpty()) {
        int existingTextureFiles = 0;
        for (const QString &path : entry.textureFilePaths) {
            if (QFileInfo::exists(path))
                ++existingTextureFiles;
        }
        writeLog(tr("Texture info for '%1': declared [%2], resolved [%3], selected '%4' (%5/%6 files found)")
            .arg(entry.name)
            .arg(declaredTextureNames.join(QStringLiteral(", ")))
            .arg(resolvedTexturePaths.join(QStringLiteral(", ")))
            .arg(selectedTextureName.isEmpty() ? tr("none") : selectedTextureName)
            .arg(existingTextureFiles)
            .arg(entry.textureFilePaths.size()),
            LogSource::Application, LogLevel::Debug);
    }
    if (!entry.materialSet.empty()) {
        int baseCount = 0;
        int normalCount = 0;
        int aoCount = 0;
        int roughnessCount = 0;
        for (const MeshIOMaterialSlot &slot : entry.materialSet.entries) {
            if (slot.baseColorTexture.isValid())
                ++baseCount;
            if (slot.normalTexture.isValid())
                ++normalCount;
            if (slot.occlusionTexture.isValid())
                ++aoCount;
            if (slot.roughnessTexture.isValid())
                ++roughnessCount;
        }
        writeLog(
            tr("Material info for '%1': %2 slot(s), base=%3, normal=%4, ao=%5, roughness=%6")
                .arg(entry.name)
                .arg(entry.materialSet.entries.size())
                .arg(baseCount)
                .arg(normalCount)
                .arg(aoCount)
                .arg(roughnessCount),
            LogSource::Application);
    }

    emit meshDataChanged(index);
    emit loadProgressUpdated(100, tr("Reloaded %1").arg(entry.name));
    emit loadProgressFinished(true, tr("Reloaded %1").arg(entry.name));
    if (ownUndoStep)
        endUndoStep(true);
    return 0;
}

int Document::saveMesh(int index, const QString &filename, const MeshIOSaveOptions &options)
{
    if (index < 0 || index >= meshCount()) {
        writeLog(tr("Cannot save: invalid mesh index %1").arg(index), LogSource::Application);
        return -1;
    }
    const QString normalizedFilename = filename.trimmed();
    if (normalizedFilename.isEmpty()) {
        writeLog(tr("Cannot save: empty target file path"), LogSource::Application);
        return -2;
    }

    const MeshIOPlugin *plugin = m_pluginManager->pluginForSave(normalizedFilename);
    if (!plugin) {
        writeLog(
            tr("No export plugin found for: %1").arg(normalizedFilename),
            LogSource::Application);
        return -3;
    }

    MeshEntry &entry = mesh(index);
    QElapsedTimer timer;
    timer.start();

    writeLog(
        tr("Saving mesh '%1' to %2")
            .arg(entry.name, normalizedFilename),
        LogSource::Application);

    Document *previousCallbackDocument = g_callbackDocument;
    const CallbackMode previousCallbackMode = m_callbackMode;
    m_callbackMode = CallbackMode::Save;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    g_callbackDocument = this;
    const MeshIOTextureContext textureContext{
        &entry.textureFileNames,
        &entry.textureFilePaths,
        &entry.textureAssets,
        &entry.materialSet
    };
    const int err = plugin->save(
        normalizedFilename,
        entry.mesh,
        options,
        logCallback(),
        &textureContext);
    g_callbackDocument = previousCallbackDocument;
    m_callbackMode = previousCallbackMode;
    clearProgressLog();

    const qint64 elapsedMs = timer.elapsed();
    if (err != 0) {
        writeLog(
            tr("Save failed in %1 ms: %2")
                .arg(elapsedMs)
                .arg(plugin->errorString(err)),
            LogSource::Application);
        return err;
    }

    writeLog(
        tr("Saved mesh '%1' to %2 in %3 ms")
            .arg(entry.name, normalizedFilename)
            .arg(elapsedMs),
        LogSource::Application);
    return 0;
}

int Document::saveMesh(int index, const QString &filename)
{
    return saveMesh(index, filename, MeshIOSaveOptions{});
}

int Document::saveCurrentMesh(const QString &filename, const MeshIOSaveOptions &options)
{
    if (m_currentMeshIndex < 0 || m_currentMeshIndex >= meshCount()) {
        writeLog(tr("Cannot save: no current mesh selected"), LogSource::Application);
        return -1;
    }
    return saveMesh(m_currentMeshIndex, filename, options);
}

int Document::saveCurrentMesh(const QString &filename)
{
    return saveCurrentMesh(filename, MeshIOSaveOptions{});
}

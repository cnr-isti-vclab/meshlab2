#include "document_internal.h"
#include "textureassociationutils.h"

#include <QImageWriter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

using namespace DocumentInternal;

QString Document::openDialogFilter() const
{
    const QString meshFilters = m_pluginManager ? m_pluginManager->openDialogFilter() : QString();
    QStringList filters = meshFilters.split(QStringLiteral(";;"), Qt::SkipEmptyParts);
    if (filters.isEmpty())
        filters << tr("Mesh and Project Files (*.mlp cameras.txt cameras.bin)");
    else
        filters[0].replace(
            QStringLiteral(")"),
            QStringLiteral(" *.mlp cameras.txt cameras.bin)"));
    for (const QString &projectFilter : { tr("MeshLab Project (*.mlp)"),
                                          tr("COLMAP Model (cameras.txt cameras.bin)") }) {
        if (!filters.contains(projectFilter))
            filters.insert(filters.size() > 1 ? filters.size() - 1 : 1, projectFilter);
    }
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


int Document::availableSaveMask(const MeshEntry &entry)
{
    int mask = entry.ioMask;
    mask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    if (entry.mesh.FN() > 0) {
        mask |= vcg::tri::io::Mask::IOM_FACEINDEX;
        // Polygonal export is an output choice, not an optional mesh component:
        // disabling it deliberately writes the underlying triangulation.
        mask |= vcg::tri::io::Mask::IOM_BITPOLYGONAL;
    }
    if (entry.mesh.EN() > 0)
        mask |= vcg::tri::io::Mask::IOM_EDGEINDEX;
    return mask;
}

int Document::requiredSaveMask(const MeshEntry &entry, int capabilityMask)
{
    int requiredMask = 0;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_VERTCOORD) != 0 && entry.mesh.VN() > 0)
        requiredMask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_FACEINDEX) != 0 && entry.mesh.FN() > 0)
        requiredMask |= vcg::tri::io::Mask::IOM_FACEINDEX;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_EDGEINDEX) != 0 && entry.mesh.EN() > 0)
        requiredMask |= vcg::tri::io::Mask::IOM_EDGEINDEX;
    return requiredMask;
}

int Document::defaultSaveMask(const MeshEntry &entry, int capabilityMask)
{
    const int availableMask = availableSaveMask(entry);
    int mask = availableMask & capabilityMask;
    mask |= requiredSaveMask(entry, capabilityMask);
    if ((entry.ioMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL) == 0)
        mask &= ~vcg::tri::io::Mask::IOM_BITPOLYGONAL;
    // Prefer wedge attributes over per-vertex ones when both are available.
    if ((mask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTTEXCOORD;
    if ((mask & vcg::tri::io::Mask::IOM_WEDGNORMAL) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTNORMAL;
    return mask;
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

Document::CallbackScope::CallbackScope(Document &doc, CallbackMode mode)
    : m_doc(doc), m_previousDocument(g_callbackDocument), m_previousMode(doc.m_callbackMode)
{
    doc.m_callbackMode = mode;
    doc.m_cancelRequested.store(false, std::memory_order_relaxed);
    g_callbackDocument = &doc;
}

Document::CallbackScope::~CallbackScope()
{
    g_callbackDocument = m_previousDocument;
    m_doc.m_callbackMode = m_previousMode;
    m_doc.clearProgressLog();
}

std::vector<MeshFilterParameterDescriptor> Document::loadParameters(const QString &filename) const
{
    const MeshIOPlugin *plugin = m_pluginManager ? m_pluginManager->pluginFor(filename) : nullptr;
    return plugin ? plugin->loadParameters(filename) : std::vector<MeshFilterParameterDescriptor>{};
}

QString Document::importPluginId(const QString &filename) const
{
    const MeshIOPlugin *plugin = m_pluginManager ? m_pluginManager->pluginFor(filename) : nullptr;
    return plugin ? plugin->pluginId() : QString();
}

void Document::setRememberedLoadOptions(const QString &pluginId, const MeshFilterParameterValues &values)
{
    if (values.isEmpty())
        m_rememberedLoadOptions.remove(pluginId);
    else
        m_rememberedLoadOptions.insert(pluginId, values);
}

bool Document::resolveLoadOptions(const MeshIOPlugin &plugin, const QString &filename,
    const MeshFilterParameterValues &given, MeshFilterParameterValues &resolved,
    MeshFilterParameterValues &changed, QString &error) const
{
    resolved.clear();
    changed.clear();
    const std::vector<MeshFilterParameterDescriptor> declared = plugin.loadParameters(filename);
    const MeshFilterParameterValues remembered = m_rememberedLoadOptions.value(plugin.pluginId());
    QStringList known;
    for (const MeshFilterParameterDescriptor &parameter : declared) {
        known << parameter.id;
        // A remembered value that no longer fits (the declaration changed) is passed over.
        QVariant value = parameter.defaultValue;
        QVariant converted;
        QString ignored;
        if (remembered.contains(parameter.id)
            && MeshFilterPluginManager::convertParameterValue(parameter, remembered.value(parameter.id), converted, ignored))
            value = converted;
        if (given.contains(parameter.id)) {
            if (!MeshFilterPluginManager::convertParameterValue(parameter, given.value(parameter.id), value, error))
                return false;
        }
        resolved.insert(parameter.id, value);
        if (!sameParameterValue(value, parameter.defaultValue))
            changed.insert(parameter.id, value);
    }
    for (auto it = given.constBegin(); it != given.constEnd(); ++it) {
        if (!known.contains(it.key())) {
            error = known.isEmpty()
                ? tr("%1 files have no load options, and '%2' was given.").arg(QFileInfo(filename).suffix().toUpper(), it.key())
                : tr("Unknown load option '%1' for %2 files: the options are %3.")
                      .arg(it.key(), QFileInfo(filename).suffix().toUpper(), known.join(QStringLiteral(", ")));
            return false;
        }
    }
    return true;
}

void Document::logLoadContext(const MeshIOLoadContext &context, const MeshIOPlugin &plugin, const QString &filename)
{
    const QString fileName = QFileInfo(filename).fileName();
    for (const QString &message : context.messages)
        writeLog(tr("'%1': %2").arg(fileName, message), LogSource::Application);
    if (context.options.isEmpty())
        return;
    QStringList used;
    for (const MeshFilterParameterDescriptor &parameter : plugin.loadParameters(filename))
        used << QStringLiteral("%1 = %2").arg(parameter.label,
            parameter.type == MeshFilterParameterType::Bool
                ? (context.options.value(parameter.id).toBool() ? tr("on") : tr("off"))
                : context.options.value(parameter.id).toString());
    writeLog(tr("Read '%1' with %2. Reload with Options reads it again with others.")
                 .arg(fileName, used.join(QStringLiteral(", "))),
        LogSource::Application);
}

Document::ImportedTextureInfo Document::prepareImportedMesh(
    MeshEntry &entry, const QString &filename, int loadMask, const MeshIOMaterialSet &loadedMaterialSet)
{
    // Framework invariant: meshes entering the document from IO must not
    // retain deleted elements in storage.
    compactMeshStorageInvariant(entry.mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);
    computeMissingNormals(entry.mesh, loadMask);
    entry.ioMask = loadMask;
    entry.materialSet = normalizeMaterialSet(filename, loadedMaterialSet, entry.mesh);

    // Texture names are resolved against the file, and the mesh keeps the resolved paths.
    ImportedTextureInfo info;
    entry.textureFileNames.clear();
    entry.textureFilePaths.clear();
    std::vector<std::string> normalizedMeshTexturePaths;
    bool selectedExistingTexture = false;
    for (const std::string &rawTextureName : entry.mesh.textures) {
        const QString textureName = QString::fromStdString(rawTextureName).trimmed();
        if (textureName.isEmpty())
            continue;
        info.declared.append(textureName);
        const QString resolvedTexturePath = resolveTexturePath(filename, textureName);
        info.resolved.append(resolvedTexturePath);
        entry.textureFileNames.append(textureName);
        entry.textureFilePaths.append(resolvedTexturePath);
        normalizedMeshTexturePaths.push_back(QDir::toNativeSeparators(resolvedTexturePath).toStdString());
        if (info.selected.isEmpty())
            info.selected = textureName;
        if (!selectedExistingTexture && QFileInfo::exists(resolvedTexturePath)) {
            info.selected = textureName;
            selectedExistingTexture = true;
        }
    }
    if (!normalizedMeshTexturePaths.empty())
        entry.mesh.textures = std::move(normalizedMeshTexturePaths);
    entry.textureAssets = buildTextureAssetsFromLegacyAssociation(
        entry.textureFileNames,
        entry.textureFilePaths,
        entry.mesh.textures,
        loadedMaterialSet.textureAssets);

    // Also register any channel textures from materialSet that were not listed in
    // mesh.textures (e.g. glTF normal / occlusion / metallic-roughness maps that have no UV slot).
    for (const MeshIOMaterialSlot &slot : entry.materialSet.entries) {
        forEachTextureRef(slot, [&](MeshIOTextureRole, const MeshIOMaterialTextureRef &ref) {
            const QString p = ref.filePath.trimmed();
            if (p.isEmpty() || entry.textureFilePaths.contains(p))
                return;
            entry.textureFileNames.append(QFileInfo(p).fileName());
            entry.textureFilePaths.append(p);
        });
    }
    return info;
}

void Document::logImportedMesh(const MeshEntry &entry, int loadMask, const ImportedTextureInfo &textures)
{
    writeLog(tr("File info for '%1': %2 (mask: 0x%3)")
        .arg(entry.name)
        .arg(summarizeLoadMask(loadMask))
        .arg(QString::number(static_cast<quint32>(loadMask), 16).toUpper()),
        LogSource::Application, LogLevel::Debug);
    if (!textures.declared.isEmpty()) {
        int existingTextureFiles = 0;
        for (const QString &path : entry.textureFilePaths) {
            if (QFileInfo::exists(path))
                ++existingTextureFiles;
        }
        writeLog(tr("Texture info for '%1': declared [%2], resolved [%3], selected '%4' (%5/%6 files found)")
            .arg(entry.name)
            .arg(textures.declared.join(QStringLiteral(", ")))
            .arg(textures.resolved.join(QStringLiteral(", ")))
            .arg(textures.selected.isEmpty() ? tr("none") : textures.selected)
            .arg(existingTextureFiles)
            .arg(entry.textureFilePaths.size()),
            LogSource::Application, LogLevel::Debug);
    }
    if (!entry.materialSet.empty()) {
        int count[6] = {};
        for (const MeshIOMaterialSlot &slot : entry.materialSet.entries)
            forEachTextureRef(slot, [&](MeshIOTextureRole role, const MeshIOMaterialTextureRef &ref) {
                count[int(role)] += ref.isValid() ? 1 : 0;
            });
        writeLog(
            tr("Material info for '%1': %2 slot(s), base=%3, normal=%4, ao=%5, roughness=%6, metallic=%7, emissive=%8")
                .arg(entry.name)
                .arg(entry.materialSet.entries.size())
                .arg(count[0]).arg(count[1]).arg(count[2]).arg(count[3]).arg(count[4]).arg(count[5]),
            LogSource::Application);
    }
}

int Document::loadMesh(const QString &filename, QString *errorMessage, const QString &label,
                       const QMatrix4x4 *transform, const MeshFilterParameterValues &options)
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

    MeshFilterParameterValues loadOptions, changedOptions;
    QString optionError;
    if (!resolveLoadOptions(*plugin, filename, options, loadOptions, changedOptions, optionError)) {
        writeLog(tr("Cannot load %1: %2").arg(filename, optionError), LogSource::Application, LogLevel::Warning);
        reportError(optionError);
        return -1;
    }

    const bool ownUndoStep = !m_undoManager->isRestoring() && !m_undoManager->isStepActive();
    if (ownUndoStep) {
        ScriptAction sa;
        sa.kind = QStringLiteral("load_mesh");
        sa.filePaths = QStringList{filename};
        sa.params = changedOptions;
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

    std::optional<CallbackScope> callbacks(std::in_place, *this, CallbackMode::Load);
    int loadMask = 0;
    MeshIOMaterialSet loadedMaterialSet;
    MeshIOLoadContext loadContext = this->loadContext();
    loadContext.options = loadOptions;
    // Pre-enable storable OCF fields so the importer can write into them.
    entry->mesh.vert.EnableTexCoord();
    entry->mesh.face.EnableWedgeTexCoord();
    int err = plugin->load(filename, entry->mesh, logCallback(), &loadMask, &loadedMaterialSet, loadContext);
    // Disable storable OCF fields that the importer did not actually populate.
    if (!(loadMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD))  entry->mesh.vert.DisableTexCoord();
    if (!(loadMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD))  entry->mesh.face.DisableWedgeTexCoord();
    callbacks.reset();
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

    adoptFileOrigin(loadContext, entry->mesh, filename, meshCount() == 0);
    entry->loadOptions = loadOptions;
    entry->meshId = m_nextMeshId++;
    entry->geometryRevision = m_nextGeometryRevision++;
    entry->materialRevision = m_nextGeometryRevision++;
    if (transform)
        entry->transform = *transform;
    else
        entry->transform.setToIdentity();
    const QString trimmedLabel = label.trimmed();
    entry->name = trimmedLabel.isEmpty() ? QFileInfo(filename).fileName() : trimmedLabel;
    entry->sourcePath = filename;
    const ImportedTextureInfo textureInfo = prepareImportedMesh(*entry, filename, loadMask, loadedMaterialSet);

    recordMeshStored(*entry, filename, nullptr);
    adoptDirectoryOf(filename);
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
    logLoadContext(loadContext, *plugin, filename);
    adoptFileUnit(loadContext.unit, filename, meshCount() == 1);
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
    logImportedMesh(meshEntry, loadMask, textureInfo);

    emit meshAdded(index);
    setCurrentMeshIndex(index);
    // The photos the file holds become rasters, in this undo step. Not inside a project: it
    // lists its rasters itself, these among them once it has been saved with them.
    if (!m_bulkLoading) {
        QStringList warnings;
        // Relative to the origin the points were loaded with, the document's by now if any.
        if (!m_origin)
            loadContext.origin.reset();
        const std::vector<MeshIORaster> rasters = plugin->loadRasters(filename, loadContext, warnings);
        for (const QString &warning : warnings)
            writeLog(warning, LogSource::Application, LogLevel::Warning);
        for (const MeshIORaster &raster : rasters)
            addRasterImage(raster.image, raster.name, QString(), CameraShot::fromVcgShot(raster.shot));
        if (!rasters.empty())
            writeLog(tr("Added %n raster(s) from '%1'", nullptr, int(rasters.size())).arg(meshEntry.name),
                LogSource::Application);
    }
    emit loadProgressUpdated(100, tr("Loaded %1").arg(meshEntry.name));
    emit loadProgressFinished(true, tr("Loaded %1").arg(meshEntry.name));
    if (ownUndoStep)
        endUndoStep(true);
    return 0;
}

int Document::reloadMesh(int index, const MeshFilterParameterValues *options, QString *errorMessage)
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

    // The options given, or those the layer was read with, as far as the format still
    // declares them.
    MeshFilterParameterValues given = options ? *options : MeshFilterParameterValues{};
    if (!options) {
        for (const MeshFilterParameterDescriptor &parameter : plugin->loadParameters(sourcePath))
            if (entry.loadOptions.contains(parameter.id))
                given.insert(parameter.id, entry.loadOptions.value(parameter.id));
    }
    MeshFilterParameterValues loadOptions, changedOptions;
    QString optionError;
    if (!resolveLoadOptions(*plugin, sourcePath, given, loadOptions, changedOptions, optionError)) {
        writeLog(tr("Cannot reload mesh '%1': %2").arg(entry.name, optionError),
            LogSource::Application, LogLevel::Warning);
        if (errorMessage)
            *errorMessage = optionError;
        return -4;
    }

    const bool ownUndoStep = !m_undoManager->isRestoring() && !m_undoManager->isStepActive();
    if (ownUndoStep && options) {
        // Recorded so a script reads the file with the options the user ended with.
        ScriptAction sa;
        sa.kind = QStringLiteral("reload_mesh");
        sa.filePaths = QStringList{sourcePath};
        sa.params = changedOptions;
        sa.currentMeshIndex = index;
        beginUndoStep(tr("Reload with Options"), sa);
    } else if (ownUndoStep) {
        beginUndoStep(tr("Reload Mesh"));
    }

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

    // Read into a layer of its own first: the live one is replaced only once it all worked.
    MeshEntry staged;
    VCGMesh &reloadedMesh = staged.mesh;
    std::optional<CallbackScope> callbacks(std::in_place, *this, CallbackMode::Load);
    int loadMask = 0;
    MeshIOMaterialSet loadedMaterialSet;
    // Pre-enable storable OCF fields so the importer can write into them.
    reloadedMesh.vert.EnableTexCoord();
    reloadedMesh.face.EnableWedgeTexCoord();
    MeshIOLoadContext loadContext = this->loadContext();
    loadContext.options = loadOptions;
    const int err = plugin->load(sourcePath, reloadedMesh, logCallback(), &loadMask, &loadedMaterialSet, loadContext);
    // Disable storable OCF fields that the importer did not actually populate.
    if (!(loadMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD))  reloadedMesh.vert.DisableTexCoord();
    if (!(loadMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD))  reloadedMesh.face.DisableWedgeTexCoord();
    callbacks.reset();
    const qint64 importElapsedMs = loadTimer.elapsed();

    if (err != 0 && plugin->isLoadErrorCritical(sourcePath, err)) {
        emit loadProgressFinished(false, plugin->errorString(err));
        writeLog(
            tr("Reload failed in %1 ms: %2")
                .arg(importElapsedMs)
                .arg(plugin->errorString(err)),
            LogSource::Application);
        if (errorMessage)
            *errorMessage = plugin->errorString(err);
        if (ownUndoStep)
            endUndoStep(false);
        return err;
    }
    if (err != 0)
        writeLog(tr("Reload warning: %1").arg(plugin->errorString(err)), LogSource::Application, LogLevel::Warning);

    adoptFileOrigin(loadContext, reloadedMesh, sourcePath, meshCount() == 1);
    const ImportedTextureInfo textureInfo = prepareImportedMesh(staged, sourcePath, loadMask, loadedMaterialSet);

    deepCopyMesh(reloadedMesh, entry.mesh);
    entry.ioMask = staged.ioMask;
    entry.loadOptions = loadOptions;
    // The name is the user's label, not the file's: reloading the contents keeps it.
    entry.sourcePath = sourcePath;
    entry.textureFileNames = staged.textureFileNames;
    entry.textureFilePaths = staged.textureFilePaths;
    entry.textureAssets = staged.textureAssets;
    entry.materialSet = staged.materialSet;
    entry.geometryRevision = m_nextGeometryRevision++;
    entry.materialRevision = m_nextGeometryRevision++;
    recordMeshStored(entry, sourcePath, nullptr);
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
    logLoadContext(loadContext, *plugin, sourcePath);
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
    logImportedMesh(entry, loadMask, textureInfo);

    emit meshDataChanged(index);
    emit loadProgressUpdated(100, tr("Reloaded %1").arg(entry.name));
    emit loadProgressFinished(true, tr("Reloaded %1").arg(entry.name));
    if (ownUndoStep)
        endUndoStep(true);
    return 0;
}

void Document::adoptFileUnit(LengthUnit fileUnit, const QString &filename, bool onlyLayers)
{
    if (fileUnit == LengthUnit::Unknown || fileUnit == m_unit)
        return;
    const QString fileName = QFileInfo(filename).fileName();
    if (m_unit != LengthUnit::Unknown) {
        writeLog(tr("'%1' is in %2 and the document's unit is %3: nothing was converted.")
                     .arg(fileName, LengthUnits::name(fileUnit).toLower(),
                          LengthUnits::name(m_unit).toLower()),
                 LogSource::Application, LogLevel::Warning);
    } else if (onlyLayers) {
        setUnit(fileUnit);
        writeLog(tr("The unit of the document is %1, as '%2' says.")
                     .arg(LengthUnits::name(fileUnit).toLower(), fileName),
                 LogSource::Application);
    } else {
        writeLog(tr("'%1' is in %2, but the document has other layers and no unit: it was left unset.")
                     .arg(fileName, LengthUnits::name(fileUnit).toLower()),
                 LogSource::Application);
    }
}

MeshIOLoadContext Document::loadContext() const
{
    MeshIOLoadContext context;
    context.origin = m_origin;
    return context;
}

void Document::adoptFileOrigin(const MeshIOLoadContext &context, VCGMesh &mesh, const QString &filename, bool onlyLayer)
{
    if (!context.origin)
        return;
    const QString fileName = QFileInfo(filename).fileName();
    vcg::Box3d box;
    for (const auto &v : mesh.vert)
        box.Add(vcg::Point3d::Construct(v.cP()));
    // Half the float step at the largest coordinate: how far a position may be from the file's.
    const auto precision = [&box] {
        const double m = std::max({ std::abs(box.min[0]), std::abs(box.min[1]), std::abs(box.min[2]),
            std::abs(box.max[0]), std::abs(box.max[1]), std::abs(box.max[2]) });
        const float f = float(m);
        return QString::number((std::nextafter(f, std::numeric_limits<float>::infinity()) - f) / 2, 'g', 2);
    };
    if (context.origin == m_origin) {
        if (isFarFromZero(box))
            writeLog(tr("'%1' lies far from the document origin %2: its positions are kept only to %3. "
                        "Open it in a document of its own to keep its precision.")
                         .arg(fileName, originText(*m_origin), precision()),
                LogSource::Application, LogLevel::Warning);
        return;
    }
    if (!m_origin && onlyLayer && rasterCount() == 0 && !m_bulkLoading) {
        setOrigin(context.origin);
        writeLog(tr("The document origin is %1, as '%2' lies far from zero: positions are relative to it.")
                     .arg(originText(*m_origin), fileName),
            LogSource::Application);
        return;
    }
    // Back to the file's coordinates, which the other layers share.
    for (auto &v : mesh.vert)
        v.P() = vcg::Point3f::Construct(vcg::Point3d::Construct(v.cP()) + *context.origin);
    box.Translate(*context.origin);
    writeLog(tr("'%1' lies far from zero, and the document already has layers: its positions are kept only to %2. "
                "Open it in a document of its own to keep its precision.")
                 .arg(fileName, precision()),
        LogSource::Application, LogLevel::Warning);
}

std::vector<MeshIOTextureAsset> Document::texturesToSave(const MeshEntry &entry) const
{
    // A texture whose image a file holds is handed to the saver with that file, which it
    // copies or refers to; one no file holds goes without, and is written from its image.
    // The asset's own path cannot say which: undo may have brought back an image that a
    // later save wrote somewhere else, and a filter that replaced a texture's image leaves
    // the path of the file it came from. A texture with no image is its file.
    std::vector<MeshIOTextureAsset> assets = entry.textureAssets;
    for (MeshIOTextureAsset &asset : assets) {
        if (!asset.hasImage())
            continue;
        const auto stored = m_storedImages.find(asset.image.cacheKey());
        asset.sourcePath = stored == m_storedImages.end() || stored->second.packed
            ? QString()
            : QDir::toNativeSeparators(stored->second.path);
    }
    return assets;
}

int Document::maskToSave(const MeshEntry &entry, int requestedMask, int capabilityMask)
{
    // No mask asks for what the layer holds, as the Save dialog proposes; left to the exporter, it
    // asked for everything the format can hold, and a project or Python save wrote white colors
    // and zero scalars the layer never had.
    int mask = requestedMask != 0 ? requestedMask : defaultSaveMask(entry, capabilityMask);
    // Texture coordinates are optional components: a mask asking for ones the mesh has not
    // allocated made an exporter read storage that does not exist (the glTF writer crashed on
    // a mesh with faces and no UVs). The bits for them are dropped here, for every exporter.
    if (!vcg::tri::HasPerVertexTexCoord(entry.mesh))
        mask &= ~vcg::tri::io::Mask::IOM_VERTTEXCOORD;
    if (!vcg::tri::HasPerWedgeTexCoord(entry.mesh))
        mask &= ~(vcg::tri::io::Mask::IOM_WEDGTEXCOORD | vcg::tri::io::Mask::IOM_WEDGTEXMULTI);
    return mask;
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

    std::optional<CallbackScope> callbacks(std::in_place, *this, CallbackMode::Save);
    std::vector<MeshIOTextureAsset> savedAssets = texturesToSave(entry);
    QStringList savedTexturePaths;
    const MeshIOTextureContext textureContext{
        &entry.textureFileNames,
        &entry.textureFilePaths,
        &savedAssets,
        &entry.materialSet,
        &savedTexturePaths
    };
    // The formats that record a unit get the document's.
    MeshIOSaveOptions saveOptions = options;
    saveOptions.unit = m_unit;
    saveOptions.origin = m_origin.value_or(vcg::Point3d(0, 0, 0));
    saveOptions.mask = maskToSave(entry, saveOptions.mask, plugin->saveMaskCapability(normalizedFilename));
    const int err = plugin->save(
        normalizedFilename,
        entry.mesh,
        saveOptions,
        logCallback(),
        &textureContext);
    callbacks.reset();

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
    if (m_origin && !plugin->isGeoreferenced(normalizedFilename))
        writeLog(tr("'%1' holds coordinates relative to the document origin %2: add it to get true ones.")
                     .arg(QFileInfo(normalizedFilename).fileName(), originText(*m_origin)),
            LogSource::Application);

    // Saving names the layer's file, as in any document application, and a label that was
    // still the old file's name follows it. Not an undo step: it is a fact about the disk.
    const QString savedPath = QFileInfo(normalizedFilename).absoluteFilePath();
    if (!entry.sourcePath.isEmpty() && entry.name == QFileInfo(entry.sourcePath).fileName())
        entry.name = QFileInfo(savedPath).fileName();
    // Another file is now the layer's, read with its own format's defaults; the same file
    // keeps the options that read it.
    if (QFileInfo(entry.sourcePath).absoluteFilePath() != savedPath)
        entry.loadOptions.clear();
    entry.sourcePath = savedPath;
    recordMeshStored(entry, savedPath, &savedTexturePaths);
    adoptDirectoryOf(savedPath);
    emit meshDataChanged(index);
    return 0;
}

void Document::setDirectory(const QString &directory)
{
    const QString dir = directory.trimmed().isEmpty() ? QString() : QDir::cleanPath(QDir(directory).absolutePath());
    if (dir == m_directory)
        return;
    m_directory = dir;
    if (!dir.isEmpty())
        writeLog(tr("Document folder: %1").arg(QDir::toNativeSeparators(dir)), LogSource::Application);
    emit directoryChanged();
}

void Document::adoptDirectoryOf(const QString &file)
{
    if (m_directory.isEmpty())
        setDirectory(QFileInfo(file).absolutePath());
}

QString Document::displayPath(const QString &path) const
{
    if (path.trimmed().isEmpty() || m_directory.isEmpty())
        return QDir::toNativeSeparators(path);
    const QString relative = QDir(m_directory).relativeFilePath(QFileInfo(path).absoluteFilePath());
    if (QDir::isAbsolutePath(relative) || relative.startsWith(QStringLiteral("../../")))
        return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    return QDir::toNativeSeparators(relative);
}

void Document::recordMeshStored(MeshEntry &entry, const QString &path, const QStringList *savedTexturePaths)
{
    const QString file = QFileInfo(path).absoluteFilePath();
    m_storedMeshes[entry.meshId] = { file, entry.geometryRevision, entry.materialRevision,
                                     entry.selectionRevision, entry.colorRevision, entry.scalarRevision };
    bool repointed = false;
    for (int i = 0; i < int(entry.textureAssets.size()); ++i) {
        MeshIOTextureAsset &asset = entry.textureAssets[size_t(i)];
        QString stored;
        if (!savedTexturePaths)
            stored = asset.hasImage() && asset.sourcePath.trimmed().isEmpty() ? file : QString();
        else if (i < savedTexturePaths->size())
            stored = savedTexturePaths->at(i);
        if (stored.isEmpty())
            continue;
        if (stored == file) {
            if (asset.hasImage())
                m_storedImages[asset.image.cacheKey()] = { file, true };
            continue;
        }
        if (asset.hasImage())
            m_storedImages[asset.image.cacheKey()] = { stored, false };
        asset.sourcePath = QDir::toNativeSeparators(stored);
        repointed = true;
    }
    if (repointed)
        TextureAssociationUtils::rebuildLegacyTextureAssociation(entry);
}

Document::StoredState Document::meshStoredState(int index) const
{
    if (index < 0 || index >= meshCount())
        return StoredState::NotStored;
    const MeshEntry &entry = mesh(index);
    const auto stored = m_storedMeshes.find(entry.meshId);
    if (stored == m_storedMeshes.end())
        return StoredState::NotStored;
    const StoredMesh &s = stored->second;
    return s.geometryRevision == entry.geometryRevision && s.materialRevision == entry.materialRevision
            && s.selectionRevision == entry.selectionRevision
            && s.colorRevision == entry.colorRevision && s.scalarRevision == entry.scalarRevision
        ? StoredState::Stored
        : StoredState::Changed;
}

Document::StoredState Document::rasterPlaneStoredState(int rasterIndex, int planeIndex) const
{
    if (rasterIndex < 0 || rasterIndex >= rasterCount())
        return StoredState::NotStored;
    const RasterEntry &entry = raster(rasterIndex);
    if (planeIndex < 0 || planeIndex >= int(entry.planes.size()))
        return StoredState::NotStored;
    const RasterPlane &plane = entry.planes[size_t(planeIndex)];
    const bool onDisk = QFileInfo(plane.sourcePath.trimmed()).isFile();
    if (!onDisk)
        return StoredState::NotStored;
    // Not decoded yet, or decoded and untouched: the file holds it.
    if (!plane.hasImage() || plane.image.cacheKey() == plane.sourceImageKey)
        return StoredState::Stored;
    return StoredState::Changed;
}

namespace {
// The format comes from the suffix: whatever the installed image plugins can write.
bool writeImageFile(const QImage &image, const QString &file, QString &error)
{
    QImageWriter writer(file);
    if (writer.write(image))
        return true;
    error = QObject::tr("Cannot save the image to %1: %2").arg(QDir::toNativeSeparators(file), writer.errorString());
    return false;
}
}

bool Document::saveMeshTexture(int meshIndex, int textureIndex, const QString &path, QString *error)
{
    QString message;
    const auto fail = [&] {
        writeLog(message, LogSource::Application, LogLevel::Warning);
        if (error)
            *error = message;
        return false;
    };
    if (meshIndex < 0 || meshIndex >= meshCount()
        || textureIndex < 0 || textureIndex >= int(mesh(meshIndex).textureAssets.size())) {
        message = tr("Cannot save texture %1: the layer has no such texture.").arg(textureIndex);
        return fail();
    }
    MeshEntry &entry = mesh(meshIndex);
    MeshIOTextureAsset &asset = entry.textureAssets[size_t(textureIndex)];
    QImage image = asset.image;
    if (image.isNull() && !TextureAssociationUtils::readImageFile(asset.sourcePath, image, message))
        return fail();
    const QString file = QFileInfo(path.trimmed()).absoluteFilePath();
    if (!writeImageFile(image, file, message))
        return fail();

    const bool moved = asset.sourcePath.trimmed().isEmpty()
        || QFileInfo(asset.sourcePath.trimmed()).absoluteFilePath() != file;
    asset.sourcePath = QDir::toNativeSeparators(file);
    if (asset.hasImage())
        m_storedImages[asset.image.cacheKey()] = { file, false };
    TextureAssociationUtils::rebuildLegacyTextureAssociation(entry);
    // The mesh file still names the texture's old file, or none.
    if (moved)
        entry.materialRevision = m_nextGeometryRevision++;
    writeLog(tr("Saved texture '%1' of '%2' to %3")
                 .arg(meshTextureDisplayName(entry, textureIndex), entry.name, QDir::toNativeSeparators(file)),
             LogSource::Application);
    emit meshDataChanged(meshIndex);
    return true;
}

bool Document::saveRasterPlaneImage(int rasterIndex, int planeIndex, const QString &path, QString *error)
{
    QString message;
    if (rasterIndex < 0 || rasterIndex >= rasterCount()
        || planeIndex < 0 || planeIndex >= int(raster(rasterIndex).planes.size())) {
        message = tr("Cannot save plane %1: the raster has no such plane.").arg(planeIndex);
    } else {
        RasterPlane &plane = raster(rasterIndex).planes[size_t(planeIndex)];
        ensureRasterPlaneImage(raster(rasterIndex), plane);
        const QString file = QFileInfo(path.trimmed()).absoluteFilePath();
        if (plane.image.isNull()) {
            message = tr("Cannot save plane %1: it has no image.").arg(planeIndex);
        } else if (writeImageFile(plane.image, file, message)) {
            plane.sourcePath = QDir::toNativeSeparators(file);
            plane.sourceUndistorted = true; // what is in memory, already through the lens
            plane.sourceImageKey = plane.image.cacheKey();
            writeLog(tr("Saved plane %1 of '%2' to %3")
                         .arg(planeIndex).arg(raster(rasterIndex).name, QDir::toNativeSeparators(file)),
                     LogSource::Application);
            emit rasterDataChanged(rasterIndex);
            return true;
        }
    }
    writeLog(message, LogSource::Application, LogLevel::Warning);
    if (error)
        *error = message;
    return false;
}

Document::StoredState Document::meshTextureStoredState(int meshIndex, int textureIndex) const
{
    if (meshIndex < 0 || meshIndex >= meshCount())
        return StoredState::NotStored;
    const MeshEntry &entry = mesh(meshIndex);
    if (textureIndex < 0 || textureIndex >= int(entry.textureAssets.size()))
        return StoredState::NotStored;
    const MeshIOTextureAsset &asset = entry.textureAssets[size_t(textureIndex)];
    const bool hasFile = QFileInfo(asset.sourcePath.trimmed()).isFile();
    if (!asset.hasImage())
        return hasFile ? StoredState::Stored : StoredState::NotStored;
    if (m_storedImages.count(asset.image.cacheKey()))
        return StoredState::Stored;
    // An image no load or save recorded is one a filter made, or put in place of the one its
    // file holds.
    return hasFile ? StoredState::Changed : StoredState::NotStored;
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

bool Document::holdsScenes(const QString &filename) const
{
    const MeshIOPlugin *plugin = m_pluginManager ? m_pluginManager->pluginFor(filename) : nullptr;
    return plugin && plugin->holdsScenes(filename);
}

int Document::importLayers(const QString &filename, QString *errorMessage, const MeshFilterParameterValues &options)
{
    const auto fail = [&](const QString &why, int code) {
        writeLog(tr("Cannot import layers from %1: %2").arg(filename, why), LogSource::Application, LogLevel::Warning);
        if (errorMessage)
            *errorMessage = why;
        return code;
    };
    const MeshIOPlugin *plugin = m_pluginManager->pluginFor(filename);
    if (!plugin || !plugin->holdsScenes(filename))
        return fail(tr(".%1 files hold no scene of layers").arg(QFileInfo(filename).suffix().toLower()), -1);
    MeshFilterParameterValues loadOptions, changedOptions;
    QString optionError;
    if (!resolveLoadOptions(*plugin, filename, options, loadOptions, changedOptions, optionError))
        return fail(optionError, -1);

    const bool ownUndoStep = !m_undoManager->isRestoring() && !m_undoManager->isStepActive();
    if (ownUndoStep) {
        ScriptAction sa;
        sa.kind = QStringLiteral("import_layers");
        sa.filePaths = QStringList{filename};
        sa.params = changedOptions;
        beginUndoStep(tr("Import Layers"), sa);
    }
    writeLog(tr("Importing layers: %1 (%2)").arg(filename, plugin->name()), LogSource::Application);
    emit loadProgressStarted(filename);

    std::vector<MeshIOSceneLayer> layers;
    MeshIOLoadContext context = loadContext();
    context.options = loadOptions;
    int err = 0;
    {
        CallbackScope callbacks(*this, CallbackMode::Load);
        err = plugin->loadScene(filename, layers, logCallback(), context);
    }
    if (err != 0) {
        emit loadProgressFinished(false, plugin->errorString(err));
        if (ownUndoStep)
            endUndoStep(false);
        return fail(plugin->errorString(err), err);
    }

    const bool documentWasEmpty = meshCount() == 0;
    int firstIndex = -1;
    for (MeshIOSceneLayer &layer : layers) {
        auto entry = std::make_unique<MeshEntry>();
        deepCopyMesh(*layer.mesh, entry->mesh);
        layer.mesh.reset();
        if (!(layer.loadMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD))
            entry->mesh.vert.DisableTexCoord();
        if (!(layer.loadMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD))
            entry->mesh.face.DisableWedgeTexCoord();
        adoptFileOrigin(context, entry->mesh, filename, documentWasEmpty && firstIndex < 0);
        entry->loadOptions = loadOptions;
        entry->meshId = m_nextMeshId++;
        entry->geometryRevision = m_nextGeometryRevision++;
        entry->materialRevision = m_nextGeometryRevision++;
        entry->transform = layer.transform;
        entry->visible = layer.visible;
        entry->name = uniqueMeshName(layer.label.trimmed().isEmpty() ? QFileInfo(filename).completeBaseName()
                                                                     : layer.label.trimmed());
        const ImportedTextureInfo textureInfo = prepareImportedMesh(*entry, filename, layer.loadMask, layer.materialSet);
        const int index = meshCount();
        m_meshes.push_back(std::move(entry));
        refreshMeshPolygonFaceCount(index);
        logImportedMesh(mesh(index), layer.loadMask, textureInfo);
        emit meshAdded(index);
        if (firstIndex < 0)
            firstIndex = index;
    }
    writeLog(tr("Imported %n layer(s) from '%1'", nullptr, int(layers.size())).arg(QFileInfo(filename).fileName()),
        LogSource::Application);
    logLoadContext(context, *plugin, filename);
    adoptFileUnit(context.unit, filename, documentWasEmpty);
    adoptDirectoryOf(filename);
    if (firstIndex >= 0)
        setCurrentMeshIndex(firstIndex);
    emit loadProgressFinished(true, tr("Imported %1").arg(QFileInfo(filename).fileName()));
    if (ownUndoStep)
        endUndoStep(true);
    return 0;
}

int Document::exportVisibleLayers(const QString &filename, QString *errorMessage)
{
    const auto fail = [&](const QString &why, int code) {
        writeLog(tr("Cannot export layers to %1: %2").arg(filename, why), LogSource::Application, LogLevel::Warning);
        if (errorMessage)
            *errorMessage = why;
        return code;
    };
    const MeshIOPlugin *plugin = m_pluginManager->pluginForSave(filename);
    if (!plugin || !plugin->holdsScenes(filename))
        return fail(tr(".%1 files hold no scene of layers").arg(QFileInfo(filename).suffix().toLower()), -1);

    // What each visible layer hands the exporter, as saveMesh prepares it for one; the texture
    // lists live here for as long as the views point at them.
    const int capability = plugin->saveMaskCapability(filename);
    std::vector<int> visible;
    for (int i = 0; i < meshCount(); ++i)
        if (mesh(i).visible)
            visible.push_back(i);
    if (visible.empty())
        return fail(tr("no layer is visible"), -2);
    std::vector<std::vector<MeshIOTextureAsset>> assets(visible.size());
    std::vector<MeshIOTextureContext> contexts(visible.size());
    std::vector<MeshIOSceneLayerView> views(visible.size());
    for (size_t k = 0; k < visible.size(); ++k) {
        MeshEntry &entry = mesh(visible[k]);
        assets[k] = texturesToSave(entry);
        contexts[k] = { &entry.textureFileNames, &entry.textureFilePaths, &assets[k], &entry.materialSet, nullptr };
        views[k].mesh = &entry.mesh;
        views[k].mask = maskToSave(entry, 0, capability);
        views[k].transform = entry.transform;
        views[k].label = entry.name;
        views[k].textures = &contexts[k];
    }
    MeshIOSaveOptions options;
    options.unit = m_unit;
    options.origin = m_origin.value_or(vcg::Point3d(0, 0, 0));
    int err = 0;
    {
        CallbackScope callbacks(*this, CallbackMode::Save);
        err = plugin->saveScene(filename, views, options, logCallback());
    }
    if (err != 0)
        return fail(plugin->errorString(err), err);
    writeLog(tr("Exported %n visible layer(s) to %1", nullptr, int(views.size())).arg(filename), LogSource::Application);
    return 0;
}

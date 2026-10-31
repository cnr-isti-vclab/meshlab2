#include "document_internal.h"

using namespace DocumentInternal;

int Document::loadMeshLabProject(const QString &filename)
{
    const QString normalizedFilename = filename.trimmed();
    if (normalizedFilename.isEmpty())
        return -1;

    std::vector<MeshLabProjectMeshEntry> projectMeshes;
    std::vector<MeshLabProjectRasterEntry> projectRasters;
    std::vector<MeshLabProjectVisibilitySetEntry> projectSets;
    LengthUnit projectUnit = LengthUnit::Unknown;
    QString parseError;
    if (!parseMeshLabProjectFile(
            normalizedFilename, projectMeshes, projectRasters, projectSets, projectUnit, parseError)) {
        writeLog(parseError, LogSource::Application);
        return -1;
    }

    const bool ownUndoStep = !m_undoManager->isRestoring() && !m_undoManager->isStepActive();
    if (ownUndoStep) {
        ScriptAction sa;
        sa.kind = QStringLiteral("load_project");
        sa.filePaths = QStringList{filename};
        beginUndoStep(tr("Open MeshLab Project"), sa);
    }

    QElapsedTimer projectTimer;
    projectTimer.start();
    writeLog(tr("Loading MeshLab project: %1").arg(normalizedFilename), LogSource::Application);

    const qint64 xmlParseMs = projectTimer.elapsed();
    m_bulkLoading = true;
    int loadedMeshes = 0;
    int loadedRasters = 0;

    // The unit the project says is the user's word, so it comes before the layers, whose own
    // files then have to agree with it. A project that says nothing leaves the unit as it is:
    // its layers may be joining others.
    if (projectUnit != LengthUnit::Unknown)
        setUnit(projectUnit);

    // The id each entry of the file got, by its position in the file; 0 for one that was skipped.
    // What the visibility sets name.
    std::vector<std::uint64_t> idOfOrdinal(projectMeshes.size(), 0);
    for (size_t ordinal = 0; ordinal < projectMeshes.size(); ++ordinal) {
        const MeshLabProjectMeshEntry &projectMesh = projectMeshes[ordinal];
        const QString meshPath = projectMesh.sourcePath.trimmed();
        if (meshPath.isEmpty()) {
            writeLog(
                tr("Project mesh '%1' has no filename and was skipped")
                    .arg(projectMesh.label.isEmpty() ? tr("unnamed") : projectMesh.label),
                LogSource::Application, LogLevel::Warning);
            continue;
        }
        if (!QFileInfo::exists(meshPath)) {
            writeLog(
                tr("Project mesh file is missing: %1").arg(meshPath),
                LogSource::Application);
            continue;
        }

        const int meshesBefore = meshCount();
        const int result = loadMesh(meshPath, nullptr, projectMesh.label,
                                    projectMesh.hasTransform ? &projectMesh.transform : nullptr);
        if (result != 0) {
            writeLog(
                tr("Failed to load project mesh: %1").arg(meshPath),
                LogSource::Application, LogLevel::Warning);
            continue;
        }
        ++loadedMeshes;
        if (meshCount() > meshesBefore)
            idOfOrdinal[ordinal] = mesh(meshCount() - 1).meshId;
    }

    for (const MeshLabProjectVisibilitySetEntry &projectSet : projectSets) {
        std::vector<std::uint64_t> ids;
        for (const int ordinal : projectSet.meshOrdinals) {
            if (ordinal < int(idOfOrdinal.size()) && idOfOrdinal[ordinal] != 0)
                ids.push_back(idOfOrdinal[ordinal]);
        }
        // A set none of whose layers is here would show nothing, which is not what it is for.
        if (!ids.empty()) {
            addVisibilitySet(
                projectSet.name.isEmpty() ? tr("Set %1").arg(visibilitySets().size() + 1)
                                          : projectSet.name,
                std::move(ids));
        }
    }

    int rasterIdx = 0;
    for (const MeshLabProjectRasterEntry &projectRaster : projectRasters) {
        QElapsedTimer rt;
        rt.start();
        RasterEntry rasterEntry;
        rasterEntry.name = projectRaster.label;
        rasterEntry.shot = projectRaster.shot;
        rasterEntry.visible = true;

        for (const MeshLabProjectPlaneEntry &projectPlane : projectRaster.planes) {
            RasterPlane plane;
            plane.semantic = projectPlane.semantic;
            plane.name = projectPlane.name;
            plane.sourcePath = projectPlane.sourcePath;

            if (!projectPlane.sourcePath.trimmed().isEmpty() && QFileInfo::exists(projectPlane.sourcePath)) {
                QImageReader reader(projectPlane.sourcePath);
                plane.size = reader.size();
            } else if (!projectPlane.sourcePath.trimmed().isEmpty()) {
                writeLog(
                    tr("Project raster plane file is missing: %1").arg(projectPlane.sourcePath),
                    LogSource::Application);
            }

            if (!plane.size.isValid() || plane.size.width() <= 0)
                plane.size = projectRaster.shot.viewportPx();
            rasterEntry.planes.push_back(std::move(plane));
        }
        // The last plane is the current one, as in MeshLab, where each plane read became
        // current: a plane added to a raster is written after the image it was added to.
        rasterEntry.currentPlaneIndex = int(rasterEntry.planes.size()) - 1;

        if (rasterEntry.planes.empty()) {
            RasterPlane plane;
            plane.semantic = RasterPlaneSemantic::RGBA;
            plane.size = projectRaster.shot.viewportPx();
            rasterEntry.planes.push_back(std::move(plane));
            rasterEntry.currentPlaneIndex = 0;
        }

        if (rasterEntry.sourcePath.trimmed().isEmpty() && !rasterEntry.planes.empty())
            rasterEntry.sourcePath = rasterEntry.planes.front().sourcePath.trimmed();

        if (addRaster(rasterEntry) >= 0) {
            const qint64 addMs = rt.elapsed();
            const RasterPlane *p = rasterEntry.currentPlane();
            const QSize sz = p ? p->size : QSize();
            writeLog(
                tr("Raster %1/%2: %3 ms — %4%5")
                    .arg(rasterIdx + 1)
                    .arg(projectRasters.size())
                    .arg(addMs)
                    .arg(rasterEntry.name)
                    .arg(sz.isValid() ? tr(" (%1x%2)").arg(sz.width()).arg(sz.height()) : QString()),
                LogSource::Application, LogLevel::Debug);
            ++loadedRasters;
        }
        ++rasterIdx;
    }

    m_bulkLoading = false;
    emit bulkLoadFinished();
    // Emit a single rasterAdded to trigger one rebuild for all loaded rasters
    if (loadedRasters > 0)
        emit rasterAdded(rasterCount() - 1);
    const qint64 rasterLoopMs = projectTimer.elapsed() - xmlParseMs;
    if (rasterCount() > 0 && m_currentRasterIndex < 0)
        setCurrentRasterIndex(rasterCount() - 1);
    const bool success = (loadedMeshes + loadedRasters) > 0;
    writeLog(
        tr("MeshLab project load: %1 ms total (XML %2 ms, rasters %3 ms)")
            .arg(projectTimer.elapsed())
            .arg(xmlParseMs)
            .arg(rasterLoopMs),
        LogSource::Application, LogLevel::Debug);
    writeLog(
        tr("MeshLab project import complete: %1 mesh(es), %2 raster(s)")
            .arg(loadedMeshes)
            .arg(loadedRasters),
        LogSource::Application);

    m_bulkLoading = false;
    if (success)
        m_projectFilePath = QFileInfo(normalizedFilename).absoluteFilePath();
    if (ownUndoStep)
        endUndoStep(success);
    return success ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Project saving
// ---------------------------------------------------------------------------

bool Document::saveMeshLabProject(
    const QString &filename,
    const MeshLabProjectSaveOptions &options,
    QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error) *error = msg;
        return false;
    };

    const QDir projectDir = QFileInfo(filename).absoluteDir();
    if (!projectDir.exists()) {
        if (!projectDir.mkpath(QStringLiteral(".")))
            return fail(tr("Cannot create project directory: %1").arg(projectDir.absolutePath()));
    }

    // A file is in the project when it is in its folder or below.
    const auto insideProject = [&](const QString &path) {
        const QString abs = QFileInfo(path).absoluteFilePath();
        return abs.startsWith(projectDir.absolutePath() + QLatin1Char('/'))
            || QFileInfo(abs).absolutePath() == projectDir.absolutePath();
    };
    // Files the project writes go into one flat folder each, under the layer's name made safe
    // for every reader and suffixed when the name is taken on disk or by this save: two
    // layers called "scan" must not end up in one file.
    QSet<QString> takenNames;
    const auto uniqueFile = [&](const QString &folder, const QString &name, const QString &fallback,
                                const QString &suffix) {
        static const QRegularExpression unsafe(QStringLiteral("[^\\w.-]"));
        QString stem = QFileInfo(name.simplified()).completeBaseName();
        stem.replace(unsafe, QStringLiteral("_"));
        if (stem.isEmpty())
            stem = fallback;
        const QDir dir(projectDir.filePath(folder));
        QString candidate = stem + QLatin1Char('.') + suffix;
        for (int i = 1; takenNames.contains(dir.filePath(candidate).toLower()) || dir.exists(candidate); ++i)
            candidate = QStringLiteral("%1_%2.%3").arg(stem).arg(i).arg(suffix);
        takenNames.insert(dir.filePath(candidate).toLower());
        return dir.filePath(candidate);
    };

    // Phase 1: what happens to each layer. A layer with no file exists only in memory -- a
    // filter's output, a duplicate -- so it is always written: leaving it out would silently
    // drop it from the project. A layer whose file does not hold it any more is written back
    // over that file when the option asks. A file outside the project folder is refused
    // unless the option to copy files is on; then the layer is saved into the project, with
    // its textures, and the project names that copy. Otherwise the file is referenced.
    enum class MeshAction { Skip, Reference, Save, SaveCopy };
    struct MeshPlan {
        MeshAction action = MeshAction::Skip;
        QString absPath;
    };
    QVector<MeshPlan> meshPlans(meshCount());
    bool outsideFiles = false;
    for (int mi = 0; mi < meshCount(); ++mi) {
        const MeshEntry &entry = mesh(mi);
        if (options.onlyVisibleMeshes && !entry.visible)
            continue;
        MeshPlan &plan = meshPlans[mi];
        const bool hasFile = !entry.sourcePath.isEmpty();
        if (hasFile && !insideProject(entry.sourcePath)) {
            outsideFiles = true;
            QString suffix = QFileInfo(entry.sourcePath).suffix().toLower();
            if (!m_pluginManager->pluginForSave(QStringLiteral("x.") + suffix))
                suffix = QStringLiteral("ply");
            plan = { MeshAction::SaveCopy, uniqueFile(QStringLiteral("meshes"), entry.name,
                                                      QStringLiteral("mesh_%1").arg(mi), suffix) };
        } else if (!hasFile) {
            plan = { MeshAction::Save, uniqueFile(QStringLiteral("meshes"), entry.name,
                                                  QStringLiteral("mesh_%1").arg(mi), QStringLiteral("ply")) };
        } else if (options.saveModifiedMeshes && meshStoredState(mi) != StoredState::Stored) {
            plan = { MeshAction::Save, entry.sourcePath };
        } else {
            plan = { MeshAction::Reference, entry.sourcePath };
        }
    }

    // One per plane: the planes of a raster share its camera, not its file. A snapshot plane
    // has no file and is written to images/; one outside the project is copied there.
    struct PlanePlan {
        QString absPath;
        bool write = false;
        QString copyFrom;
    };
    QVector<QVector<PlanePlan>> planePlans(rasterCount());
    for (int ri = 0; ri < rasterCount(); ++ri) {
        const RasterEntry &re = raster(ri);
        for (int pi = 0; pi < int(re.planes.size()); ++pi) {
            const RasterPlane &p = re.planes[size_t(pi)];
            const QString name = pi > 0 ? QStringLiteral("%1_%2").arg(re.name).arg(pi) : re.name;
            PlanePlan plan;
            if (p.sourcePath.isEmpty()) {
                plan.absPath = uniqueFile(QStringLiteral("images"), name, QStringLiteral("raster_%1").arg(ri),
                                          QStringLiteral("png"));
                plan.write = true;
            } else if (!insideProject(p.sourcePath)) {
                outsideFiles = true;
                plan.copyFrom = p.sourcePath;
                plan.absPath = uniqueFile(QStringLiteral("images"), QFileInfo(p.sourcePath).fileName(),
                                          QStringLiteral("raster_%1").arg(ri),
                                          QFileInfo(p.sourcePath).suffix());
            } else {
                plan.absPath = p.sourcePath;
            }
            planePlans[ri].push_back(plan);
        }
    }

    if (outsideFiles && !options.copyFiles)
        return fail(tr("Some mesh or raster files are outside the project directory. "
                       "Enable 'copy files to project folder' to proceed."));

    // Phase 2: ensure directories exist
    projectDir.mkpath(QStringLiteral("meshes"));
    projectDir.mkpath(QStringLiteral("images"));

    // Phase 3: write the layers. Saving names each layer's file and records it as stored
    // (saveMesh), so the next project save writes only what has changed since.
    for (int mi = 0; mi < meshCount(); ++mi) {
        const MeshPlan &plan = meshPlans[mi];
        if (plan.action != MeshAction::Save && plan.action != MeshAction::SaveCopy)
            continue;
        MeshIOSaveOptions saveOptions;
        saveOptions.copyAssociatedTextures = plan.action == MeshAction::SaveCopy;
        if (saveMesh(mi, plan.absPath, saveOptions) != 0)
            return fail(tr("Failed to save mesh '%1' to %2").arg(mesh(mi).name, plan.absPath));
    }

    // Phase 4: write or copy the raster images
    for (int ri = 0; ri < rasterCount(); ++ri) {
        for (int pi = 0; pi < planePlans[ri].size(); ++pi) {
            const PlanePlan &plan = planePlans[ri][pi];
            if (plan.write) {
                QString planeError;
                ensureRasterPlaneImage(raster(ri).planes[size_t(pi)]);
                if (raster(ri).planes[size_t(pi)].hasImage()
                    && !saveRasterPlaneImage(ri, pi, plan.absPath, &planeError))
                    return fail(planeError);
            } else if (!plan.copyFrom.isEmpty()) {
                if (!QFile::copy(plan.copyFrom, plan.absPath))
                    return fail(tr("Failed to copy raster image '%1'").arg(plan.copyFrom));
                raster(ri).planes[size_t(pi)].sourcePath = QDir::toNativeSeparators(plan.absPath);
            }
        }
    }

    // Phase 5: write XML
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(tr("Cannot write project file: %1").arg(filename));

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("MeshLabProject"));

    // Meshes. The position of each in the file is what the visibility sets say it by.
    xml.writeStartElement(QStringLiteral("MeshGroup"));
    std::map<std::uint64_t, int> ordinalOfMeshId;
    for (int mi = 0; mi < meshCount(); ++mi) {
        const MeshEntry &entry = mesh(mi);
        if (options.onlyVisibleMeshes && !entry.visible) continue;
        const QString &absPath = meshPlans[mi].absPath;
        if (absPath.isEmpty()) continue;
        ordinalOfMeshId[entry.meshId] = int(ordinalOfMeshId.size());

        QString relPath = projectDir.relativeFilePath(absPath);

        xml.writeStartElement(QStringLiteral("MLMesh"));
        xml.writeAttribute(QStringLiteral("label"), entry.name);
        xml.writeAttribute(QStringLiteral("filename"), relPath);

        // Matrix
        xml.writeStartElement(QStringLiteral("MLMatrix44"));
        const QMatrix4x4 &t = entry.transform;
        auto matrixText = QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11 %12 %13 %14 %15 %16")
            .arg(t(0,0), 0, 'f', 6).arg(t(0,1), 0, 'f', 6).arg(t(0,2), 0, 'f', 6).arg(t(0,3), 0, 'f', 6)
            .arg(t(1,0), 0, 'f', 6).arg(t(1,1), 0, 'f', 6).arg(t(1,2), 0, 'f', 6).arg(t(1,3), 0, 'f', 6)
            .arg(t(2,0), 0, 'f', 6).arg(t(2,1), 0, 'f', 6).arg(t(2,2), 0, 'f', 6).arg(t(2,3), 0, 'f', 6)
            .arg(t(3,0), 0, 'f', 6).arg(t(3,1), 0, 'f', 6).arg(t(3,2), 0, 'f', 6).arg(t(3,3), 0, 'f', 6);
        xml.writeCharacters(matrixText.trimmed());
        xml.writeEndElement(); // MLMatrix44

        xml.writeEndElement(); // MLMesh
    }
    xml.writeEndElement(); // MeshGroup

    // Rasters
    xml.writeStartElement(QStringLiteral("RasterGroup"));
    for (int ri = 0; ri < rasterCount(); ++ri) {
        const RasterEntry &re = raster(ri);
        if (planePlans[ri].isEmpty()) continue;

        xml.writeStartElement(QStringLiteral("MLRaster"));
        xml.writeAttribute(QStringLiteral("label"), re.name);

        // Camera
        xml.writeStartElement(QStringLiteral("VCGCamera"));
        if (re.shot.isValid()) {
            CameraShot::VcgShot vcgShot = re.shot.toVcgShot();

            // Translation: negated (matching load convention)
            vcg::Point3f tra = vcgShot.Extrinsics.Tra();
            xml.writeAttribute(QStringLiteral("TranslationVector"),
                QStringLiteral("%1 %2 %3 1").arg(-tra[0], 0, 'f', 6).arg(-tra[1], 0, 'f', 6).arg(-tra[2], 0, 'f', 6));

            // Rotation matrix
            vcg::Matrix44f rot = vcgShot.Extrinsics.Rot();
            xml.writeAttribute(QStringLiteral("RotationMatrix"),
                QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11 %12 %13 %14 %15 %16")
                    .arg(rot[0][0], 0, 'f', 6).arg(rot[0][1], 0, 'f', 6).arg(rot[0][2], 0, 'f', 6).arg(rot[0][3], 0, 'f', 6)
                    .arg(rot[1][0], 0, 'f', 6).arg(rot[1][1], 0, 'f', 6).arg(rot[1][2], 0, 'f', 6).arg(rot[1][3], 0, 'f', 6)
                    .arg(rot[2][0], 0, 'f', 6).arg(rot[2][1], 0, 'f', 6).arg(rot[2][2], 0, 'f', 6).arg(rot[2][3], 0, 'f', 6)
                    .arg(rot[3][0], 0, 'f', 6).arg(rot[3][1], 0, 'f', 6).arg(rot[3][2], 0, 'f', 6).arg(rot[3][3], 0, 'f', 6));

            xml.writeAttribute(QStringLiteral("CameraType"),
                QString::number(                int(vcgShot.Intrinsics.cameraType)));
            xml.writeAttribute(QStringLiteral("FocalMm"),
                QString::number(vcgShot.Intrinsics.FocalMm, 'f', 4));
            xml.writeAttribute(QStringLiteral("LensDistortion"), QStringLiteral("0 0"));
            xml.writeAttribute(QStringLiteral("PixelSizeMm"),
                QStringLiteral("%1 %2").arg(vcgShot.Intrinsics.PixelSizeMm[0], 0, 'f', 6)
                    .arg(vcgShot.Intrinsics.PixelSizeMm[1], 0, 'f', 6));
            xml.writeAttribute(QStringLiteral("ViewportPx"),
                QStringLiteral("%1 %2").arg(vcgShot.Intrinsics.ViewportPx[0])
                    .arg(vcgShot.Intrinsics.ViewportPx[1]));
            xml.writeAttribute(QStringLiteral("CenterPx"),
                QStringLiteral("%1 %2").arg(vcgShot.Intrinsics.CenterPx[0], 0, 'f', 2)
                    .arg(vcgShot.Intrinsics.CenterPx[1], 0, 'f', 2));
        }
        xml.writeAttribute(QStringLiteral("BinaryData"), QStringLiteral("0"));
        xml.writeEndElement(); // VCGCamera

        // Planes, in order: a loaded raster shows its last one, as MeshLab always has
        for (int pi = 0; pi < int(re.planes.size()); ++pi) {
            const RasterPlane &plane = re.planes[size_t(pi)];
            xml.writeStartElement(QStringLiteral("Plane"));
            QString planeRelPath = projectDir.relativeFilePath(planePlans[ri][pi].absPath);
            xml.writeAttribute(QStringLiteral("fileName"), planeRelPath);
            QString semanticStr;
            switch (plane.semantic) {
            case RasterPlaneSemantic::RGBA:         semanticStr = QStringLiteral("RGBA"); break;
            case RasterPlaneSemantic::MaskUInt8:    semanticStr = QStringLiteral("MaskUInt8"); break;
            case RasterPlaneSemantic::MaskFloat:    semanticStr = QStringLiteral("MaskFloat"); break;
            case RasterPlaneSemantic::DepthFloat:   semanticStr = QStringLiteral("DepthFloat"); break;
            default:
                semanticStr = QStringLiteral("RGBA");
            }
            xml.writeAttribute(QStringLiteral("semantic"), semanticStr);
            xml.writeEndElement(); // Plane
        }

        xml.writeEndElement(); // MLRaster
    }
    xml.writeEndElement(); // RasterGroup

    // Visibility sets, which old versions do not know and skip. A set keeps the layers that went
    // into the file; one left with none is not written.
    bool openedSets = false;
    for (const VisibilitySet &set : visibilitySets()) {
        QStringList ordinals;
        for (const auto &entry : ordinalOfMeshId) {
            if (std::find(set.meshIds.begin(), set.meshIds.end(), entry.first) != set.meshIds.end())
                ordinals << QString::number(entry.second);
        }
        if (ordinals.isEmpty())
            continue;
        if (!openedSets) {
            xml.writeStartElement(QStringLiteral("VisibilitySets"));
            openedSets = true;
        }
        xml.writeStartElement(QStringLiteral("VisibilitySet"));
        xml.writeAttribute(QStringLiteral("name"), set.name);
        xml.writeAttribute(QStringLiteral("meshes"), ordinals.join(QLatin1Char(' ')));
        xml.writeEndElement(); // VisibilitySet
    }
    if (openedSets)
        xml.writeEndElement(); // VisibilitySets

    // The unit, when the user has said it; old versions skip the element.
    if (unit() != LengthUnit::Unknown) {
        xml.writeStartElement(QStringLiteral("LengthUnit"));
        xml.writeAttribute(QStringLiteral("name"), LengthUnits::symbol(unit()));
        xml.writeEndElement(); // LengthUnit
    }

    xml.writeEndElement(); // MeshLabProject
    xml.writeEndDocument();
    file.close();

    m_projectFilePath = QFileInfo(filename).absoluteFilePath();
    writeLog(tr("Saved project: %1").arg(filename), LogSource::Application);
    return true;
}


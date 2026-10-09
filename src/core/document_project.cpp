#include "document_internal.h"

using namespace DocumentInternal;

int Document::loadMeshLabProject(const QString &filename)
{
    const QString normalizedFilename = filename.trimmed();
    if (normalizedFilename.isEmpty())
        return -1;

    std::vector<MeshLabProjectMeshEntry> projectMeshes;
    std::vector<MeshLabProjectRasterEntry> projectRasters;
    QString parseError;
    if (!parseMeshLabProjectFile(normalizedFilename, projectMeshes, projectRasters, parseError)) {
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

    for (const MeshLabProjectMeshEntry &projectMesh : projectMeshes) {
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

        const int result = loadMesh(meshPath, nullptr, projectMesh.label,
                                    projectMesh.hasTransform ? &projectMesh.transform : nullptr);
        if (result != 0) {
            writeLog(
                tr("Failed to load project mesh: %1").arg(meshPath),
                LogSource::Application, LogLevel::Warning);
            continue;
        }
        ++loadedMeshes;
    }

    int rasterIdx = 0;
    for (const MeshLabProjectRasterEntry &projectRaster : projectRasters) {
        QElapsedTimer rt;
        rt.start();
        RasterEntry rasterEntry;
        rasterEntry.name = projectRaster.label;
        rasterEntry.shot = projectRaster.shot;
        rasterEntry.visible = true;
        rasterEntry.currentPlaneIndex = -1;

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
            if (rasterEntry.currentPlaneIndex < 0)
                rasterEntry.currentPlaneIndex = int(rasterEntry.planes.size()) - 1;
        }

        if (rasterEntry.planes.empty()) {
            RasterPlane plane;
            plane.semantic = RasterPlaneSemantic::RGBA;
            plane.size = projectRaster.shot.viewportPx();
            rasterEntry.planes.push_back(std::move(plane));
            rasterEntry.currentPlaneIndex = 0;
        }

        if (rasterEntry.sourcePath.trimmed().isEmpty()) {
            if (const RasterPlane *plane = rasterEntry.currentPlane())
                rasterEntry.sourcePath = plane->sourcePath.trimmed();
        }

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

    // Check for files outside the project directory
    const auto isOutsideProject = [&](const QString &absPath) {
        return absPath.startsWith(projectDir.absolutePath() + QDir::separator())
            || QFileInfo(absPath).dir() == projectDir;
    };
    (void)isOutsideProject;

    QStringList newMeshPaths;
    QStringList newImagePaths;

    // Phase 1: collect paths and validate
    struct EntryPath {
        QString absPath;
        bool needsCopy = false;
    };
    QVector<EntryPath> meshSavePaths(meshCount());
    QVector<EntryPath> rasterSavePaths(rasterCount());

    for (int mi = 0; mi < meshCount(); ++mi) {
        const MeshEntry &entry = mesh(mi);
        if (options.onlyVisibleMeshes && !entry.visible) continue;
        if (entry.modified && options.saveModifiedMeshes) {
            // Will re-save
            if (!entry.sourcePath.isEmpty()) {
                meshSavePaths[mi].absPath = entry.sourcePath;
                meshSavePaths[mi].needsCopy = false;
            } else {
                // New mesh — save to meshes/ dir
                QString safeName = entry.name.simplified();
                safeName.replace(QRegularExpression(QStringLiteral("[^\\w.-]")), QStringLiteral("_"));
                if (safeName.isEmpty()) safeName = QStringLiteral("mesh_%1").arg(mi);
                if (!safeName.endsWith(QStringLiteral(".ply"), Qt::CaseInsensitive))
                    safeName += QStringLiteral(".ply");
                meshSavePaths[mi].absPath = projectDir.filePath(QStringLiteral("meshes") + QDir::separator() + safeName);
                meshSavePaths[mi].needsCopy = true;
            }
        } else if (!entry.sourcePath.isEmpty()) {
            meshSavePaths[mi].absPath = entry.sourcePath;
            meshSavePaths[mi].needsCopy = false;
        } else {
            // No source path and not modified — skip
            meshSavePaths[mi].absPath.clear();
        }
    }

    for (int ri = 0; ri < rasterCount(); ++ri) {
        const RasterEntry &re = raster(ri);
        if (!re.planes.empty()) {
            const RasterPlane &p = re.planes.front();
            if (!p.sourcePath.isEmpty()) {
                rasterSavePaths[ri].absPath = p.sourcePath;
                rasterSavePaths[ri].needsCopy = false;
            } else {
                // Snapshot raster — save to images/
                QString safeName = re.name.simplified();
                safeName.replace(QRegularExpression(QStringLiteral("[^\\w.-]")), QStringLiteral("_"));
                if (safeName.isEmpty()) safeName = QStringLiteral("raster_%1").arg(ri);
                safeName += QStringLiteral(".png");
                rasterSavePaths[ri].absPath = projectDir.filePath(QStringLiteral("images") + QDir::separator() + safeName);
                rasterSavePaths[ri].needsCopy = true;
            }
        }
    }

    // Check for files requiring copy-to-project
    bool needsCopy = false;
    for (int mi = 0; mi < meshCount(); ++mi) {
        if (!meshSavePaths[mi].absPath.isEmpty() && meshSavePaths[mi].needsCopy) continue;
        if (!meshSavePaths[mi].absPath.isEmpty()) {
            if (!QFileInfo(meshSavePaths[mi].absPath).absoluteFilePath().startsWith(projectDir.absolutePath() + QDir::separator())
                && QFileInfo(meshSavePaths[mi].absPath).absolutePath() != projectDir.absolutePath()) {
                needsCopy = true;
            }
        }
    }
    for (int ri = 0; ri < rasterCount(); ++ri) {
        if (!rasterSavePaths[ri].absPath.isEmpty() && rasterSavePaths[ri].needsCopy) continue;
        if (!rasterSavePaths[ri].absPath.isEmpty()) {
            if (!QFileInfo(rasterSavePaths[ri].absPath).absoluteFilePath().startsWith(projectDir.absolutePath() + QDir::separator())
                && QFileInfo(rasterSavePaths[ri].absPath).absolutePath() != projectDir.absolutePath()) {
                needsCopy = true;
            }
        }
    }

    if (needsCopy && !options.copyFiles)
        return fail(tr("Some mesh or raster files are outside the project directory. "
                       "Enable 'copy files to project folder' to proceed."));

    // Phase 2: ensure directories exist
    projectDir.mkpath(QStringLiteral("meshes"));
    projectDir.mkpath(QStringLiteral("images"));

    // Phase 3: save/copy mesh files
    for (int mi = 0; mi < meshCount(); ++mi) {
        if (meshSavePaths[mi].absPath.isEmpty()) continue;
        const QString &destPath = meshSavePaths[mi].absPath;
        if (meshSavePaths[mi].needsCopy) {
            // Save mesh to destination
            const int result = saveMesh(mi, destPath);
            if (result != 0)
                return fail(tr("Failed to save mesh '%1' to %2").arg(mesh(mi).name, destPath));
            newMeshPaths.push_back(destPath);
        } else if (needsCopy && !meshSavePaths[mi].absPath.startsWith(projectDir.absolutePath() + QDir::separator())) {
            // Copy existing file into project
            const QString dest = projectDir.filePath(QStringLiteral("meshes") + QDir::separator()
                + QFileInfo(meshSavePaths[mi].absPath).fileName());
            if (!QFile::copy(meshSavePaths[mi].absPath, dest))
                return fail(tr("Failed to copy mesh file '%1'").arg(meshSavePaths[mi].absPath));
        }
    }

    // Phase 4: save raster images
    for (int ri = 0; ri < rasterCount(); ++ri) {
        const QString &destPath = rasterSavePaths[ri].absPath;
        if (destPath.isEmpty()) continue;
        if (rasterSavePaths[ri].needsCopy) {
            // Save QImage
            RasterPlane *rp = raster(ri).currentPlane();
            if (rp) {
                ensureRasterPlaneImage(*rp);
                if (!rp->image.isNull()) {
                    if (!rp->image.save(destPath))
                        return fail(tr("Failed to save raster image to %1").arg(destPath));
                    newImagePaths.push_back(destPath);
                }
            }
        } else if (needsCopy && !destPath.startsWith(projectDir.absolutePath() + QDir::separator())) {
            const QString dest = projectDir.filePath(QStringLiteral("images") + QDir::separator()
                + QFileInfo(destPath).fileName());
            if (!QFile::copy(destPath, dest))
                return fail(tr("Failed to copy raster image '%1'").arg(destPath));
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

    // Meshes
    xml.writeStartElement(QStringLiteral("MeshGroup"));
    for (int mi = 0; mi < meshCount(); ++mi) {
        const MeshEntry &entry = mesh(mi);
        if (options.onlyVisibleMeshes && !entry.visible) continue;
        const QString &absPath = meshSavePaths[mi].absPath;
        if (absPath.isEmpty()) continue;

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
        const QString &absPath = rasterSavePaths[ri].absPath;
        if (absPath.isEmpty()) continue;

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

        // Planes
        for (const RasterPlane &plane : re.planes) {
            xml.writeStartElement(QStringLiteral("Plane"));
            QString planeRelPath = projectDir.relativeFilePath(absPath);
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

    xml.writeEndElement(); // MeshLabProject
    xml.writeEndDocument();
    file.close();

    writeLog(tr("Saved project: %1").arg(filename), LogSource::Application);
    return true;
}


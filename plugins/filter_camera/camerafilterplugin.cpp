#include "camerafilterplugin.h"

#include "camerashot.h"
#include "document.h"
#include "filterparam.h"
#include "meshfilterpluginmanager.h"
#include "softdepthbuffer.h"
#include "vcgmesh.h"
#include "viewtrackball.h"

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace {

constexpr QLatin1StringView kSetMeshCamera("set_mesh_camera");
constexpr QLatin1StringView kSetRasterCamera("set_raster_camera");
constexpr QLatin1StringView kQualityFromCamera("compute_vertex_scalar_from_camera");
constexpr QLatin1StringView kCameraRotate("rotate_cameras");
constexpr QLatin1StringView kCameraScale("scale_cameras");
constexpr QLatin1StringView kCameraTranslate("translate_cameras");
constexpr QLatin1StringView kCameraTransform("transform_camera_extrinsics");
constexpr QLatin1StringView kOrientNormals("orient_vertex_normals_by_cameras");
constexpr QLatin1StringView kCameraViewSelection("set_camera_to_view_selection");
constexpr QLatin1StringView kCameraFromDirection("set_camera_from_direction");

MeshFilterRunResult fail(const QString &m) { return {false, false, m}; }
MeshFilterRunResult ok(const QStringList &info = {}) {
    MeshFilterRunResult r; r.success = true; r.documentModified = true;
    r.infoMessages = info; return r;
}

QMatrix4x4 rotationMatrix(const QVector3D &axis, float angleDeg)
{
    float rad = float(angleDeg * M_PI / 180.0);
    float c = std::cos(rad), s = std::sin(rad);
    QVector3D a = axis.normalized();
    float x = a.x(), y = a.y(), z = a.z();
    float t = 1.0f - c;
    QMatrix4x4 m;
    m(0,0) = t*x*x + c;   m(0,1) = t*x*y - s*z; m(0,2) = t*x*z + s*y;
    m(1,0) = t*x*y + s*z; m(1,1) = t*y*y + c;   m(1,2) = t*y*z - s*x;
    m(2,0) = t*x*z - s*y; m(2,1) = t*y*z + s*x; m(2,2) = t*z*z + c;
    m(3,3) = 1.0f;
    return m;
}

QMatrix4x4 translateMatrix(float x, float y, float z)
{
    QMatrix4x4 m;
    m(0,3) = x; m(1,3) = y; m(2,3) = z;
    return m;
}

QMatrix4x4 scaleMatrix(float s)
{
    QMatrix4x4 m;
    m(0,0) = s; m(1,1) = s; m(2,2) = s;
    return m;
}

// Apply a similarity transformation to a CameraShot (rotate + translate + scale)
void applySimilarityToShot(CameraShot &shot, const QVector3D &rotDeg,
                            const QVector3D &transl, float scale)
{
    QMatrix4x4 r = rotationMatrix({1,0,0}, rotDeg.x()) *
                   rotationMatrix({0,1,0}, rotDeg.y()) *
                   rotationMatrix({0,0,1}, rotDeg.z());
    r(0,3) = transl.x();
    r(1,3) = transl.y();
    r(2,3) = transl.z();
    if (scale != 1.0f) {
        // Apply scale via rescalingWorld
        // Apply rotation+translation first, then scale centered on viewpoint
        shot.applyRigidTransformation(r);
        shot.rescalingWorld(scale);
    } else {
        shot.applyRigidTransformation(r);
    }
}

// Build a viewpoint vector from camera parameters
QVector3D viewDirection(const QVector3D &dir, const QVector3D &up = QVector3D(0,1,0))
{
    QVector3D fwd = dir.normalized();
    if (fwd.lengthSquared() < 0.001f) fwd = QVector3D(0,0,-1);
    return fwd;
}

// Set a CameraShot from viewpoint, direction, focal, sensor width parameters
void setShotFromParams(CameraShot &shot, const QVector3D &vp,
                        const QVector3D &dir, float focalMm,
                        float sensorWidthMm = 36.0f, QSize viewport = QSize())
{
    shot.setViewPoint(vp);
    shot.setFocalMm(focalMm);
    if (viewport.isValid() && viewport.width() > 0 && viewport.height() > 0) {
        shot.setViewportPx(viewport);
        float px = float(viewport.width()) / sensorWidthMm;
        float py = float(viewport.height()) / (sensorWidthMm * float(viewport.height()) / float(viewport.width()));
        shot.setPixelSizeMm({1.0f/px, 1.0f/py});
        shot.setCenterPx({float(viewport.width())/2.0f, float(viewport.height())/2.0f});
    }
    // Orient using a QMatrix4x4 and pass through VCG shot
    QVector3D fwd = dir.normalized();
    if (fwd.lengthSquared() > 0.5f) {
        QVector3D up(0,1,0);
        if (std::abs(QVector3D::dotProduct(fwd, up)) > 0.99f)
            up = QVector3D(1,0,0);
        QVector3D right = QVector3D::crossProduct(up, fwd).normalized();
        up = QVector3D::crossProduct(fwd, right).normalized();
        QMatrix4x4 rot;
        rot(0,0) = right.x(); rot(0,1) = right.y(); rot(0,2) = right.z();
        rot(1,0) = up.x();    rot(1,1) = up.y();    rot(1,2) = up.z();
        rot(2,0) = fwd.x();   rot(2,1) = fwd.y();   rot(2,2) = fwd.z();
        rot(3,3) = 1.0f;
        vcg::Matrix44f vcgRot;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                vcgRot[r][c] = rot(r,c);
        CameraShot::VcgShot vcgShot = shot.toVcgShot();
        vcgShot.Extrinsics.SetRot(vcgRot);
        vcgShot.Extrinsics.SetTra(vcg::Point3f(vp.x(), vp.y(), vp.z()));
        shot = CameraShot::fromVcgShot(vcgShot);
    }
}

// Apply a transformation to mesh entries (toall mode) — bake transform into vertices
void applyTransformToVisibleMeshes(Document &doc, const QMatrix4x4 &transf)
{
    for (int mi = 0; mi < doc.meshCount(); ++mi) {
        auto &ent = doc.mesh(mi);
        if (!ent.visible) continue;
        VCGMesh &m = ent.mesh;
        // Bake the transform into vertex positions
        for (auto &v : m.vert) {
            QVector3D p = transformPoint(transf, v.cP());
            v.P()[0] = p.x(); v.P()[1] = p.y(); v.P()[2] = p.z();
        }
        ent.transform = ent.transform * transf;
        doc.markMeshGeometryChanged(mi, QStringLiteral("Camera transform applied"));
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

QString CameraFilterPlugin::pluginId() const
{ return QStringLiteral("meshlab2.filter.camera"); }

QString CameraFilterPlugin::name() const
{ return QStringLiteral("Camera Filters"); }

MeshFilterRunResult CameraFilterPlugin::runFilter(
    const QString &fid, const FilterParams &p, Document &doc) const
{
    using namespace vcg::tri::io;

    // --- Set Mesh Camera ---
    if (fid == QString::fromLatin1(kSetMeshCamera)) {
        QVector3D vp = p.getPoint3f(QStringLiteral("viewpoint"));
        QVector3D dir = p.getPoint3f(QStringLiteral("direction"));
        float focal = float(p.getDouble(QStringLiteral("focalMm")));
        doc.writeLog(QObject::tr("Set Mesh Camera: viewpoint=(%1,%2,%3) focal=%4")
            .arg(vp.x()).arg(vp.y()).arg(vp.z()).arg(focal),
            Document::LogSource::Application);
        return ok({QObject::tr("Mesh camera parameters logged.")});
    }

    // --- Set Raster Camera ---
    if (fid == QString::fromLatin1(kSetRasterCamera)) {
        int ri = doc.currentRasterIndex();
        if (ri < 0) return fail(QObject::tr("No current raster selected."));
        auto &re = doc.raster(ri);
        QVector3D vp = p.getPoint3f(QStringLiteral("viewpoint"));
        QVector3D dir = p.getPoint3f(QStringLiteral("direction"));
        float focal = float(p.getDouble(QStringLiteral("focalMm")));
        float sensorW = float(p.getDouble(QStringLiteral("sensorWidthMm")));
        const auto *rp = re.currentPlane();
        QSize vpSize = rp ? rp->size : QSize();
        if (!vpSize.isValid() || vpSize.width() <= 0) {
            if (re.shot.isValid())
                vpSize = re.shot.viewportPx();
        }
        CameraShot shot;
        setShotFromParams(shot, vp, dir, focal, sensorW, vpSize);
        re.shot = shot;
        doc.writeLog(QObject::tr("Set camera for raster '%1'.").arg(re.name),
                     Document::LogSource::Application);
        return ok({QObject::tr("Set camera for raster '%1'.").arg(re.name)});
    }

    // --- Compute Vertex Scalar from Camera ---
    if (fid == QString::fromLatin1(kQualityFromCamera)) {
        int mi = doc.currentMeshIndex();
        if (mi < 0) return fail(QObject::tr("No current mesh."));
        VCGMesh &m = doc.mesh(mi).mesh;

        // Use the current raster camera; fall back to first visible if needed
        CameraShot shot;
        bool haveShot = false;
        int curRi = doc.currentRasterIndex();
        if (curRi >= 0) {
            auto &re = doc.raster(curRi);
            if (re.shot.isValid()) {
                shot = re.shot; haveShot = true;
            }
        }
        if (!haveShot) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid()) {
                    shot = re.shot; haveShot = true; break;
                }
            }
        }
        if (!haveShot) {
            return fail(QObject::tr("No valid camera found."));
        }

        bool depthFlag = p.getBool(QStringLiteral("depth"), true);
        bool facingFlag = p.getBool(QStringLiteral("facing"), false);
        bool clipFlag = p.getBool(QStringLiteral("clip"), false);
        bool normalize = p.getBool(QStringLiteral("normalize"), false);
        bool map = p.getBool(QStringLiteral("map"), false);

        auto &ent = doc.mesh(mi);
        ent.ioMask |= Mask::IOM_VERTQUALITY;
        if (map) ent.ioMask |= Mask::IOM_VERTCOLOR;

        QSize vp = shot.viewportPx();
        float deltaN = ent.mesh.bbox.Diag() / 100.0f;

        for (auto &v : m.vert) {
            QVector3D wv = transformPoint(ent.transform, v.cP());
            vcg::Point3f pc = shot.toVcgShot().ConvertWorldToCameraCoordinates(vcg::Point3f(wv.x(), wv.y(), wv.z()));
            vcg::Point3f vn(v.cN()[0], v.cN()[1], v.cN()[2]);
            vcg::Point3f pn = shot.toVcgShot().ConvertWorldToCameraCoordinates(
                vcg::Point3f(wv.x(), wv.y(), wv.z()) + vn * deltaN);
            QVector2D pp = shot.project(wv);
            float depth = shot.depth(wv);
            float q = 1.0f;
            if (depthFlag)
                q *= depth;
            if (facingFlag)
                q *= (pn[2] - pc[2]);
            if (clipFlag) {
                if (pp.x() < 0 || pp.y() < 0 || pp.x() > float(vp.width()) || pp.y() > float(vp.height()))
                    q = 0;
            }
            v.Q() = q;
        }

        if (normalize)
            vcg::tri::UpdateQuality<VCGMesh>::VertexNormalize(m);
        if (map) {
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(m);
            ent.ioMask |= Mask::IOM_VERTCOLOR;
        }

        doc.markMeshGeometryChanged(mi, QObject::tr("Computed vertex quality from camera on '%1'.").arg(ent.name));
        return ok({QObject::tr("Quality computed for %1 vertices.").arg(m.VN())});
    }

    // --- Camera Rotate ---
    if (fid == QString::fromLatin1(kCameraRotate)) {
        bool toall = p.getBool(QStringLiteral("toall"), false);
        bool toallRaster = p.getBool(QStringLiteral("toallRaster"), false);
        QString camEnum = p.getEnum(QStringLiteral("camera"));
        QString axisEnum = p.getEnum(QStringLiteral("rotAxis"));
        QString centerEnum = p.getEnum(QStringLiteral("rotCenter"));
        float angle = float(p.getDouble(QStringLiteral("angle")));

        // Determine rotation axis
        QVector3D axis;
        if (axisEnum == QStringLiteral("x")) axis = {1,0,0};
        else if (axisEnum == QStringLiteral("y")) axis = {0,1,0};
        else if (axisEnum == QStringLiteral("z")) axis = {0,0,1};
        else axis = p.getPoint3f(QStringLiteral("customAxis"));

        QMatrix4x4 rot = rotationMatrix(axis, angle);

        // Determine rotation center
        QVector3D center;
        if (centerEnum == QStringLiteral("origin")) center = {0,0,0};
        else if (centerEnum == QStringLiteral("viewpoint")) {
            int ri = doc.currentRasterIndex();
            if (ri >= 0 && doc.raster(ri).shot.isValid())
                center = doc.raster(ri).shot.viewPoint();
            else
                center = {0,0,0};
        } else center = p.getPoint3f(QStringLiteral("customCenter"));

        QMatrix4x4 trTran = translateMatrix(center.x(), center.y(), center.z());
        QMatrix4x4 trTranInv = translateMatrix(-center.x(), -center.y(), -center.z());
        QMatrix4x4 transf = trTran * rot * trTranInv;

        if (toall) {
            applyTransformToVisibleMeshes(doc, transf);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applyRigidTransformation(transf);
            }
            doc.writeLog(QObject::tr("Rotated all cameras by %1 deg.").arg(angle),
                         Document::LogSource::Application);
        } else if (toallRaster) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applyRigidTransformation(transf);
            }
            doc.writeLog(QObject::tr("Rotated all rasters by %1 deg.").arg(angle),
                         Document::LogSource::Application);
        } else if (camEnum == QStringLiteral("raster") || camEnum == QStringLiteral("mesh")) {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            doc.raster(ri).shot.applyRigidTransformation(transf);
        }

        return ok({QObject::tr("Rotation applied.")});
    }

    // --- Camera Scale ---
    if (fid == QString::fromLatin1(kCameraScale)) {
        bool toall = p.getBool(QStringLiteral("toall"), false);
        bool toallRaster = p.getBool(QStringLiteral("toallRaster"), false);
        QString camEnum = p.getEnum(QStringLiteral("camera"));
        QString centerEnum = p.getEnum(QStringLiteral("scaleCenter"));
        float scale = float(p.getDouble(QStringLiteral("scale")));

        QVector3D center;
        if (centerEnum == QStringLiteral("origin")) center = {0,0,0};
        else if (centerEnum == QStringLiteral("viewpoint")) {
            int ri = doc.currentRasterIndex();
            if (ri >= 0 && doc.raster(ri).shot.isValid())
                center = doc.raster(ri).shot.viewPoint();
            else center = {0,0,0};
        } else center = p.getPoint3f(QStringLiteral("customCenter"));

        QMatrix4x4 trTran = translateMatrix(center.x(), center.y(), center.z());
        QMatrix4x4 trTranInv = translateMatrix(-center.x(), -center.y(), -center.z());
        QMatrix4x4 transf = trTran * scaleMatrix(scale) * trTranInv;

        auto scaleShot = [&](CameraShot &shot) {
            QMatrix4x4 tr;
            tr(0,0) = 1.0f; tr(1,1) = 1.0f; tr(2,2) = 1.0f;
            tr(0,3) = (1.0f - scale) * center.x();
            tr(1,3) = (1.0f - scale) * center.y();
            tr(2,3) = (1.0f - scale) * center.z();
            shot.applyRigidTransformation(tr);
            shot.rescalingWorld(scale);
        };

        if (toall) {
            applyTransformToVisibleMeshes(doc, transf);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    scaleShot(doc.raster(ri).shot);
            }
        } else if (toallRaster) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    scaleShot(doc.raster(ri).shot);
            }
        } else if (camEnum == QStringLiteral("raster") || camEnum == QStringLiteral("mesh")) {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            scaleShot(doc.raster(ri).shot);
        }
        return ok({QObject::tr("Scaling applied (factor=%1).").arg(scale)});
    }

    // --- Camera Translate ---
    if (fid == QString::fromLatin1(kCameraTranslate)) {
        bool toall = p.getBool(QStringLiteral("toall"), false);
        bool toallRaster = p.getBool(QStringLiteral("toallRaster"), false);
        QString camEnum = p.getEnum(QStringLiteral("camera"));
        float tx = float(p.getDouble(QStringLiteral("tx")));
        float ty = float(p.getDouble(QStringLiteral("ty")));
        float tz = float(p.getDouble(QStringLiteral("tz")));
        bool centerFlag = p.getBool(QStringLiteral("centerFlag"), false);

        QMatrix4x4 trTran = translateMatrix(tx, ty, tz);
        if (centerFlag) {
            int ri = doc.currentRasterIndex();
            if (ri >= 0 && doc.raster(ri).shot.isValid()) {
                QVector3D vp = doc.raster(ri).shot.viewPoint();
                float d = vp.length();
                if (d > 0.001f) trTran = translateMatrix(-vp.x(), -vp.y(), -vp.z());
            }
        }

        if (toall) {
            applyTransformToVisibleMeshes(doc, trTran);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applyRigidTransformation(trTran);
            }
        } else if (toallRaster) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applyRigidTransformation(trTran);
            }
        } else if (camEnum == QStringLiteral("raster") || camEnum == QStringLiteral("mesh")) {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            doc.raster(ri).shot.applyRigidTransformation(trTran);
        }
        return ok({QObject::tr("Translation applied.")});
    }

    // --- Camera Transform (extrinsics) ---
    if (fid == QString::fromLatin1(kCameraTransform)) {
        bool toall = p.getBool(QStringLiteral("toall"), false);
        bool toallRaster = p.getBool(QStringLiteral("toallRaster"), false);
        QString camEnum = p.getEnum(QStringLiteral("camera"));
        QString behaviour = p.getEnum(QStringLiteral("behaviour"));

        if (behaviour == QStringLiteral("replace")) {
            // "New extrinsics" mode — not implemented with current parameter system
            return fail(QObject::tr("'New extrinsics' mode not available. Use 'Apply transformation' mode instead."));
        }

        QVector3D rotDeg = p.getPoint3f(QStringLiteral("rotationDeg"));
        QVector3D transl = p.getPoint3f(QStringLiteral("translation"));
        float scale = float(p.getDouble(QStringLiteral("uniformScale")));

        if (toall) {
            QMatrix4x4 r = rotationMatrix({1,0,0}, rotDeg.x()) *
                           rotationMatrix({0,1,0}, rotDeg.y()) *
                           rotationMatrix({0,0,1}, rotDeg.z());
            QMatrix4x4 trTran = translateMatrix(transl.x(), transl.y(), transl.z());
            QMatrix4x4 transf = trTran * r;
            if (scale != 1.0f) transf = trTran * scaleMatrix(scale) * rotationMatrix({1,0,0}, rotDeg.x())
                                        * rotationMatrix({0,1,0}, rotDeg.y()) * rotationMatrix({0,0,1}, rotDeg.z());
            applyTransformToVisibleMeshes(doc, transf);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    applySimilarityToShot(doc.raster(ri).shot, rotDeg, transl, scale);
            }
        } else if (toallRaster) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    applySimilarityToShot(doc.raster(ri).shot, rotDeg, transl, scale);
            }
        } else if (camEnum == QStringLiteral("raster") || camEnum == QStringLiteral("mesh")) {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            applySimilarityToShot(doc.raster(ri).shot, rotDeg, transl, scale);
        }
        return ok({QObject::tr("Transformation applied.")});
    }

    // --- Re-Orient Normals with Cameras ---
    if (fid == QString::fromLatin1(kOrientNormals)) {
        int mi = doc.currentMeshIndex();
        if (mi < 0) return fail(QObject::tr("No current mesh."));
        VCGMesh &m = doc.mesh(mi).mesh;
        auto &ent = doc.mesh(mi);

        // Check for Bundler-style "correspondences" per-vertex attribute
        struct Correspondence { unsigned int id_img; float padding[3]; };
        using CorrVec = std::vector<Correspondence>;
        auto ch = vcg::tri::Allocator<VCGMesh>::FindPerVertexAttribute<CorrVec>(m, "correspondences");
        bool haveCorr = vcg::tri::Allocator<VCGMesh>::IsValidHandle(m, ch);

        if (haveCorr) {
            // Bundler mode: use stored camera index per vertex
            std::vector<CameraShot> cameraShots;
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                cameraShots.push_back(re.shot);
            }
            for (auto &v : m.vert) {
                unsigned int camIdx = ch[v][0].id_img;
                if (camIdx >= (unsigned int)cameraShots.size()) continue;
                const CameraShot &shot = cameraShots[size_t(camIdx)];
                if (!shot.isValid()) continue;
                QVector3D wv = transformPoint(ent.transform, v.cP());
                QVector3D viewDir = (shot.viewPoint() - wv).normalized();
                vcg::Point3f n(v.cN()[0], v.cN()[1], v.cN()[2]);
                if (viewDir.x() * n[0] + viewDir.y() * n[1] + viewDir.z() * n[2] < 0.0f)
                    v.N() = -n;
            }
            doc.markMeshGeometryChanged(mi, QObject::tr("Reoriented normals using Bundler correspondences on '%1'.").arg(ent.name));
            return ok({QObject::tr("Normals reoriented using Bundler correspondences.")});
        }

        // General mode: orient normals toward the best visible camera
        // For each vertex, find the closest visible raster and orient toward it
        int nFlipped = 0;
        for (auto &v : m.vert) {
            QVector3D wv = transformPoint(ent.transform, v.cP());
            float bestScore = -std::numeric_limits<float>::max();
            QVector3D bestDir(0,0,0);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (!re.visible || !re.shot.isValid()) continue;
                QVector3D dir = (re.shot.viewPoint() - wv).normalized();
                vcg::Point3f n(v.cN()[0], v.cN()[1], v.cN()[2]);
                float score = dir.x() * n[0] + dir.y() * n[1] + dir.z() * n[2];
                if (score > bestScore) { bestScore = score; bestDir = dir; }
            }
            if (bestScore > -0.9f) {
                vcg::Point3f n(v.cN()[0], v.cN()[1], v.cN()[2]);
                if (bestDir.x() * n[0] + bestDir.y() * n[1] + bestDir.z() * n[2] < 0.0f) {
                    v.N() = -n;
                    ++nFlipped;
                }
            }
        }
        if (nFlipped == 0 && doc.rasterCount() == 0)
            return fail(QObject::tr("No visible rasters with valid cameras to orient normals."));
        doc.markMeshGeometryChanged(mi, QObject::tr("Reoriented %1 vertex normals using cameras on '%2'.").arg(nFlipped).arg(ent.name));
        return ok({QObject::tr("Flipped %1 normals.").arg(nFlipped)});
    }

    // --- Set Camera to View Selection ---
    if (fid == QString::fromLatin1(kCameraViewSelection)) {
        int mi = doc.currentMeshIndex();
        if (mi < 0) return fail(QObject::tr("No current mesh."));
        VCGMesh &m = doc.mesh(mi).mesh;
        auto &ent = doc.mesh(mi);

        float margin = float(p.getDouble(QStringLiteral("marginFactor"), 1.0));
        float fovYDeg = float(p.getDouble(QStringLiteral("fovYDeg"), 45.0));
        bool useFaceNormals = p.getBool(QStringLiteral("useFaceNormals"), true);

        // Collect selected faces
        std::vector<const VCGVertex *> selVerts;
        QVector3D centroid(0, 0, 0);
        QVector3D avgNormal(0, 0, 0);
        int selCount = 0;
        vcg::Box3f selBbox;
        selBbox.SetNull();

        for (const auto &f : m.face) {
            if (!f.IsS() || f.IsD()) continue;
            QVector3D fc = (transformPoint(ent.transform, f.cP(0))
                          + transformPoint(ent.transform, f.cP(1))
                          + transformPoint(ent.transform, f.cP(2))) / 3.0f;
            centroid += fc;
            vcg::Point3f fn = ((f.cP(1) - f.cP(0)) ^ (f.cP(2) - f.cP(0))).Normalize();
            avgNormal += QVector3D(fn[0], fn[1], fn[2]);
            selBbox.Add(vcg::Point3f(fc.x(), fc.y(), fc.z()));
            selCount++;
        }

        if (selCount == 0) {
            // Fall back to selected vertices
            for (const auto &v : m.vert) {
                if (!v.IsS() || v.IsD()) continue;
                QVector3D wv = transformPoint(ent.transform, v.cP());
                centroid += wv;
                vcg::Point3f vn(v.cN()[0], v.cN()[1], v.cN()[2]);
                avgNormal += QVector3D(vn[0], vn[1], vn[2]);
                selBbox.Add(vcg::Point3f(wv.x(), wv.y(), wv.z()));
                selCount++;
                selVerts.push_back(&v);
            }
            if (selCount == 0)
                return fail(QObject::tr("No faces or vertices selected."));
        }

        centroid /= float(selCount);
        QVector3D viewDir = avgNormal.normalized();
        if (viewDir.lengthSquared() < 0.01f) return fail(QObject::tr("Selection normal is zero."));

        // Distance to fit selection in viewport
        float halfDiag = selBbox.Diag() * 0.5f;
        float fovYRad = float(qDegreesToRadians(fovYDeg));
        float distance = (halfDiag / std::tan(fovYRad * 0.5f)) * margin;

        QVector3D eye = centroid - viewDir * distance;

        ViewTrackball tb;
        tb.setFromLookAt(eye, centroid, fovYDeg);
        auto state = tb.state();
        state.radius = qMax(halfDiag, 1e-4f);

        // Serialize to CameraState JSON
        QJsonObject trackball;
        trackball.insert(QStringLiteral("center"), QJsonArray{state.center.x(), state.center.y(), state.center.z()});
        QJsonArray rot{state.rotation.x(), state.rotation.y(), state.rotation.z(), state.rotation.scalar()};
        trackball.insert(QStringLiteral("rotation_xyzw"), rot);
        trackball.insert(QStringLiteral("distance"), state.distance);
        trackball.insert(QStringLiteral("radius"), state.radius);
        trackball.insert(QStringLiteral("fov_y_degrees"), state.fovYDeg);

        QJsonObject root;
        root.insert(QStringLiteral("kind"), QStringLiteral("QMeshLab.CameraState"));
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("trackball"), trackball);

        QString json = QString::fromUtf8(
            QJsonDocument(root).toJson(QJsonDocument::Indented));

        doc.writeLog(json, Document::LogSource::Application);

        // Apply to current raster if available
        int ri = doc.currentRasterIndex();
        if (ri >= 0) {
            CameraShot shot;
            auto vcgShot = shot.toVcgShot();
            vcgShot.Intrinsics.cameraType = vcg::Camera<float>::PERSPECTIVE;
            vcgShot.Intrinsics.ViewportPx = vcg::Point2i(800, 600);
            vcgShot.Intrinsics.CenterPx = vcg::Point2f(400, 300);
            vcgShot.Intrinsics.PixelSizeMm = vcg::Point2f(1, 1);
            vcgShot.Intrinsics.FocalMm = 600.0f / (2.0f * std::tan(fovYRad * 0.5f));
            QVector3D up(0, 1, 0);
            if (std::abs(QVector3D::dotProduct(viewDir, up)) > 0.99f) up = QVector3D(1, 0, 0);
            vcgShot.LookAt(eye.x(), eye.y(), eye.z(),
                           centroid.x(), centroid.y(), centroid.z(),
                           up.x(), up.y(), up.z());
            doc.raster(ri).shot = CameraShot::fromVcgShot(vcgShot);
        }

        return ok({QObject::tr("Camera state (selection view):\n%1").arg(json)});
    }

    // --- Set Camera from Direction ---
    if (fid == QString::fromLatin1(kCameraFromDirection)) {
        QVector3D direction = p.getPoint3f(QStringLiteral("direction"),
                                           QVector3D(0, 0, -1)).normalized();
        float margin = float(p.getDouble(QStringLiteral("marginFactor"), 1.0));
        float fovYDeg = float(p.getDouble(QStringLiteral("fovYDeg"), 45.0));
        QString targetEnum = p.getEnum(QStringLiteral("target"));

        // Compute target center
        QVector3D center(0, 0, 0);
        float halfDiag = 1.0f;

        if (targetEnum == QStringLiteral("mesh_bbox")) {
            int mi = doc.currentMeshIndex();
            if (mi < 0) return fail(QObject::tr("No current mesh."));
            auto &ent = doc.mesh(mi);
            QMatrix4x4 tf = ent.transform;
            vcg::Box3f bbox;
            bbox.SetNull();
            for (const auto &v : ent.mesh.vert) {
                if (v.IsD()) continue;
                QVector3D wv = transformPoint(tf, v.cP());
                bbox.Add(vcg::Point3f(wv.x(), wv.y(), wv.z()));
            }
            if (bbox.IsNull()) return fail(QObject::tr("Mesh bounding box is empty."));
            center = QVector3D(bbox.Center()[0], bbox.Center()[1], bbox.Center()[2]);
            halfDiag = bbox.Diag() * 0.5f;
        } else {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            auto &re = doc.raster(ri);
            if (!re.shot.isValid()) return fail(QObject::tr("Raster camera is not valid."));
            center = re.shot.viewPoint() + re.shot.referenceAxis(2) * 1.0f;
            halfDiag = 1.0f;
        }

        float fovYRad = float(qDegreesToRadians(fovYDeg));
        float distance = (halfDiag / std::tan(fovYRad * 0.5f)) * margin;
        QVector3D eye = center - direction * distance;

        ViewTrackball tb;
        tb.setFromLookAt(eye, center, fovYDeg);
        auto state = tb.state();
        state.radius = qMax(halfDiag, 1e-4f);

        QJsonObject trackball;
        trackball.insert(QStringLiteral("center"), QJsonArray{state.center.x(), state.center.y(), state.center.z()});
        QJsonArray rot{state.rotation.x(), state.rotation.y(), state.rotation.z(), state.rotation.scalar()};
        trackball.insert(QStringLiteral("rotation_xyzw"), rot);
        trackball.insert(QStringLiteral("distance"), state.distance);
        trackball.insert(QStringLiteral("radius"), state.radius);
        trackball.insert(QStringLiteral("fov_y_degrees"), state.fovYDeg);

        QJsonObject root;
        root.insert(QStringLiteral("kind"), QStringLiteral("QMeshLab.CameraState"));
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("trackball"), trackball);

        QString json = QString::fromUtf8(
            QJsonDocument(root).toJson(QJsonDocument::Indented));

        doc.writeLog(json, Document::LogSource::Application);

        int ri = doc.currentRasterIndex();
        if (ri >= 0) {
            CameraShot shot;
            auto vcgShot = shot.toVcgShot();
            vcgShot.Intrinsics.cameraType = vcg::Camera<float>::PERSPECTIVE;
            vcgShot.Intrinsics.ViewportPx = vcg::Point2i(800, 600);
            vcgShot.Intrinsics.CenterPx = vcg::Point2f(400, 300);
            vcgShot.Intrinsics.PixelSizeMm = vcg::Point2f(1, 1);
            vcgShot.Intrinsics.FocalMm = 600.0f / (2.0f * std::tan(fovYRad * 0.5f));
            QVector3D up(0, 1, 0);
            if (std::abs(QVector3D::dotProduct(direction, up)) > 0.99f) up = QVector3D(1, 0, 0);
            vcgShot.LookAt(eye.x(), eye.y(), eye.z(),
                           center.x(), center.y(), center.z(),
                           up.x(), up.y(), up.z());
            doc.raster(ri).shot = CameraShot::fromVcgShot(vcgShot);
        }

        return ok({QObject::tr("Camera state (direction):\n%1").arg(json)});
    }

    return fail(QObject::tr("Unknown filter: %1").arg(fid));
}

void registerCameraFilterPlugin(MeshFilterPluginManager &pm)
{
    pm.registerPlugin(std::make_unique<CameraFilterPlugin>());
}

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
constexpr QLatin1StringView kCameraFrameFromViewpoint("set_camera_to_frame_from_viewpoint");

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

// Move every visible layer by the world-space transform the cameras get (toall mode),
// written into its vertices once. Conjugating by the layer matrix keeps that matrix as it
// was and still moves the layer in world space, where the rasters' shots move. This used to
// bake the transform and also compose it onto the matrix, moving each layer twice.
void applyTransformToVisibleMeshes(Document &doc, const QMatrix4x4 &transf)
{
    for (int mi = 0; mi < doc.meshCount(); ++mi) {
        auto &ent = doc.mesh(mi);
        if (!ent.visible) continue;
        bool invertible = false;
        const QMatrix4x4 inverse = ent.transform.inverted(&invertible);
        Document::transformMeshGeometry(
            ent.mesh, invertible ? inverse * transf * ent.transform : transf);
        doc.markMeshGeometryChanged(mi, QStringLiteral("Camera transform applied"));
    }
}

// Whether a framing filter makes an orthographic camera: asked for, or left on "current" and
// the current raster's camera is one. Without a raster, "current" means perspective.
bool framingIsOrthographic(const FilterParams &p, Document &doc)
{
    const QString projection = p.getEnum(QStringLiteral("projection"));
    if (projection == QStringLiteral("orthographic"))
        return true;
    if (projection == QStringLiteral("perspective"))
        return false;
    const int ri = doc.currentRasterIndex();
    return ri >= 0 && doc.raster(ri).shot.isOrthographic();
}

// A camera at `eye` looking at `center` that frames a sphere of radius `radius` about it. It
// goes to the log, to the view (MeshFilterRunResult::cameraState) and to a script
// (outputValues), and onto the current raster, if any, for that raster's own image: the shot
// takes the raster's size, so the image and its camera stay registered. The visible height at
// `center` is the same in both projections, H = 2 * distance * tan(fovY / 2).
MeshFilterRunResult framedCamera(Document &doc, const QVector3D &eye, const QVector3D &center,
                                 float radius, float fovYDeg, bool orthographic, const QString &what)
{
    ViewTrackball tb;
    tb.setFromLookAt(eye, center, fovYDeg);
    // The gizmo, sized to what is framed rather than to the unit sphere of a fresh trackball.
    tb.setFrame(center, qMax(radius, 1e-4f), (center - eye).length());
    if (orthographic)
        tb.setProjection(Projection::Orthographic);
    const ViewTrackball::State state = tb.state();
    const QString json = ViewTrackball::cameraStateJson(state);
    doc.writeLog(json, Document::LogSource::Application);

    MeshFilterRunResult result = ok({QObject::tr("Camera state (%1):\n%2").arg(what, json)});
    result.documentModified = false;
    result.cameraState = json;
    result.outputValues.insert(QStringLiteral("cameraState"), json);

    const int ri = doc.currentRasterIndex();
    if (ri >= 0) {
        const Document::RasterEntry &raster = doc.raster(ri);
        const RasterPlane *plane = raster.currentPlane();
        const QSize size = plane && !plane->size.isEmpty() ? plane->size : raster.shot.viewportPx();
        if (!size.isEmpty()) {
            const float tanHalfFov = std::tan(0.5f * qDegreesToRadians(fovYDeg));
            const QVector3D viewDir = (center - eye).normalized();
            CameraShot shot;
            auto vcgShot = shot.toVcgShot();
            vcgShot.Intrinsics.cameraType =
                orthographic ? vcg::Camera<float>::ORTHO : vcg::Camera<float>::PERSPECTIVE;
            vcgShot.Intrinsics.ViewportPx = vcg::Point2i(size.width(), size.height());
            vcgShot.Intrinsics.CenterPx = vcg::Point2f(0.5f * size.width(), 0.5f * size.height());
            vcgShot.Intrinsics.DistorCenterPx = vcgShot.Intrinsics.CenterPx;
            // Orthographic: world units per pixel, so that the image's height spans H.
            // Perspective: the fictitious unit pixel, with the focal length that gives the field
            // of view.
            const float pixel = orthographic ? 2.0f * state.distance * tanHalfFov / float(size.height()) : 1.0f;
            vcgShot.Intrinsics.PixelSizeMm = vcg::Point2f(pixel, pixel);
            vcgShot.Intrinsics.FocalMm = float(size.height()) / (2.0f * tanHalfFov);
            QVector3D up(0, 1, 0);
            if (std::abs(QVector3D::dotProduct(viewDir, up)) > 0.99f) up = QVector3D(1, 0, 0);
            vcgShot.LookAt(eye.x(), eye.y(), eye.z(),
                           center.x(), center.y(), center.z(),
                           up.x(), up.y(), up.z());
            doc.setRasterShot(ri, CameraShot::fromVcgShot(vcgShot),
                              QObject::tr("Raster camera set (%1): '%2'").arg(what, raster.name));
            result.documentModified = true;
        }
    }
    return result;
}

// The distance from which a sphere of `radius` just fills a field of view of `fovYDeg`: its
// outline, not its middle section, touches the edges.
float sphereFramingDistance(float radius, float fovYDeg)
{
    return radius / std::sin(0.5f * qDegreesToRadians(fovYDeg));
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
        if (framingIsOrthographic(p, doc))
            shot.setCameraType(CameraShot::CameraType::Orthographic);
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
        // The cameras get the transform the meshes get. A camera moved by a uniform scale sees
        // the same image, so only its viewpoint moves and its intrinsics stay as they are.
        QMatrix4x4 transf = trTran * scaleMatrix(scale) * trTranInv;

        if (toall) {
            applyTransformToVisibleMeshes(doc, transf);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applySimilarity(transf);
            }
        } else if (toallRaster) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applySimilarity(transf);
            }
        } else if (camEnum == QStringLiteral("raster") || camEnum == QStringLiteral("mesh")) {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            doc.raster(ri).shot.applySimilarity(transf);
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

        // Rotation, then scale, then translation: one similarity, the same for the meshes and the
        // cameras, so that the cameras stay registered to the meshes they move with.
        const QMatrix4x4 transf = translateMatrix(transl.x(), transl.y(), transl.z()) * scaleMatrix(scale)
            * rotationMatrix({1,0,0}, rotDeg.x()) * rotationMatrix({0,1,0}, rotDeg.y())
            * rotationMatrix({0,0,1}, rotDeg.z());
        if (toall) {
            applyTransformToVisibleMeshes(doc, transf);
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applySimilarity(transf);
            }
        } else if (toallRaster) {
            for (int ri = 0; ri < doc.rasterCount(); ++ri) {
                auto &re = doc.raster(ri);
                if (re.visible && re.shot.isValid())
                    doc.raster(ri).shot.applySimilarity(transf);
            }
        } else if (camEnum == QStringLiteral("raster") || camEnum == QStringLiteral("mesh")) {
            int ri = doc.currentRasterIndex();
            if (ri < 0) return fail(QObject::tr("No current raster."));
            doc.raster(ri).shot.applySimilarity(transf);
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
                QVector3D viewDir = shot.directionToCamera(wv);
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
                QVector3D dir = re.shot.directionToCamera(wv);
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
        const auto &ent = doc.mesh(mi);
        const VCGMesh &m = ent.mesh;

        float margin = float(p.getDouble(QStringLiteral("marginFactor"), 1.0));
        float fovYDeg = float(p.getDouble(QStringLiteral("fovYDeg"), 45.0));
        bool useFaceNormals = p.getBool(QStringLiteral("useFaceNormals"), true);

        // What is framed is the vertices of the selected faces, or the selected vertices when
        // no face is; the way it faces is the average of the normals, all in world space. Face
        // normals come from the world-space corners, area-weighted; vertex normals go through
        // the layer matrix's normal transform.
        const QMatrix4x4 normalTransform = ent.transform.inverted().transposed();
        vcg::Box3f selBbox;
        selBbox.SetNull();
        QVector3D faceNormal(0, 0, 0), vertexNormal(0, 0, 0);
        bool anyFace = false;
        for (const auto &f : m.face) {
            if (!f.IsS() || f.IsD()) continue;
            QVector3D w[3];
            for (int k = 0; k < 3; ++k) {
                w[k] = transformPoint(ent.transform, f.cP(k));
                selBbox.Add(vcg::Point3f(w[k].x(), w[k].y(), w[k].z()));
                const vcg::Point3f n = f.cV(k)->cN();
                vertexNormal += normalTransform.mapVector(QVector3D(n[0], n[1], n[2])).normalized();
            }
            faceNormal += QVector3D::crossProduct(w[1] - w[0], w[2] - w[0]);
            anyFace = true;
        }
        if (!anyFace) {
            for (const auto &v : m.vert) {
                if (!v.IsS() || v.IsD()) continue;
                const QVector3D wv = transformPoint(ent.transform, v.cP());
                selBbox.Add(vcg::Point3f(wv.x(), wv.y(), wv.z()));
                vertexNormal += normalTransform.mapVector(QVector3D(v.cN()[0], v.cN()[1], v.cN()[2])).normalized();
            }
        }
        if (selBbox.IsNull())
            return fail(QObject::tr("No faces or vertices selected."));

        const QVector3D viewDir = (anyFace && useFaceNormals ? faceNormal : vertexNormal).normalized();
        if (viewDir.lengthSquared() < 0.01f) return fail(QObject::tr("Selection normal is zero."));

        const QVector3D center(selBbox.Center()[0], selBbox.Center()[1], selBbox.Center()[2]);
        const float radius = selBbox.Diag() * 0.5f;
        const float distance = sphereFramingDistance(radius, fovYDeg) * margin;
        // In front of the selection, where its normals point, looking back at it.
        const QVector3D eye = center + viewDir * distance;

        return framedCamera(doc, eye, center, radius, fovYDeg, framingIsOrthographic(p, doc),
                            QObject::tr("selection view"));
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

        const float distance = sphereFramingDistance(halfDiag, fovYDeg) * margin;
        QVector3D eye = center - direction * distance;

        return framedCamera(doc, eye, center, halfDiag, fovYDeg, framingIsOrthographic(p, doc),
                            QObject::tr("direction"));
    }

    // --- Set Camera to Frame from Viewpoint ---
    if (fid == QString::fromLatin1(kCameraFrameFromViewpoint)) {
        ViewTrackball::State state;
        QString error;
        QSize viewport;
        if (!ViewTrackball::stateFromCameraStateJson(p.getCameraState(QStringLiteral("camera")), state,
                                                     &error, &viewport))
            return fail(QObject::tr("Invalid camera: %1").arg(error));

        // The world-space vertices to frame: of the visible layers, of the current one, or of
        // its selection (the selected vertices and those of the selected faces).
        const QString target = p.getEnum(QStringLiteral("target"));
        std::vector<QVector3D> points;
        const auto addLayer = [&](int mi, bool selectedOnly) {
            const auto &ent = doc.mesh(mi);
            std::vector<bool> inSelectedFace;
            if (selectedOnly) {
                inSelectedFace.assign(ent.mesh.vert.size(), false);
                for (const auto &f : ent.mesh.face)
                    if (!f.IsD() && f.IsS())
                        for (int k = 0; k < 3; ++k)
                            inSelectedFace[size_t(vcg::tri::Index(ent.mesh, f.cV(k)))] = true;
            }
            for (size_t i = 0; i < ent.mesh.vert.size(); ++i) {
                const auto &v = ent.mesh.vert[i];
                if (!v.IsD() && (!selectedOnly || v.IsS() || inSelectedFace[i]))
                    points.push_back(transformPoint(ent.transform, v.cP()));
            }
        };
        if (target == QStringLiteral("visible")) {
            for (int mi = 0; mi < doc.meshCount(); ++mi)
                if (doc.mesh(mi).visible)
                    addLayer(mi, false);
        } else {
            const int mi = doc.currentMeshIndex();
            if (mi < 0) return fail(QObject::tr("No current mesh."));
            addLayer(mi, target == QStringLiteral("selection"));
        }

        const bool fitAspect = p.getEnum(QStringLiteral("imageShape")) == QStringLiteral("fit");
        float aspect = 0.0f;
        if (!fitAspect) {
            if (viewport.isEmpty())
                return fail(QObject::tr("The camera does not say how wide its image is (it has no viewport_px); "
                                        "fit the image to the target instead."));
            aspect = float(viewport.width()) / float(viewport.height());
        }
        ViewTrackball camera;
        camera.setState(state);
        const ViewTrackball::EyeFraming framing = camera.frameFromEye(points, aspect, fitAspect);
        if (framing != ViewTrackball::EyeFraming::Framed)
            return fail(ViewTrackball::eyeFramingText(framing));

        const QString json = ViewTrackball::cameraStateJson(camera.state(), fitAspect ? QSize() : viewport);
        MeshFilterRunResult result = ok({QObject::tr("Framed %1 vertices from the viewpoint: a field of view of "
                                                     "%2 degrees, %3 times as wide as it is tall.")
                                             .arg(points.size())
                                             .arg(double(camera.fovYDegrees()), 0, 'f', 2)
                                             .arg(double(aspect), 0, 'f', 4)});
        result.documentModified = false;
        result.cameraState = json;
        result.outputValues.insert(QStringLiteral("cameraState"), json);
        result.outputValues.insert(QStringLiteral("aspect"), double(aspect));
        result.outputValues.insert(QStringLiteral("fovYDeg"), double(camera.fovYDegrees()));
        return result;
    }

    return fail(QObject::tr("Unknown filter: %1").arg(fid));
}

void registerCameraFilterPlugin(MeshFilterPluginManager &pm)
{
    pm.registerPlugin(std::make_unique<CameraFilterPlugin>());
}

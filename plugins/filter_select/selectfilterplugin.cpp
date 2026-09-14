#include "selectfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "viewpointoccluder.h"
#include "viewtrackball.h"
#include <QColor>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/convex_hull.h>
#include <vcg/complex/algorithms/point_outlier.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/math/base.h>
#include <vcg/space/colorspace.h>
#include <vcg/space/triangle3.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {
constexpr QLatin1StringView kFilterSelectAll("select_all");
constexpr QLatin1StringView kFilterSelectNone("select_none");
constexpr QLatin1StringView kFilterSelectByAngle("select_faces_by_view_angle");
constexpr QLatin1StringView kFilterSelectVisibleVerts("select_visible_vertices");
constexpr QLatin1StringView kFilterSelectUgly("select_ill_shaped_faces");
constexpr QLatin1StringView kFilterSelectInvert("invert_selection");
constexpr QLatin1StringView kFilterSelectConnected("select_connected_faces");
constexpr QLatin1StringView kFilterSelectFaceFromVert("select_faces_from_vertices");
constexpr QLatin1StringView kFilterSelectVertFromFace("select_vertices_from_faces");
constexpr QLatin1StringView kFilterDeleteSelectedVerts("remove_selected_vertices");
constexpr QLatin1StringView kFilterDeleteAllFaces("remove_all_faces");
constexpr QLatin1StringView kFilterDeleteSelectedFaces("remove_selected_faces");
constexpr QLatin1StringView kFilterDeleteSelectedFaceVerts("remove_selected_faces_and_vertices");
constexpr QLatin1StringView kFilterSelectErode("erode_selection");
constexpr QLatin1StringView kFilterSelectDilate("dilate_selection");
constexpr QLatin1StringView kFilterSelectBorder("select_border");
constexpr QLatin1StringView kFilterSelectByFaceQuality("select_faces_by_scalar");
constexpr QLatin1StringView kFilterSelectByVertQuality("select_vertices_by_scalar");
constexpr QLatin1StringView kFilterSelectByColor("select_faces_by_color");
constexpr QLatin1StringView kFilterSelectSelfIntersect("select_self_intersecting_faces");
constexpr QLatin1StringView kFilterSelectTexBorder("select_vertex_texture_seams");
constexpr QLatin1StringView kFilterSelectNonManifoldFace("select_non_manifold_edges_vcglib");
constexpr QLatin1StringView kFilterSelectNonManifoldVertex("select_non_manifold_vertices");
constexpr QLatin1StringView kFilterSelectFacesByEdge("select_faces_by_edge_length");
constexpr QLatin1StringView kFilterSelectOutlier("select_outliers");
constexpr QLatin1StringView kFilterSelectByRectangle("select_by_screen_rectangle");


void updateGeometryAfterDeletion(VCGMesh &mesh)
{
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

}

QString SelectFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.select");
}

QString SelectFilterPlugin::name() const
{
    return QObject::tr("Selection Filters");
}

MeshFilterRunResult SelectFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    using Mask = vcg::tri::io::Mask;
    using Sel = vcg::tri::UpdateSelection<VCGMesh>;

    auto fail = [](const QString &msg) {
        MeshFilterRunResult result;
        result.success = false;
        result.documentModified = false;
        result.errorMessage = msg;
        return result;
    };

    auto selectionSummary = [](const VCGMesh &mesh) {
        return QObject::tr("Selection now contains %1 / %2 vertices and %3 / %4 faces.")
            .arg(Sel::VertexCount(mesh))
            .arg(mesh.VN())
            .arg(Sel::FaceCount(mesh))
            .arg(mesh.FN());
    };

    auto selectionResult = [&](int meshIndex, Document::MeshEntry &entry, const QString &changeMsg, QStringList extra = {}) {
        entry.ioMask |= (Mask::IOM_VERTFLAGS | Mask::IOM_FACEFLAGS);
        doc.markMeshSelectionChanged(meshIndex, changeMsg);
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = std::move(extra);
        result.infoMessages.push_back(selectionSummary(entry.mesh));
        return result;
    };

    auto interruptResult = []() {
        return MeshFilterRunResult{ false, false, QObject::tr("Filter interrupted by user.") };
    };

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    vcg::CallBackPos *cb = doc.progressCallback();

    if (filterId == QString::fromLatin1(kFilterSelectAll)) {
        if (params.getBool(QStringLiteral("allVerts")))
            Sel::VertexAll(mesh);
        if (params.getBool(QStringLiteral("allFaces")))
            Sel::FaceAll(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Select all on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectNone)) {
        if (params.getBool(QStringLiteral("allVerts")))
            Sel::VertexClear(mesh);
        if (params.getBool(QStringLiteral("allFaces")))
            Sel::FaceClear(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Clear selection on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectByRectangle)) {
        // Build the projection that maps a mesh element to normalized screen
        // coordinates. Two spaces are supported: the 3D trackball view, and the
        // UV/parametrization view (ortho projection of texture coordinates).
        const QString space = params.getEnum(QStringLiteral("space"));
        const bool uvSpace = (space == QStringLiteral("uv"));
        const float aspect = float(params.getDouble(QStringLiteral("aspect")));
        QMatrix4x4 mvp;
        vcg::Point3f eyeLocal(0.0f, 0.0f, 0.0f); // camera eye in mesh-local space (view3d)
        if (uvSpace) {
            // Rebuild the exact ortho MVP the UV renderer uses (pan/zoom/aspect).
            const float xLim = aspect >= 1.0f ? aspect : 1.0f;
            const float yLim = aspect >= 1.0f ? 1.0f : (1.0f / std::max(1e-6f, aspect));
            const float zoom = float(params.getDouble(QStringLiteral("uv_zoom")));
            QMatrix4x4 proj;
            proj.ortho(-xLim, xLim, -yLim, yLim, -1.0f, 1.0f);
            QMatrix4x4 model;
            model.scale(zoom, zoom, 1.0f);
            model.translate(-float(params.getDouble(QStringLiteral("uv_pan_x"))),
                            -float(params.getDouble(QStringLiteral("uv_pan_y"))), 0.0f);
            mvp = proj * model;
        } else {
            const QString camJson = params.getCameraState(QStringLiteral("camera_state"));
            QJsonParseError pe;
            const QJsonDocument jd = QJsonDocument::fromJson(camJson.toUtf8(), &pe);
            if (pe.error != QJsonParseError::NoError || !jd.isObject())
                return fail(QObject::tr("select_by_rectangle: invalid camera_state JSON."));
            const QJsonObject root = jd.object();
            const QJsonObject tbObj = root.contains(QStringLiteral("trackball"))
                ? root.value(QStringLiteral("trackball")).toObject()
                : root;
            ViewTrackball::State st;
            QString stErr;
            if (!ViewTrackball::stateFromJson(tbObj, st, &stErr))
                return fail(QObject::tr("select_by_rectangle: %1").arg(stErr));
            ViewTrackball cam;
            cam.setState(st);
            mvp = cam.projectionMatrix(aspect > 1e-6f ? aspect : 1.0f) * cam.viewMatrix() * entry.transform;
            const QVector3D e = entry.transform.inverted().map(cam.cameraEyePosition());
            eyeLocal = vcg::Point3f(e.x(), e.y(), e.z());
        }

        const double lox = std::min(params.getDouble(QStringLiteral("rect_min_x")),
                                    params.getDouble(QStringLiteral("rect_max_x")));
        const double hix = std::max(params.getDouble(QStringLiteral("rect_min_x")),
                                    params.getDouble(QStringLiteral("rect_max_x")));
        const double loy = std::min(params.getDouble(QStringLiteral("rect_min_y")),
                                    params.getDouble(QStringLiteral("rect_max_y")));
        const double hiy = std::max(params.getDouble(QStringLiteral("rect_min_y")),
                                    params.getDouble(QStringLiteral("rect_max_y")));

        // Project a point (3D position, or UV coordinate with z=0) and test it
        // against the normalized rect (origin bottom-left, y up).
        auto inRect = [&](float x, float y, float z) -> bool {
            const QVector4D clip = mvp * QVector4D(x, y, z, 1.0f);
            if (clip.w() <= 1e-6f)
                return false;
            const float sx = (clip.x() / clip.w()) * 0.5f + 0.5f;
            const float sy = (clip.y() / clip.w()) * 0.5f + 0.5f;
            return sx >= lox && sx <= hix && sy >= loy && sy <= hiy;
        };

        const QString element = params.getEnum(QStringLiteral("element"));
        const QString mode = params.getEnum(QStringLiteral("mode"));
        const bool doFaces = (element != QStringLiteral("vertex"));

        // "Visible only": keep only faces not occluded from the viewpoint. A ray
        // is cast from the camera eye to each candidate face centroid and the face
        // is dropped if any geometry lies in between. The occlusion query is
        // delegated to ViewpointOccluder (embree when built, vcglib grid otherwise),
        // cached by geometryRevision so repeated drags reuse it. 3D + faces only.
        const bool visibleOnly =
            params.getBool(QStringLiteral("visible_only")) && !uvSpace && doFaces && mesh.FN() > 0;
        std::shared_ptr<ViewpointOccluder> occluder;
        if (visibleOnly)
            occluder = ViewpointOccluder::getOrBuild(mesh, entry.geometryRevision);
        int changed = 0;

        if (doFaces && mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh has no faces."));

        // "Connected components": grow what the rectangle touched to the whole component
        // it belongs to. The growth has to start from the raw hits rather than from the
        // finished selection -- with Ctrl (subtract) the whole component under the
        // rectangle must come out, and growing the *remainder* would do very nearly the
        // opposite. So while expanding, the pass below marks hits only and the composition
        // mode is applied afterwards, against a snapshot taken here.
        const bool expandComponents =
            params.getBool(QStringLiteral("expand_to_components")) && mesh.FN() > 0;
        std::vector<bool> selectionBefore;
        if (expandComponents) {
            const std::size_t n = doFaces ? std::size_t(mesh.face.size())
                                          : std::size_t(mesh.vert.size());
            selectionBefore.resize(n, false);
            for (std::size_t i = 0; i < n; ++i)
                selectionBefore[i] = doFaces ? mesh.face[i].IsS() : mesh.vert[i].IsS();
        }
        // Marking hits means selecting them, whatever the requested mode.
        const bool subtract = !expandComponents && (mode == QStringLiteral("subtract"));
        if (expandComponents || mode == QStringLiteral("replace")) {
            if (doFaces)
                Sel::FaceClear(mesh);
            else
                Sel::VertexClear(mesh);
        }

        QElapsedTimer timer;
        timer.start();
        unsigned nThreads = 1;

        if (uvSpace) {
            // UV space: element coordinates come from per-corner parametrization,
            // so iterate faces once. A flat layout has no occlusion, so the plain
            // "select what projects inside" rule is exactly right. Single pass —
            // vertices are shared between faces, so parallel writes are avoided.
            const int ioMask = entry.ioMask;
            for (VCGFace &f : mesh.face) {
                if (f.IsD())
                    continue;
                float u[3], v[3];
                if (!vcgFaceCornerUV(ioMask, f, 0, u[0], v[0])
                    || !vcgFaceCornerUV(ioMask, f, 1, u[1], v[1])
                    || !vcgFaceCornerUV(ioMask, f, 2, u[2], v[2]))
                    continue;
                if (doFaces) {
                    const float cu = (u[0] + u[1] + u[2]) / 3.0f;
                    const float cv = (v[0] + v[1] + v[2]) / 3.0f;
                    if (!inRect(cu, cv, 0.0f))
                        continue;
                    if (subtract) {
                        if (f.IsS()) { f.ClearS(); ++changed; }
                    } else if (!f.IsS()) {
                        f.SetS(); ++changed;
                    }
                } else {
                    for (int c = 0; c < 3; ++c) {
                        VCGVertex *vp = f.V(c);
                        if (!vp || vp->IsD() || !inRect(u[c], v[c], 0.0f))
                            continue;
                        if (subtract) {
                            if (vp->IsS()) { vp->ClearS(); ++changed; }
                        } else if (!vp->IsS()) {
                            vp->SetS(); ++changed;
                        }
                    }
                }
            }
        } else {
            // 3D view space: parallelize over elements. Each writes only its own
            // BitFlags (a fixed per-element component), so ranges need no locking.
            auto worker = [&](std::size_t lo, std::size_t hi) -> int {
                int local = 0;
                if (doFaces) {
                    for (std::size_t i = lo; i < hi; ++i) {
                        VCGFace &f = mesh.face[i];
                        if (f.IsD())
                            continue;
                        const vcg::Point3f b = vcg::Barycenter(f);
                        if (!inRect(b.X(), b.Y(), b.Z()))
                            continue;
                        if (visibleOnly && occluder->isOccluded(b, eyeLocal))
                            continue;
                        if (subtract) {
                            if (f.IsS()) { f.ClearS(); ++local; }
                        } else if (!f.IsS()) {
                            f.SetS(); ++local;
                        }
                    }
                } else {
                    for (std::size_t i = lo; i < hi; ++i) {
                        VCGVertex &v = mesh.vert[i];
                        if (v.IsD())
                            continue;
                        const auto &p = v.P();
                        if (!inRect(p.X(), p.Y(), p.Z()))
                            continue;
                        if (subtract) {
                            if (v.IsS()) { v.ClearS(); ++local; }
                        } else if (!v.IsS()) {
                            v.SetS(); ++local;
                        }
                    }
                }
                return local;
            };

            const std::size_t elemCount = doFaces ? mesh.face.size() : mesh.vert.size();
            nThreads = std::max(1u, std::thread::hardware_concurrency());
            if (elemCount < 200000)
                nThreads = 1; // thread setup isn't worth it below this size

            if (nThreads <= 1) {
                changed = worker(0, elemCount);
            } else {
                std::vector<std::thread> pool;
                std::vector<int> counts(nThreads, 0);
                const std::size_t chunk = (elemCount + nThreads - 1) / nThreads;
                for (unsigned t = 0; t < nThreads; ++t) {
                    const std::size_t lo = std::size_t(t) * chunk;
                    const std::size_t hi = std::min(elemCount, lo + chunk);
                    if (lo >= hi)
                        break;
                    pool.emplace_back([&counts, &worker, t, lo, hi]() { counts[t] = worker(lo, hi); });
                }
                for (std::thread &th : pool)
                    th.join();
                for (int c : counts)
                    changed += c;
            }
        }

        std::size_t grownComponentFaces = 0;
        if (expandComponents) {
            {
                // FF adjacency is built here rather than declared in inputPrepare: every
                // drag would otherwise pay for it, and the ordinary case has no use for it.
                VCGMeshFFAdjScope _ffAdj(mesh);
                vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
                // vcglib only grows components face-wise, so a vertex rectangle goes out
                // through the faces it touched and comes back over all of theirs.
                if (!doFaces)
                    Sel::FaceFromVertexLoose(mesh);
                grownComponentFaces = Sel::FaceConnectedFF(mesh);
                if (!doFaces)
                    Sel::VertexFromFaceLoose(mesh);
            }

            // Now apply the requested composition against the snapshot.
            changed = 0;
            const std::size_t n = selectionBefore.size();
            for (std::size_t i = 0; i < n; ++i) {
                const bool before = selectionBefore[i];
                const bool grown = doFaces ? mesh.face[i].IsS() : mesh.vert[i].IsS();
                bool after = grown;
                if (mode == QStringLiteral("add"))
                    after = before || grown;
                else if (mode == QStringLiteral("subtract"))
                    after = before && !grown;
                if (after != before)
                    ++changed;
                if (doFaces) {
                    if (after) mesh.face[i].SetS(); else mesh.face[i].ClearS();
                } else {
                    if (after) mesh.vert[i].SetS(); else mesh.vert[i].ClearS();
                }
            }
        }

        const qint64 elapsedMs = timer.elapsed();
        const int total = doFaces ? mesh.FN() : mesh.VN();
        QStringList messages = {
            QObject::tr("Rectangle %1: %2 %3 affected.")
                .arg(mode)
                .arg(changed)
                .arg(doFaces ? QObject::tr("faces") : QObject::tr("vertices")),
            QObject::tr("Projection over %1 %2 took %3 ms across %4 thread(s) "
                        "(GPU overlay update happens afterwards on the render thread).")
                .arg(total)
                .arg(doFaces ? QObject::tr("faces") : QObject::tr("vertices"))
                .arg(elapsedMs)
                .arg(nThreads)
        };
        if (expandComponents)
            messages << QObject::tr("Expanded to whole connected components (%1 faces in the "
                                    "components the rectangle touched).")
                            .arg(grownComponentFaces);
        if (occluder)
            messages << QObject::tr("Visibility test via %1.")
                            .arg(occluder->usesEmbree() ? QObject::tr("embree")
                                                        : QObject::tr("vcglib grid"));
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Rectangle %1 on '%2'").arg(mode, entry.name),
            messages);
    }

    if (filterId == QString::fromLatin1(kFilterSelectVisibleVerts)) {
        if (mesh.VN() < 4)
            return fail(QObject::tr("Visibility selection needs at least 4 vertices."));

        if (params.getBool(QStringLiteral("usecamera"))) {
            return fail(QObject::tr(
                "Use ViewPoint from Mesh Camera is not supported in current MeshLab data model."));
        }

        const QVector3D vp = params.getPoint3f(QStringLiteral("viewpoint"));
        const vcg::Point3f viewpoint(float(vp.x()), float(vp.y()), float(vp.z()));
        const float logR = float(params.getDouble(QStringLiteral("radiusThreshold")));

        // Hidden point removal (Katz/Tal/Basri): mirror every point through a sphere
        // centred on the viewpoint, then take the convex hull of the mirrored set — the
        // points that land on it are the ones visible from the viewpoint.
        //
        // vcglib's ConvexHull::ComputePointVisibility packages exactly this, but it
        // asserts that the viewpoint ends up on the hull and aborts when it does not,
        // which is precisely what happens when the viewpoint sits inside the cloud — the
        // default (0,0,0) on any centred mesh. Composing ComputeConvexHull directly costs
        // a few lines, reports that case as an error instead of crashing, and keeps
        // vcglib's debug printfs out of the log.
        std::vector<int> sourceOfPoint;
        sourceOfPoint.reserve(std::size_t(std::max(0, mesh.VN())));
        float maxDist = 0.0f;
        for (const VCGVertex &v : mesh.vert) {
            if (v.IsD())
                continue;
            maxDist = std::max(maxDist, (v.cP() - viewpoint).Norm());
        }
        if (maxDist <= 0.0f)
            return fail(QObject::tr("All vertices coincide with the viewpoint."));

        const float radius = maxDist * std::pow(10.0f, logR);
        VCGMesh flipped;
        std::vector<int> visibleSources; // nothing is marked until every check has passed
        for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
            if (mesh.vert[i].IsD())
                continue;
            vcg::Point3f q = mesh.vert[i].cP() - viewpoint;
            const float d = q.Norm();
            if (d <= std::numeric_limits<float>::epsilon()) {
                // Sits on the viewpoint: trivially visible, and cannot be mirrored.
                visibleSources.push_back(int(i));
                continue;
            }
            q += q * (2.0f * (radius - d) / d);
            vcg::tri::Allocator<VCGMesh>::AddVertex(flipped, q);
            sourceOfPoint.push_back(int(i));
        }
        if (flipped.VN() < 4)
            return fail(QObject::tr("Visibility selection needs at least 4 live vertices."));

        // The viewpoint itself, at the origin of the mirrored frame. It is a hull vertex
        // exactly when the viewpoint lies outside the point set.
        const std::size_t viewpointPoint = sourceOfPoint.size();
        vcg::tri::Allocator<VCGMesh>::AddVertex(flipped, vcg::Point3f(0.0f, 0.0f, 0.0f));

        VCGMesh hull;
        {
            VCGMeshFFAdjScope ffAdj(hull);
            if (!vcg::tri::ConvexHull<VCGMesh, VCGMesh>::ComputeConvexHull(flipped, hull))
                return fail(QObject::tr("Visibility selection failed: the points are degenerate."));
        }

        auto indexInput = vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<std::size_t>(
            hull, std::string("indexInput"));
        bool viewpointOnHull = false;
        for (int i = 0; i < hull.VN(); ++i) {
            const std::size_t source = indexInput[i];
            if (source == viewpointPoint) {
                viewpointOnHull = true;
                continue;
            }
            if (source < sourceOfPoint.size())
                visibleSources.push_back(sourceOfPoint[source]);
        }
        if (!viewpointOnHull) {
            return fail(QObject::tr(
                "The viewpoint (%1, %2, %3) is enclosed by the point set, so visibility is "
                "undefined. Move it outside the mesh.")
                    .arg(QString::number(viewpoint.X(), 'g', 4))
                    .arg(QString::number(viewpoint.Y(), 'g', 4))
                    .arg(QString::number(viewpoint.Z(), 'g', 4)));
        }

        int selected = 0;
        for (int source : visibleSources) {
            VCGVertex &v = mesh.vert[std::size_t(source)];
            if (!v.IsS())
                ++selected;
            v.SetS();
        }

        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Select visible vertices on '%1'").arg(entry.name),
            { QObject::tr("Marked %1 vertices visible from (%2, %3, %4).")
                    .arg(selected)
                    .arg(QString::number(viewpoint.X(), 'g', 4))
                    .arg(QString::number(viewpoint.Y(), 'g', 4))
                    .arg(QString::number(viewpoint.Z(), 'g', 4)) });
    }

    if (filterId == QString::fromLatin1(kFilterSelectByAngle)) {
        if (mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh has no faces."));

        if (params.getBool(QStringLiteral("usecamera"))) {
            return fail(QObject::tr(
                "Use ViewPoint from Mesh Camera is not supported in current MeshLab data model."));
        }

        const QVector3D vp = params.getPoint3f(QStringLiteral("viewpoint"));
        const vcg::Point3f viewpoint(float(vp.x()), float(vp.y()), float(vp.z()));
        const float angleDeg = float(params.getDouble(QStringLiteral("anglelimit")));
        const float limit = std::cos(vcg::math::ToRad(angleDeg));

        int selected = 0;
        for (VCGFace &f : mesh.face) {
            vcg::Point3f viewray = vcg::Barycenter(f) - viewpoint;
            const float nrm = std::sqrt(viewray.SquaredNorm());
            if (nrm <= 1e-20f)
                continue;
            viewray /= nrm;
            vcg::Point3f n = f.cN();
            const float nn = std::sqrt(n.SquaredNorm());
            if (nn <= 1e-20f)
                continue;
            n /= nn;
            if (viewray.dot(n) < limit) {
                if (!f.IsS())
                    ++selected;
                f.SetS();
            }
        }

        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Select faces by view angle on '%1'").arg(entry.name),
            { QObject::tr("Marked %1 faces by angle threshold %2°.")
                    .arg(selected)
                    .arg(QString::number(angleDeg, 'f', 2)) });
    }

    if (filterId == QString::fromLatin1(kFilterSelectUgly)) {
        if (mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh has no faces."));

        Sel::Clear(mesh);
        int selectedByAR = 0;
        int selectedByNF = 0;
        int selectedByFolded = 0;

        if (params.getBool(QStringLiteral("useAR"))) {
            const float aRatio = float(params.getDouble(QStringLiteral("ARatio")));
            for (VCGFace &f : mesh.face) {
                const float q = vcg::QualityRadii(f.V(0)->P(), f.V(1)->P(), f.V(2)->P());
                if (q < aRatio) {
                    if (!f.IsS())
                        ++selectedByAR;
                    f.SetS();
                }
            }
        }

        if (params.getBool(QStringLiteral("useNF"))) {
            VCGMeshFFAdjScope _ffAdj(mesh);
            vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
            const float nfRatio = float(params.getDouble(QStringLiteral("NFRatio")));
            for (VCGFace &f : mesh.face) {
                float worstAngle = 0.0f;
                for (int ei = 0; ei < 3; ++ei) {
                    VCGFace *adjf = f.FFp(ei);
                    if (!adjf || adjf == &f)
                        continue;
                    vcg::Point3f n0 = f.N();
                    vcg::Point3f n1 = adjf->N();
                    const float nn0 = std::sqrt(n0.SquaredNorm());
                    const float nn1 = std::sqrt(n1.SquaredNorm());
                    if (nn0 <= 1e-20f || nn1 <= 1e-20f)
                        continue;
                    n0 /= nn0;
                    n1 /= nn1;
                    const float dot = std::clamp(n0.dot(n1), -1.0f, 1.0f);
                    const float angle = vcg::math::ToDeg(std::fabs(std::acos(dot)));
                    worstAngle = std::max(worstAngle, angle);
                }
                if (worstAngle > nfRatio) {
                    if (!f.IsS())
                        ++selectedByNF;
                    f.SetS();
                }
            }
        }

        if (params.getBool(QStringLiteral("select_folded_faces"))) {
            const float angleThr =
                float(params.getDouble(QStringLiteral("folded_faces_angle_threshold")));
            const int beforeSel = int(Sel::FaceCount(mesh));
            VCGMeshVFAdjScope _vfAdj(mesh);
            vcg::tri::UpdateTopology<VCGMesh>::VertexFace(mesh);
            vcg::tri::Clean<VCGMesh>::SelectFoldedFaceFromOneRingFaces(
                mesh,
                std::cos(vcg::math::ToRad(angleThr)));
            selectedByFolded = std::max(0, int(Sel::FaceCount(mesh)) - beforeSel);
        }

        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Select problematic faces on '%1'").arg(entry.name),
            {
                QObject::tr("Selected by aspect ratio: %1").arg(selectedByAR),
                QObject::tr("Selected by normal angle: %1").arg(selectedByNF),
                QObject::tr("Selected folded faces: %1").arg(selectedByFolded)
            });
    }

    if (filterId == QString::fromLatin1(kFilterSelectInvert)) {
        if (params.getBool(QStringLiteral("InvVerts")))
            Sel::VertexInvert(mesh);
        if (params.getBool(QStringLiteral("InvFaces")))
            Sel::FaceInvert(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Invert selection on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectConnected)) {
        if (mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh has no faces."));
        Sel::FaceConnectedFF(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Expanded connected face selection on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectFaceFromVert)) {
        const bool strict = params.getBool(QStringLiteral("Inclusive"));
        if (strict)
            Sel::FaceFromVertexStrict(mesh);
        else
            Sel::FaceFromVertexLoose(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Transferred vertex selection to faces on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectVertFromFace)) {
        const bool strict = params.getBool(QStringLiteral("Inclusive"));
        if (strict)
            Sel::VertexFromFaceStrict(mesh);
        else
            Sel::VertexFromFaceLoose(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Transferred face selection to vertices on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterDeleteSelectedVerts)) {
        const int selectedVerts = Sel::VertexCount(mesh);
        if (selectedVerts == 0) {
            MeshFilterRunResult result;
            result.success = true;
            result.documentModified = false;
            result.infoMessages = { QObject::tr("Nothing done: no vertex selected.") };
            return result;
        }

        const int beforeV = mesh.VN();
        const int beforeF = mesh.FN();
        Sel::FaceClear(mesh);
        Sel::FaceFromVertexLoose(mesh);
        for (VCGFace &f : mesh.face) {
            if (f.IsS())
                vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, f);
        }
        for (VCGVertex &v : mesh.vert) {
            if (v.IsS())
                vcg::tri::Allocator<VCGMesh>::DeleteVertex(mesh, v);
        }
        updateGeometryAfterDeletion(mesh);
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Deleted selected vertices from '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Deleted %1 vertices, %2 faces.")
                .arg(beforeV - mesh.VN())
                .arg(beforeF - mesh.FN())
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDeleteAllFaces)) {
        MeshFilterRunResult result;
        result.success = true;
        const int before = mesh.FN();
        if (before <= 0) {
            result.documentModified = false;
            result.infoMessages = { QObject::tr("Nothing done: no faces found in current mesh.") };
            return result;
        }

        for (VCGFace &f : mesh.face)
            vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, f);
        updateGeometryAfterDeletion(mesh);
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Deleted all faces from '%1'.").arg(entry.name));

        result.documentModified = true;
        result.infoMessages = { QObject::tr("Deleted all %1 faces.").arg(before - mesh.FN()) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDeleteSelectedFaces)) {
        const int selectedFaces = Sel::FaceCount(mesh);
        if (selectedFaces == 0) {
            MeshFilterRunResult result;
            result.success = true;
            result.documentModified = false;
            result.infoMessages = { QObject::tr("Nothing done: no faces selected.") };
            return result;
        }

        const int beforeF = mesh.FN();
        for (VCGFace &f : mesh.face) {
            if (f.IsS())
                vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, f);
        }
        updateGeometryAfterDeletion(mesh);
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Deleted selected faces from '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Deleted %1 faces.")
                .arg(beforeF - mesh.FN())
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDeleteSelectedFaceVerts)) {
        const int selectedFaces = Sel::FaceCount(mesh);
        if (selectedFaces == 0) {
            MeshFilterRunResult result;
            result.success = true;
            result.documentModified = false;
            result.infoMessages = { QObject::tr("Nothing done: no faces selected.") };
            return result;
        }

        const int beforeV = mesh.VN();
        const int beforeF = mesh.FN();
        Sel::VertexClear(mesh);
        Sel::VertexFromFaceStrict(mesh);
        for (VCGFace &f : mesh.face) {
            if (f.IsS())
                vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, f);
        }
        for (VCGVertex &v : mesh.vert) {
            if (v.IsS())
                vcg::tri::Allocator<VCGMesh>::DeleteVertex(mesh, v);
        }
        updateGeometryAfterDeletion(mesh);
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Deleted selected faces and vertices from '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Deleted %1 faces, %2 vertices.")
                .arg(beforeF - mesh.FN())
                .arg(beforeV - mesh.VN())
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterSelectErode)) {
        Sel::VertexFromFaceStrict(mesh);
        Sel::FaceFromVertexStrict(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Erode selection on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectDilate)) {
        Sel::VertexFromFaceLoose(mesh);
        Sel::FaceFromVertexLoose(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Dilate selection on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectBorder)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceBorder(mesh);
        Sel::FaceFromBorderFlag(mesh);
        Sel::VertexFromBorderFlag(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected border on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectByVertQuality)) {
        const float minQ = float(params.getDouble(QStringLiteral("minQ")));
        const float maxQ = float(params.getDouble(QStringLiteral("maxQ")));
        const bool inclusive = params.getBool(QStringLiteral("Inclusive"));
        Sel::VertexFromQualityRange(mesh, minQ, maxQ);
        if (inclusive)
            Sel::FaceFromVertexStrict(mesh);
        else
            Sel::FaceFromVertexLoose(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected by vertex quality on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectByFaceQuality)) {
        const float minQ = float(params.getDouble(QStringLiteral("minQ")));
        const float maxQ = float(params.getDouble(QStringLiteral("maxQ")));
        const bool inclusive = params.getBool(QStringLiteral("Inclusive"));
        Sel::FaceFromQualityRange(mesh, minQ, maxQ);
        if (inclusive)
            Sel::VertexFromFaceStrict(mesh);
        else
            Sel::VertexFromFaceLoose(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected by face quality on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectByColor)) {
        const QColor targetColor = params.getColor(QStringLiteral("Color"));
        const QString colorSpace =
            params.getEnum(QStringLiteral("ColorSpace")).toLower();
        const bool inclusive = params.getBool(QStringLiteral("Inclusive"));
        const float valueRH = float(params.getDouble(QStringLiteral("PercentRH")));
        const float valueGS = float(params.getDouble(QStringLiteral("PercentGS")));
        const float valueBV = float(params.getDouble(QStringLiteral("PercentBV")));

        const float red = targetColor.redF();
        const float green = targetColor.greenF();
        const float blue = targetColor.blueF();
        float hue = targetColor.hueF();
        if (hue < 0.0f)
            hue = 0.0f;
        const float saturation = targetColor.saturationF();
        const float value = targetColor.valueF();

        Sel::FaceClear(mesh);
        Sel::VertexClear(mesh);

        for (VCGVertex &v : mesh.vert) {
            vcg::Color4f cv = vcg::Color4f::Construct(v.C());
            if (colorSpace == QStringLiteral("hsv")) {
                cv = vcg::ColorSpace<float>::RGBtoHSV(cv);
                if (std::fabs(cv[0] - hue) <= valueRH
                    && std::fabs(cv[1] - saturation) <= valueGS
                    && std::fabs(cv[2] - value) <= valueBV) {
                    v.SetS();
                }
            } else {
                if (std::fabs(cv[0] - red) <= valueRH
                    && std::fabs(cv[1] - green) <= valueGS
                    && std::fabs(cv[2] - blue) <= valueBV) {
                    v.SetS();
                }
            }
        }

        if (inclusive)
            Sel::FaceFromVertexStrict(mesh);
        else
            Sel::FaceFromVertexLoose(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected by color on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectTexBorder)) {
        VCGMeshFFAdjScope _ffAdj(mesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceBorder(mesh);
        Sel::VertexFromBorderFlag(mesh);
        // Restore standard topology and border flags.
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceBorder(mesh);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected texture seams on '%1'").arg(entry.name));
    }

    if (filterId == QString::fromLatin1(kFilterSelectNonManifoldFace)) {

        const int nm = vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh, true);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected non manifold edges on '%1'").arg(entry.name),
            { QObject::tr("Non manifold edges found: %1").arg(nm) });
    }

    if (filterId == QString::fromLatin1(kFilterSelectNonManifoldVertex)) {

        const int nm = vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh, true);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected non manifold vertices on '%1'").arg(entry.name),
            { QObject::tr("Non manifold vertices found: %1").arg(nm) });
    }

    if (filterId == QString::fromLatin1(kFilterSelectSelfIntersect)) {
        std::vector<VCGFace *> intersFaces;
        vcg::tri::Clean<VCGMesh>::SelfIntersections(mesh, intersFaces);
        Sel::FaceClear(mesh);
        for (VCGFace *f : intersFaces) {
            if (f)
                f->SetS();
        }
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected self intersecting faces on '%1'").arg(entry.name),
            { QObject::tr("Self intersecting faces: %1").arg(int(intersFaces.size())) });
    }

    if (filterId == QString::fromLatin1(kFilterSelectFacesByEdge)) {
        const float threshold = float(params.getDouble(QStringLiteral("Threshold")));
        const int selFaceNum = Sel::FaceOutOfRangeEdge(mesh, 0.0f, threshold);
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected faces by edge length on '%1'").arg(entry.name),
            { QObject::tr("Selected %1 faces with an edge longer than %2.")
                    .arg(selFaceNum)
                    .arg(QString::number(threshold, 'f', 6)) });
    }

    if (filterId == QString::fromLatin1(kFilterSelectOutlier)) {
        if (mesh.VN() <= 0)
            return fail(QObject::tr("Current mesh has no vertices."));
        const float threshold = float(params.getDouble(QStringLiteral("PropThreshold")));
        const int kNearest = std::max(1, params.getInt(QStringLiteral("KNearest")));
        vcg::VertexConstDataWrapper<VCGMesh> wrapper(mesh);
        vcg::KdTree<VCGMesh::ScalarType> kdTree(wrapper);
        const int selVertexNum =
            vcg::tri::OutlierRemoval<VCGMesh>::SelectLoOPOutliers(mesh, kdTree, kNearest, threshold);
        if (doc.isOperationCancelRequested())
            return interruptResult();
        return selectionResult(
            meshIndex,
            entry,
            QObject::tr("Selected outliers on '%1'").arg(entry.name),
            { QObject::tr("Selected %1 outlier vertices.").arg(selVertexNum) });
    }

    return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerSelectFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<SelectFilterPlugin>());
}

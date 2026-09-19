#include "createfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"
#include <QVector3D>
#include <cmath>
#include <vector>
#include <vcg/complex/algorithms/convex_hull.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/point_sampling.h>
#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/math/gen_normal.h>
#include <vcg/math/matrix33.h>
#include <vcg/math/random_generator.h>
#include <vcg/space/fitting3.h>

namespace {
constexpr QLatin1StringView kFilterCreateHexahedron("create_hexahedron");
constexpr QLatin1StringView kFilterCreateAnnulus("create_annulus");
constexpr QLatin1StringView kFilterCreateCircle("create_circle");
constexpr QLatin1StringView kFilterCreateSquare("create_square");
constexpr QLatin1StringView kFilterCreateSphere("create_sphere");
constexpr QLatin1StringView kFilterCreateSphereCap("create_sphere_cap");
constexpr QLatin1StringView kFilterCreateSpherePoints("create_points_on_sphere");
constexpr QLatin1StringView kFilterCreateSphericalCapPoints("create_points_on_spherical_cap");
constexpr QLatin1StringView kFilterCreateIcosahedron("create_icosahedron");
constexpr QLatin1StringView kFilterCreateDodecahedron("create_dodecahedron");
constexpr QLatin1StringView kFilterCreateDodecahedronSym("create_symmetric_dodecahedron");
constexpr QLatin1StringView kFilterCreateTetrahedron("create_tetrahedron");
constexpr QLatin1StringView kFilterCreateOctahedron("create_octahedron");
constexpr QLatin1StringView kFilterCreateCone("create_cone");
constexpr QLatin1StringView kFilterCreateCylinder("create_cylinder");
constexpr QLatin1StringView kFilterCreateTorus("create_torus");
constexpr QLatin1StringView kFilterFitPlane("create_plane_from_selection");
constexpr QLatin1StringView kFilterConvexHull("create_convex_hull");

// A closed polyline through the given points: one vertex each, one edge each, the last
// joined back to the first. What makes a polyline a polyline rather than a point cloud is
// that the edge container is populated, so that is all this does.
void buildClosedPolyline(VCGMesh &m, const std::vector<vcg::Point3f> &points)
{
    m.Clear();
    const int n = int(points.size());
    auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(m, n);
    for (const vcg::Point3f &p : points) {
        vi->P() = p;
        ++vi;
    }
    auto ei = vcg::tri::Allocator<VCGMesh>::AddEdges(m, n);
    for (int i = 0; i < n; ++i) {
        ei->V(0) = &m.vert[i];
        ei->V(1) = &m.vert[(i + 1) % n];
        ++ei;
    }
}

MeshFilterRunResult success(const QString &name, int newIndex, const QStringList &extraInfo = {})
{
    MeshFilterRunResult r;
    r.success = true;
    r.documentModified = true;
    r.newMeshIndices = { newIndex };
    r.infoMessages = extraInfo;
    return r;
}
} // namespace

QString CreateFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.create");
}

QString CreateFilterPlugin::name() const
{
    return QObject::tr("Creation Filters");
}

MeshFilterRunResult CreateFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kFilterCreateTetrahedron)) {
        VCGMesh m;
        vcg::tri::Tetrahedron<VCGMesh>(m);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {});
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateIcosahedron)) {
        VCGMesh m;
        vcg::tri::Icosahedron<VCGMesh>(m);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {});
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateDodecahedron)) {
        VCGMesh m;
        vcg::tri::Dodecahedron<VCGMesh>(m);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(
            m,
            {},
            vcg::tri::io::Mask::IOM_BITPOLYGONAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateDodecahedronSym)) {
        VCGMesh m;
        vcg::tri::DodecahedronSym<VCGMesh>(m);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(
            m,
            {},
            vcg::tri::io::Mask::IOM_BITPOLYGONAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateOctahedron)) {
        VCGMesh m;
        vcg::tri::Octahedron<VCGMesh>(m);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {});
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateHexahedron)) {
        const double sz = params.getDouble(QStringLiteral("size"));
        const float hsz = float(sz) * 0.5f;
        VCGMesh m;
        vcg::Box3f b(vcg::Point3f(-hsz, -hsz, -hsz), vcg::Point3f(hsz, hsz, hsz));
        vcg::tri::Box<VCGMesh>(m, b);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(
            m,
            {},
            vcg::tri::io::Mask::IOM_BITPOLYGONAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateAnnulus)) {
        const float inner = float(params.getDouble(QStringLiteral("inner_radius")));
        const float outer = float(params.getDouble(QStringLiteral("outer_radius")));
        const int sides  = params.getInt(QStringLiteral("sides"));
        VCGMesh m;
        vcg::tri::Annulus<VCGMesh>(m, inner, outer, sides);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {});
        return success(doc.mesh(idx).name, idx);
    }

    const bool circle = filterId == QString::fromLatin1(kFilterCreateCircle);
    const bool square = filterId == QString::fromLatin1(kFilterCreateSquare);
    if (circle || square) {
        const QVector3D a = params.getPoint3f(QStringLiteral("axis"));
        vcg::Point3f normal(a.x(), a.y(), a.z());
        if (normal.SquaredNorm() <= 1e-20f)
            return { false, false, QObject::tr("Normal must be non-zero.") };
        normal.Normalize();

        // A square is the four-sided member of the same family, turned an eighth of a turn:
        // its corners sit on a circle of half its diagonal, and that phase is what puts its
        // sides square to the in-plane axes instead of its corners.
        const int sides = circle ? params.getInt(QStringLiteral("sides")) : 4;
        const float radius = circle
            ? float(params.getDouble(QStringLiteral("radius")))
            : float(params.getDouble(QStringLiteral("size"))) * float(M_SQRT1_2);
        const float phase = circle ? 0.0f : float(M_PI) * 0.25f;

        const vcg::Matrix33f turn =
            vcg::RotationMatrix(vcg::Point3f(0.0f, 0.0f, 1.0f), normal, true);
        std::vector<vcg::Point3f> ring;
        ring.reserve(std::size_t(sides));
        for (int i = 0; i < sides; ++i) {
            const float t = phase + 2.0f * float(M_PI) * float(i) / float(sides);
            ring.push_back(
                turn * vcg::Point3f(radius * std::cos(t), radius * std::sin(t), 0.0f));
        }

        VCGMesh m;
        buildClosedPolyline(m, ring);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        const int idx = doc.addMesh(
            m,
            {},
            vcg::tri::io::Mask::IOM_EDGEINDEX);
        return success(
            doc.mesh(idx).name,
            idx,
            { QObject::tr("Closed polyline: %1 vertices, %2 edges.").arg(sides).arg(sides) });
    }

    if (filterId == QString::fromLatin1(kFilterCreateSphere)) {
        const float radius = float(params.getDouble(QStringLiteral("radius")));
        const int subdiv   = params.getInt(QStringLiteral("subdiv"));
        VCGMesh m;
        vcg::tri::Sphere<VCGMesh>(m, subdiv);
        vcg::tri::UpdatePosition<VCGMesh>::Scale(m, radius);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {}, vcg::tri::io::Mask::IOM_VERTNORMAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateSphereCap)) {
        const float halfAngleDeg = float(params.getDouble(QStringLiteral("half_angle")));
        const int subdiv         = params.getInt(QStringLiteral("subdiv"));
        VCGMesh m;
        m.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
        // vcg::tri::SphericalCap takes the cap's *full* angular diameter, while every
        // other cone and cap parameter in MeshLab is a half-angle. Convert here rather
        // than changing the vcglib signature, which MeshLab also compiles against.
        vcg::tri::SphericalCap(m, vcg::math::ToRad(2.0f * halfAngleDeg), subdiv);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {}, vcg::tri::io::Mask::IOM_VERTNORMAL);
        return success(doc.mesh(idx).name, idx);
    }

    const bool spherePoints = filterId == QString::fromLatin1(kFilterCreateSpherePoints);
    const bool capPoints = filterId == QString::fromLatin1(kFilterCreateSphericalCapPoints);
    if (spherePoints || capPoints) {
        const int pointNum = params.getInt(QStringLiteral("point_num"));
        const QString tech = params.getEnum(QStringLiteral("technique"));
        std::vector<vcg::Point3f> sampleVec;
        const bool randomized = tech == QLatin1StringView("montecarlo");
        QStringList seedInfo;
        vcg::Point3f capDirection(0, 1, 0);
        float capAngle = 0;
        if (capPoints) {
            const QVector3D d = params.getPoint3f(QStringLiteral("direction"));
            capDirection = vcg::Point3f(d.x(), d.y(), d.z());
            if (capDirection.SquaredNorm() <= 1e-20f)
                return { false, false, QObject::tr("Cap direction must be non-zero.") };
            capAngle = vcg::math::ToRad(float(params.getDouble(QStringLiteral("half_angle"))));
        }

        if (randomized) {
            const RandomSeed seed = params.getRandomSeed();
            vcg::math::MarsenneTwisterRNG rng{ seed.value };
            seedInfo = { seed.message() };
            if (capPoints) {
                for (int i = 0; i < pointNum; ++i)
                    sampleVec.push_back(vcg::math::GeneratePointOnUnitSphereCapUniform(
                        rng, capAngle, capDirection));
            } else {
                for (int i = 0; i < pointNum; ++i)
                    sampleVec.push_back(vcg::math::GeneratePointOnUnitSphereUniform<float>(rng));
            }
        } else if (capPoints) {
            vcg::GenNormal<float>::UniformCone(
                pointNum, sampleVec, capAngle, capDirection);
        } else if (tech == QLatin1StringView("discoball")) {
            vcg::GenNormal<float>::DiscoBall(pointNum, sampleVec);
        } else if (tech == QLatin1StringView("octahedron")) {
            vcg::GenNormal<float>::RecursiveOctahedron(pointNum, sampleVec);
        } else { // fibonacci (default)
            vcg::GenNormal<float>::Fibonacci(pointNum, sampleVec);
        }

        VCGMesh m;
        vcg::tri::Allocator<VCGMesh>::AddVertices(m, int(sampleVec.size()));
        for (int i = 0; i < int(sampleVec.size()); ++i) {
            m.vert[size_t(i)].P() = sampleVec[size_t(i)];
            m.vert[size_t(i)].N() = sampleVec[size_t(i)];
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        const int idx = doc.addMesh(
            m,
            {});
        return success(doc.mesh(idx).name, idx, seedInfo);
    }

    if (filterId == QString::fromLatin1(kFilterCreateCone)) {
        const float r0    = float(params.getDouble(QStringLiteral("r0")));
        const float r1    = float(params.getDouble(QStringLiteral("r1")));
        const float h     = float(params.getDouble(QStringLiteral("h")));
        const int subdiv  = params.getInt(QStringLiteral("subdiv"));
        VCGMesh m;
        vcg::tri::Cone<VCGMesh>(m, r0, r1, h, subdiv);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {}, vcg::tri::io::Mask::IOM_VERTNORMAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateCylinder)) {
        const float radius = float(params.getDouble(QStringLiteral("radius")));
        const float height = float(params.getDouble(QStringLiteral("height")));
        const int sides    = params.getInt(QStringLiteral("sides"));
        const int stacks   = params.getInt(QStringLiteral("stacks"));
        const bool capped  = params.getBool(QStringLiteral("capped"));
        const QVector3D a  = params.getPoint3f(QStringLiteral("axis"));
        vcg::Point3f axis(a.x(), a.y(), a.z());
        if (axis.SquaredNorm() <= 1e-20f)
            return { false, false, QObject::tr("Axis must be non-zero.") };
        axis.Normalize();

        // Centred on the origin and spanning half the height either side, which is what
        // vcg::tri::Cone does — a cylinder and a cone of the same height should occupy
        // the same space.
        const vcg::Point3f half = axis * (height / 2.0f);
        VCGMesh m;
        vcg::tri::OrientedCylinder<VCGMesh>(m, -half, half, radius, capped, sides, stacks);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {}, vcg::tri::io::Mask::IOM_VERTNORMAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterCreateTorus)) {
        const float hRadius = float(params.getDouble(QStringLiteral("h_radius")));
        const float vRadius = float(params.getDouble(QStringLiteral("v_radius")));
        const int hSubdiv   = params.getInt(QStringLiteral("h_subdiv"));
        const int vSubdiv   = params.getInt(QStringLiteral("v_subdiv"));
        VCGMesh m;
        vcg::tri::Torus(m, hRadius, vRadius, hSubdiv, vSubdiv);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);
        const int idx = doc.addMesh(m, {}, vcg::tri::io::Mask::IOM_VERTNORMAL);
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterFitPlane)) {
        const int meshIndex = doc.currentMeshIndex();
        if (meshIndex < 0 || meshIndex >= doc.meshCount())
            return { false, false, QObject::tr("No current mesh selected.") };

        const VCGMesh &src = doc.mesh(meshIndex).mesh;

        // Collect selected vertices (or all vertices from selected faces if no vertex selection)
        std::vector<vcg::Point3f> pts;
        int svn = 0;
        int sfn = 0;
        for (const auto &v : src.vert)
            if (v.IsS()) ++svn;
        for (const auto &f : src.face)
            if (f.IsS()) ++sfn;

        if (svn == 0 && sfn == 0)
            return { false, false, QObject::tr("No selection: select vertices or faces first.") };

        vcg::Point3f Naccum(0.f, 0.f, 0.f);
        if (svn > 0) {
            // Use selected vertices directly
            for (const auto &v : src.vert)
                if (v.IsS()) {
                    pts.push_back(v.cP());
                    Naccum += v.cN();
                }
        } else {
            // Expand face selection to vertices
            for (const auto &f : src.face)
                if (f.IsS())
                    for (int i = 0; i < 3; ++i) {
                        pts.push_back(f.cV(i)->cP());
                        Naccum += f.cV(i)->cN();
                    }
        }

        if (pts.size() < 3)
            return { false, false, QObject::tr("Need at least 3 selected points to fit a plane.") };

        vcg::Plane3f plane;
        vcg::FitPlaneToPointSet(pts, plane);
        plane.Normalize();
        Naccum.Normalize();
        if ((plane.Direction() * Naccum) < 0.f)
            plane.Set(-plane.Direction(), -plane.Offset());

        // Centre of projected selection
        vcg::Point3f centerP(0.f, 0.f, 0.f);
        for (const auto &p : pts)
            centerP += plane.Projection(p);
        centerP /= float(pts.size());

        // Horizontal axis: pick the world axis least parallel to plane normal
        const vcg::Point3f &n = plane.Direction();
        vcg::Point3f dirH;
        if (std::abs(n.X()) <= std::abs(n.Y()) && std::abs(n.X()) <= std::abs(n.Z()))
            dirH = vcg::Point3f(1.f, 0.f, 0.f) ^ n;
        else if (std::abs(n.Y()) <= std::abs(n.X()) && std::abs(n.Y()) <= std::abs(n.Z()))
            dirH = vcg::Point3f(0.f, 1.f, 0.f) ^ n;
        else
            dirH = vcg::Point3f(0.f, 0.f, 1.f) ^ n;
        dirH.Normalize();
        vcg::Point3f dirV = dirH ^ n;
        dirV.Normalize();

        // Extent
        const float extent = float(params.getDouble(QStringLiteral("extent")));
        float dimH = 0.f, dimV = 0.f;
        for (const auto &p : pts) {
            vcg::Point3f pp = plane.Projection(p);
            dimH = std::max(dimH, std::abs((pp - centerP) * dirH));
            dimV = std::max(dimV, std::abs((pp - centerP) * dirV));
        }
        dimH *= extent;
        dimV *= extent;

        const int subdiv = std::max(2, params.getInt(QStringLiteral("subdiv")) + 1);

        VCGMesh m;
        for (int ir = 0; ir < subdiv; ++ir)
            for (int ic = 0; ic < subdiv; ++ic) {
                vcg::Point3f p = (centerP + (dirV * -dimV) + (dirH * -dimH))
                    + dirH * (ic * 2.f * dimH / (subdiv - 1))
                    + dirV * (ir * 2.f * dimV / (subdiv - 1));
                vcg::tri::Allocator<VCGMesh>::AddVertex(m, p, n);
            }
        vcg::tri::FaceGrid(m, subdiv, subdiv);
        vcg::tri::UpdateBounding<VCGMesh>::Box(m);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);

        const int idx = doc.addMesh(m, {});
        return success(doc.mesh(idx).name, idx);
    }

    if (filterId == QString::fromLatin1(kFilterConvexHull)) {
        const int meshIndex = doc.currentMeshIndex();
        if (meshIndex < 0 || meshIndex >= doc.meshCount())
            return { false, false, QObject::tr("No current mesh selected.") };

        const VCGMesh &src = doc.mesh(meshIndex).mesh;
        if (src.VN() < 4)
            return { false, false, QObject::tr("A convex hull needs at least 4 vertices.") };

        // ComputeConvexHull compacts its input and clears the visited flags on it, so it
        // must never be handed the document's own mesh.
        VCGMesh work;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(work, src);

        VCGMesh hull;
        {
            VCGMeshFFAdjScope ffAdj(hull); // RequireFFAdjacency on the output mesh
            if (!vcg::tri::ConvexHull<VCGMesh, VCGMesh>::ComputeConvexHull(work, hull))
                return { false, false,
                    QObject::tr("Convex hull computation failed: the points may be degenerate "
                                "(all coincident, collinear, or coplanar).") };
        }
        // "indexInput" maps hull vertices back to the source; ancillary, so drop it rather
        // than carry it into the document and its undo snapshots.
        vcg::tri::Allocator<VCGMesh>::DeletePerVertexAttribute(hull, std::string("indexInput"));
        vcg::tri::UpdateBounding<VCGMesh>::Box(hull);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(hull);

        const int idx = doc.addMesh(
            hull,
            {},
            vcg::tri::io::Mask::IOM_VERTNORMAL);
        if (idx < 0)
            return { false, false, QObject::tr("Failed to add the convex hull layer.") };
        MeshFilterRunResult r = success(doc.mesh(idx).name, idx);
        r.infoMessages << QObject::tr("Hull: %1 vertices, %2 faces (from %3 input vertices).")
                              .arg(hull.VN()).arg(hull.FN()).arg(src.VN());
        return r;
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerCreateFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<CreateFilterPlugin>());
}

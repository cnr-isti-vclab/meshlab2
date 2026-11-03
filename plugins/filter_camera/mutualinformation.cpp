#include "mutualinformation.h"

#include "camerashot.h"
#include "document.h"
#include "filterparam.h"
#include "softdepthbuffer.h"
#include "vcgmesh.h"

#include <vcg/math/shot_resection.h>

#include <QElapsedTimer>
#include <QImage>
#include <QMatrix3x3>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

MeshFilterRunResult fail(const QString &m) { return {false, false, m}; }

// What the rendering shows of the mesh, as old MeshLab's OpenGL programs did; each is the one
// channel they read back. Normal Map: the normal's component up the image, in eye space.
// Combined: the vertex color blended with that by t = red^2, red being meant to hold ambient
// occlusion. Vertex Color: its red.
enum class Rendering { NormalMap, Combined, VertexColor };

constexpr int kBins = 128;

// The visible layers rendered on the CPU through a camera, at the size of the photo they are
// compared with, and the mutual information of the two images.
class MutualInformation
{
public:
    MutualInformation(const Document &doc, const QImage &photo, Rendering rendering, double backgroundWeight)
        : m_doc(doc), m_width(photo.width()), m_height(photo.height()), m_rendering(rendering),
          m_backgroundWeight(backgroundWeight)
    {
        // The photo in grey levels, rows bottom up as the camera's pixels are; transparent
        // pixels, where an undistorted photo has nothing, take no part
        const size_t n = size_t(m_width) * size_t(m_height);
        m_target.resize(n);
        m_valid.resize(n);
        for (int row = 0; row < m_height; ++row) {
            const uchar *line = photo.constScanLine(row);
            for (int x = 0; x < m_width; ++x) {
                const uchar *rgba = line + 4 * x;
                const size_t i = size_t(m_height - 1 - row) * size_t(m_width) + size_t(x);
                m_target[i] = uchar(0.3f * rgba[0] + 0.59f * rgba[1] + 0.11f * rgba[2]);
                m_valid[i] = rgba[3] > 0;
            }
        }
        m_zbuf.resize(n);
        m_render.resize(n);
    }

    double operator()(const CameraShot &shot)
    {
        render(shot);
        return information();
    }

private:
    const Document &m_doc;
    int m_width, m_height;
    Rendering m_rendering;
    double m_backgroundWeight;
    std::vector<uchar> m_target, m_render;
    std::vector<bool> m_valid;
    std::vector<float> m_zbuf;
    std::vector<QVector2D> m_proj;
    std::vector<float> m_depth;
    std::vector<QVector3D> m_normal;

    void render(const CameraShot &shot)
    {
        std::fill(m_zbuf.begin(), m_zbuf.end(), std::numeric_limits<float>::max());
        std::fill(m_render.begin(), m_render.end(), uchar(0));
        const QMatrix4x4 view = shot.viewMatrix();
        for (int mi = 0; mi < m_doc.meshCount(); ++mi) {
            const Document::MeshEntry &layer = m_doc.mesh(mi);
            if (!layer.visible)
                continue;
            const VCGMesh &mesh = layer.mesh;
            // Normals as the OpenGL pipeline had them: through the layer's and the view's
            // matrices, interpolated, normalized at each pixel
            const QMatrix4x4 normalToEye = view * QMatrix4x4(layer.transform.normalMatrix());
            m_proj.resize(mesh.vert.size());
            m_depth.resize(mesh.vert.size());
            m_normal.resize(mesh.vert.size());
            for (size_t vi = 0; vi < mesh.vert.size(); ++vi) {
                const QVector3D p = transformPoint(layer.transform, mesh.vert[vi].cP());
                m_proj[vi] = shot.project(p);
                m_depth[vi] = shot.depth(p);
                const vcg::Point3f &n = mesh.vert[vi].cN();
                m_normal[vi] = normalToEye.mapVector(QVector3D(n[0], n[1], n[2]));
            }
            for (const auto &f : mesh.face) {
                if (f.IsD())
                    continue;
                const size_t i0 = size_t(vcg::tri::Index(mesh, f.cV(0)));
                const size_t i1 = size_t(vcg::tri::Index(mesh, f.cV(1)));
                const size_t i2 = size_t(vcg::tri::Index(mesh, f.cV(2)));
                const vcg::Color4b &c0 = f.cV(0)->cC(), &c1 = f.cV(1)->cC(), &c2 = f.cV(2)->cC();
                rasterizeTriangleDepth(m_zbuf, m_width, m_height, m_proj[i0], m_proj[i1], m_proj[i2],
                                       m_depth[i0], m_depth[i1], m_depth[i2],
                                       [&](size_t i, float b0, float b1, float b2) {
                    const auto color = [&](int k) { return (b0 * c0[k] + b1 * c1[k] + b2 * c2[k]) / 255.0f; };
                    const auto up = [&]() {
                        return (b0 * m_normal[i0] + b1 * m_normal[i1] + b2 * m_normal[i2]).normalized().y() * 0.5f + 0.5f;
                    };
                    float value = 0;
                    switch (m_rendering) {
                    case Rendering::NormalMap: value = up(); break;
                    case Rendering::Combined: {
                        const float t = color(0) * color(0);
                        value = (1 - t) * color(1) + t * up();
                        break;
                    }
                    case Rendering::VertexColor: value = color(0); break;
                    }
                    m_render[i] = uchar(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                });
            }
        }
    }

    // The mutual information, in bits, of the photo and the rendering over a joint histogram
    // of 128 x 128 bins. The rendering's darkest bin, which holds its background, counts
    // 1 / backgroundWeight as much as the others, as old MeshLab weighed it.
    double information() const
    {
        std::vector<double> joint(size_t(kBins) * kBins, 0.0);
        const double background = m_backgroundWeight > 0 ? 1.0 / m_backgroundWeight : 0.0;
        for (size_t i = 0; i < m_target.size(); ++i) {
            if (!m_valid[i])
                continue;
            const int a = m_target[i] >> 1, b = m_render[i] >> 1;
            joint[size_t(a + kBins * b)] += b == 0 ? background : 1.0;
        }
        double histoA[kBins] = {}, histoB[kBins] = {}, n = 0;
        for (int b = 0; b < kBins; ++b)
            for (int a = 0; a < kBins; ++a) {
                const double ab = joint[size_t(a + kBins * b)];
                histoA[a] += ab;
                histoB[b] += ab;
                n += ab;
            }
        if (n == 0)
            return 0;
        double m = 0;
        for (int b = 0; b < kBins; ++b)
            for (int a = 0; a < kBins; ++a) {
                const double ab = joint[size_t(a + kBins * b)];
                if (ab > 0)
                    m += ab * std::log2(n * ab / (histoA[a] * histoB[b]));
            }
        return m / n;
    }
};

CameraShot toCameraShot(const vcg::Shotd &shot) { return CameraShot::fromVcgShot(vcg::Shotf::Construct(shot)); }

} // namespace

MeshFilterRunResult alignRasterByMutualInformation(const FilterParams &p, Document &doc)
{
    const int ri = doc.currentRasterIndex();
    if (ri < 0)
        return fail(QObject::tr("No current raster."));
    Document::RasterEntry &raster = doc.raster(ri);
    if (!raster.shot.isValid())
        return fail(QObject::tr("Raster '%1' has no camera to start from; set one, for example from a view.").arg(raster.name));
    if (raster.shot.isOrthographic())
        return fail(QObject::tr("Raster '%1' has an orthographic camera; only a perspective one can be aligned.").arg(raster.name));
    RasterPlane *plane = raster.currentPlane();
    if (plane)
        Document::ensureRasterPlaneImage(raster, *plane);
    if (!plane || plane->image.isNull())
        return fail(QObject::tr("Raster '%1' has no image to align to.").arg(raster.name));

    // What the camera is fitted against: the visible layers' vertices it sees, for scaling the
    // parameters, and the centre of their box, which the camera turns about
    std::vector<vcg::Point3d> samples;
    vcg::Box3d box;
    size_t faces = 0, vertices = 0;
    for (int mi = 0; mi < doc.meshCount(); ++mi)
        if (doc.mesh(mi).visible) {
            faces += size_t(doc.mesh(mi).mesh.FN());
            vertices += doc.mesh(mi).mesh.vert.size();
        }
    if (faces == 0)
        return fail(QObject::tr("No visible layer has faces to render."));
    const size_t stride = std::max<size_t>(1, vertices / 4000);
    const QSize viewport = raster.shot.viewportPx();
    for (int mi = 0; mi < doc.meshCount(); ++mi) {
        const Document::MeshEntry &layer = doc.mesh(mi);
        if (!layer.visible)
            continue;
        for (size_t vi = 0; vi < layer.mesh.vert.size(); ++vi) {
            const QVector3D q = transformPoint(layer.transform, layer.mesh.vert[vi].cP());
            const vcg::Point3d point(q.x(), q.y(), q.z());
            box.Add(point);
            const QVector2D px = raster.shot.project(q);
            if (vi % stride == 0 && raster.shot.depth(q) > 0 && px.x() >= 0 && px.y() >= 0
                && px.x() <= viewport.width() && px.y() <= viewport.height())
                samples.push_back(point);
        }
    }
    if (samples.size() < 3)
        return fail(QObject::tr("The raster's camera sees too little of the visible layers to align it."));

    // The photo and the camera at the working resolution: the longest side at most this
    const int resolution = std::max(16, p.getInt(QStringLiteral("resolution"), 800));
    const QSize imageSize = plane->image.size();
    const double factor = std::min(1.0, double(resolution) / std::max(imageSize.width(), imageSize.height()));
    const QSize size(std::max(1, int(std::lround(imageSize.width() * factor))),
                     std::max(1, int(std::lround(imageSize.height() * factor))));
    const QImage photo = plane->image.convertToFormat(QImage::Format_RGBA8888)
                             .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    CameraShot start = raster.shot;
    start.rescaleViewportPx(size);

    const QString renderingId = p.getEnum(QStringLiteral("rendering"));
    const Rendering rendering = renderingId == QStringLiteral("normalMap") ? Rendering::NormalMap
        : renderingId == QStringLiteral("vertexColor")                    ? Rendering::VertexColor
                                                                          : Rendering::Combined;
    MutualInformation information(doc, photo, rendering, p.getDouble(QStringLiteral("backgroundWeight"), 2.0));

    const bool estimateFocal = p.getBool(QStringLiteral("estimateFocal"), false);
    const int maxEvaluations = std::max(1, p.getInt(QStringLiteral("maxEvaluations"), 300));
    const double expectedError = std::max(1e-3, p.getDouble(QStringLiteral("expectedError"), 2.0));
    const double tolerance = std::clamp(p.getDouble(QStringLiteral("tolerance"), 0.1), 1e-4, expectedError);
    vcg::CallBackPos *cb = doc.progressCallback();
    QElapsedTimer timer;
    timer.start();

    // NEWUOA minimizes; a canceled run returns the worst value without rendering, so that the
    // rest of its evaluations cost nothing and the rounds stop
    int evaluations = 0;
    bool canceled = false;
    vcg::Shotd shot = vcg::Shotd::Construct(start.toVcgShot());
    const vcg::Point3d pivot = box.Center();
    const double before = information(start);
    double best = before;
    while (evaluations < maxEvaluations && !canceled) {
        const vcg::ShotResectiond::Parametrization parameters(shot, samples, pivot, estimateFocal);
        auto objective = [&](int, double *x) {
            if (canceled)
                return 0.0;
            ++evaluations;
            if (cb && !cb(std::min(99, 100 * evaluations / maxEvaluations),
                          qPrintable(QObject::tr("Mutual information: %1 bits").arg(best, 0, 'f', 4)))) {
                canceled = true;
                return 0.0;
            }
            return -information(toCameraShot(parameters.ToShot(x)));
        };
        double x[7] = {0, 0, 0, 0, 0, 0, 0};
        min_newuoa(parameters.Size(), x, objective, expectedError, tolerance, maxEvaluations - evaluations);
        if (canceled)
            break;
        const vcg::Shotd fitted = parameters.ToShot(x);
        const double value = information(toCameraShot(fitted));
        // Another round starts where this one ended, with the scales measured again there
        if (!(value > best + 1e-6))
            break;
        shot = fitted;
        best = value;
    }
    if (canceled)
        return fail(QObject::tr("Operation canceled."));

    // The camera at the raster's own size: only where it stands and looks, and its focal
    // length, changed
    CameraShot::VcgShot result = raster.shot.toVcgShot();
    const CameraShot::VcgShot fitted = toCameraShot(shot).toVcgShot();
    result.Extrinsics = fitted.Extrinsics;
    result.Intrinsics.FocalMm = fitted.Intrinsics.FocalMm;
    const CameraShot aligned = CameraShot::fromVcgShot(result);
    double moved = 0;
    for (const vcg::Point3d &s : samples) {
        const QVector3D q{float(s[0]), float(s[1]), float(s[2])};
        moved += double((aligned.project(q) - raster.shot.project(q)).lengthSquared());
    }
    moved = std::sqrt(moved / double(samples.size()));

    const QString message =
        QObject::tr("Aligned raster '%1' by mutual information at %2 x %3: %4 bits before, %5 after, "
                    "in %6 evaluations and %7 s; the camera moved its projections by %8 px.")
            .arg(raster.name).arg(size.width()).arg(size.height())
            .arg(before, 0, 'f', 4).arg(best, 0, 'f', 4).arg(evaluations)
            .arg(timer.elapsed() / 1000.0, 0, 'f', 2).arg(moved, 0, 'f', 2);
    doc.setRasterShot(ri, aligned, message);
    MeshFilterRunResult r{true, true, {}};
    r.infoMessages = {message};
    r.outputValues.insert(QStringLiteral("informationBefore"), before);
    r.outputValues.insert(QStringLiteral("informationAfter"), best);
    r.outputValues.insert(QStringLiteral("evaluations"), evaluations);
    r.outputValues.insert(QStringLiteral("displacement"), moved);
    return r;
}

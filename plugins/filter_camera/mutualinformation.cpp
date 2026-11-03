#include "mutualinformation.h"

#include "camerashot.h"
#include "document.h"
#include "filterparam.h"
#include "softdepthbuffer.h"
#include "vcgmesh.h"

#include <vcg/math/shot_resection.h>

#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
// occlusion. Vertex Color: its red. Silhouette: the layers white on black.
enum class Rendering { NormalMap, Combined, VertexColor, Silhouette };

Rendering renderingParam(const FilterParams &p)
{
    const QString id = p.getEnum(QStringLiteral("rendering"));
    return id == QStringLiteral("normalMap")     ? Rendering::NormalMap
         : id == QStringLiteral("vertexColor")   ? Rendering::VertexColor
         : id == QStringLiteral("silhouette")    ? Rendering::Silhouette
                                                 : Rendering::Combined;
}

constexpr int kBins = 128;

// A raster at the working resolution, the longest side at most `resolution`: its camera for
// that size, and its photo in grey levels with rows bottom up, as the camera's pixels are.
// Transparent pixels, where an undistorted photo has nothing, are not valid.
struct View
{
    int raster = -1;
    int width = 0, height = 0;
    CameraShot shot;
    std::vector<uchar> grey;
    std::vector<bool> valid;
};

QString makeView(Document &doc, int ri, int resolution, View &view)
{
    Document::RasterEntry &raster = doc.raster(ri);
    if (!raster.shot.isValid())
        return QObject::tr("Raster '%1' has no camera to start from; set one, for example from a view.").arg(raster.name);
    if (raster.shot.isOrthographic())
        return QObject::tr("Raster '%1' has an orthographic camera; only a perspective one can be aligned.").arg(raster.name);
    RasterPlane *plane = raster.currentPlane();
    if (plane)
        Document::ensureRasterPlaneImage(raster, *plane);
    if (!plane || plane->image.isNull())
        return QObject::tr("Raster '%1' has no image to align to.").arg(raster.name);
    const QSize imageSize = plane->image.size();
    const double factor = std::min(1.0, double(resolution) / std::max(imageSize.width(), imageSize.height()));
    view.raster = ri;
    view.width = std::max(1, int(std::lround(imageSize.width() * factor)));
    view.height = std::max(1, int(std::lround(imageSize.height() * factor)));
    const QImage photo = plane->image.convertToFormat(QImage::Format_RGBA8888)
                             .scaled(view.width, view.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    view.shot = raster.shot;
    view.shot.rescaleViewportPx(QSize(view.width, view.height));
    const size_t n = size_t(view.width) * size_t(view.height);
    view.grey.resize(n);
    view.valid.resize(n);
    for (int row = 0; row < view.height; ++row) {
        const uchar *line = photo.constScanLine(row);
        for (int x = 0; x < view.width; ++x) {
            const uchar *rgba = line + 4 * x;
            const size_t i = size_t(view.height - 1 - row) * size_t(view.width) + size_t(x);
            view.grey[i] = uchar(0.3f * rgba[0] + 0.59f * rgba[1] + 0.11f * rgba[2]);
            view.valid[i] = rgba[3] > 0;
        }
    }
    return {};
}

// Another raster painting the mesh with its photo where it sees it: its view, the depth of the
// layers from its camera, for visibility, and how much its paint counts.
struct Projector
{
    const View *view = nullptr;
    std::vector<float> depth;
    double weight = 1;
};

// The visible layers rendered on the CPU through a camera into an image of 8-bit values: the
// rendering, or where projectors are given, their photos in grey levels, weighted, wherever
// one of them sees the surface, and the rendering only where none does.
class Renderer
{
public:
    Renderer(const Document &doc, Rendering rendering, int width, int height)
        : m_doc(doc), m_rendering(rendering), m_width(width), m_height(height),
          m_zbuf(size_t(width) * size_t(height)), m_image(m_zbuf.size()), m_projected(m_zbuf.size())
    {}

    const std::vector<uchar> &image() const { return m_image; }
    const std::vector<float> &depth() const { return m_zbuf; }
    // Of the pixels the layers cover, the share a projector painted
    double projectedShare() const
    {
        size_t covered = 0, projected = 0;
        for (size_t i = 0; i < m_zbuf.size(); ++i)
            if (m_zbuf[i] != std::numeric_limits<float>::max()) {
                ++covered;
                projected += m_projected[i];
            }
        return covered ? double(projected) / double(covered) : 0.0;
    }

    void render(const CameraShot &shot, const std::vector<Projector> &projectors = {})
    {
        std::fill(m_zbuf.begin(), m_zbuf.end(), std::numeric_limits<float>::max());
        std::fill(m_image.begin(), m_image.end(), uchar(0));
        std::fill(m_projected.begin(), m_projected.end(), uchar(0));
        const QMatrix4x4 view = shot.viewMatrix();
        for (int mi = 0; mi < m_doc.meshCount(); ++mi) {
            const Document::MeshEntry &layer = m_doc.mesh(mi);
            if (!layer.visible)
                continue;
            const VCGMesh &mesh = layer.mesh;
            // Normals as the OpenGL pipeline had them: through the layer's and the view's
            // matrices, interpolated, normalized at each pixel
            const QMatrix4x4 normalToEye = view * QMatrix4x4(layer.transform.normalMatrix());
            m_world.resize(mesh.vert.size());
            m_proj.resize(mesh.vert.size());
            m_depth.resize(mesh.vert.size());
            m_normal.resize(mesh.vert.size());
            for (size_t vi = 0; vi < mesh.vert.size(); ++vi) {
                m_world[vi] = transformPoint(layer.transform, mesh.vert[vi].cP());
                m_proj[vi] = shot.project(m_world[vi]);
                m_depth[vi] = shot.depth(m_world[vi]);
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
                                       [&](size_t i, float s0, float s1, float s2) {
                    // Perspective-correct weights, as the GPU interpolates
                    float b0 = s0 / m_depth[i0], b1 = s1 / m_depth[i1], b2 = s2 / m_depth[i2];
                    const float sum = b0 + b1 + b2;
                    b0 /= sum; b1 /= sum; b2 /= sum;
                    m_projected[i] = 0;
                    if (!projectors.empty()) {
                        const QVector3D p = b0 * m_world[i0] + b1 * m_world[i1] + b2 * m_world[i2];
                        double paint = 0, weight = 0;
                        for (const Projector &projector : projectors)
                            sample(projector, p, paint, weight);
                        if (weight > 0) {
                            m_image[i] = uchar(std::clamp(paint / weight, 0.0, 255.0) + 0.5);
                            m_projected[i] = 1;
                            return;
                        }
                    }
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
                    case Rendering::Silhouette: value = 1; break;
                    }
                    m_image[i] = uchar(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                });
            }
        }
    }

private:
    const Document &m_doc;
    Rendering m_rendering;
    int m_width, m_height;
    std::vector<float> m_zbuf;
    std::vector<uchar> m_image, m_projected;
    std::vector<QVector3D> m_world, m_normal;
    std::vector<QVector2D> m_proj;
    std::vector<float> m_depth;

    // Adds a projector's grey level at world point p, bilinear, if its camera sees p: in its
    // image, and no more than 1% behind the nearest surface there
    static void sample(const Projector &projector, const QVector3D &p, double &paint, double &weight)
    {
        const View &v = *projector.view;
        const QVector2D q = v.shot.project(p);
        const float x = q.x() - 0.5f, y = q.y() - 0.5f;
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        if (ix < 0 || iy < 0 || ix + 1 >= v.width || iy + 1 >= v.height)
            return;
        const size_t i = size_t(iy) * size_t(v.width) + size_t(ix);
        const float nearest = projector.depth[i + (x - ix > 0.5f ? 1 : 0) + (y - iy > 0.5f ? size_t(v.width) : 0)];
        if (v.shot.depth(p) > nearest * 1.01f)
            return;
        const size_t corners[4] = { i, i + 1, i + size_t(v.width), i + size_t(v.width) + 1 };
        for (size_t c : corners)
            if (!v.valid[c])
                return;
        const float fx = x - ix, fy = y - iy;
        const double grey = (1 - fy) * ((1 - fx) * v.grey[corners[0]] + fx * v.grey[corners[1]])
                          + fy * ((1 - fx) * v.grey[corners[2]] + fx * v.grey[corners[3]]);
        paint += projector.weight * grey;
        weight += projector.weight;
    }
};

// The mutual information, in bits, of a view's photo and an image of the same size over a joint
// histogram of 128 x 128 bins. The image's darkest bin, which holds its background, counts
// 1 / backgroundWeight as much as the others, as old MeshLab weighed it.
double mutualInformation(const View &view, const std::vector<uchar> &image, double backgroundWeight)
{
    std::vector<double> joint(size_t(kBins) * kBins, 0.0);
    const double background = backgroundWeight > 0 ? 1.0 / backgroundWeight : 0.0;
    for (size_t i = 0; i < view.grey.size(); ++i) {
        if (!view.valid[i])
            continue;
        const int a = view.grey[i] >> 1, b = image[i] >> 1;
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

CameraShot toCameraShot(const vcg::Shotd &shot) { return CameraShot::fromVcgShot(vcg::Shotf::Construct(shot)); }

// The raster's camera with where a working-resolution camera stands and looks, and its focal
// length: nothing else of the calibration changes.
CameraShot atRasterSize(const CameraShot &raster, const CameraShot &working)
{
    CameraShot::VcgShot result = raster.toVcgShot();
    const CameraShot::VcgShot fitted = working.toVcgShot();
    result.Extrinsics = fitted.Extrinsics;
    result.Intrinsics.FocalMm = fitted.Intrinsics.FocalMm;
    return CameraShot::fromVcgShot(result);
}

// Vertices of the visible layers, about `count` of them, in world space, and their box.
std::vector<vcg::Point3d> sampleLayers(const Document &doc, size_t count, vcg::Box3d &box)
{
    size_t vertices = 0;
    for (int mi = 0; mi < doc.meshCount(); ++mi)
        if (doc.mesh(mi).visible)
            vertices += doc.mesh(mi).mesh.vert.size();
    const size_t stride = std::max<size_t>(1, vertices / std::max<size_t>(1, count));
    std::vector<vcg::Point3d> samples;
    for (int mi = 0; mi < doc.meshCount(); ++mi) {
        const Document::MeshEntry &layer = doc.mesh(mi);
        if (!layer.visible)
            continue;
        for (size_t vi = 0; vi < layer.mesh.vert.size(); ++vi) {
            const QVector3D q = transformPoint(layer.transform, layer.mesh.vert[vi].cP());
            box.Add(vcg::Point3d(q.x(), q.y(), q.z()));
            if (vi % stride == 0)
                samples.emplace_back(q.x(), q.y(), q.z());
        }
    }
    return samples;
}

// The samples a camera sees, in front of it and inside its image.
std::vector<vcg::Point3d> seenBy(const CameraShot &shot, const std::vector<vcg::Point3d> &samples)
{
    std::vector<vcg::Point3d> seen;
    const QSize viewport = shot.viewportPx();
    for (const vcg::Point3d &s : samples) {
        const QVector3D q{float(s[0]), float(s[1]), float(s[2])};
        const QVector2D px = shot.project(q);
        if (shot.depth(q) > 0 && px.x() >= 0 && px.y() >= 0 && px.x() <= viewport.width() && px.y() <= viewport.height())
            seen.push_back(s);
    }
    return seen;
}

bool hasFaces(const Document &doc)
{
    for (int mi = 0; mi < doc.meshCount(); ++mi)
        if (doc.mesh(mi).visible && doc.mesh(mi).mesh.FN() > 0)
            return true;
    return false;
}

// How a camera is refined: the optimizer's settings, shared by both filters.
struct Settings
{
    Rendering rendering = Rendering::Combined;
    bool estimateFocal = false;
    int maxEvaluations = 300;
    double expectedError = 2, tolerance = 0.1, backgroundWeight = 2;
    double pairsWeight = 0;

    explicit Settings(const FilterParams &p, bool focalByDefault)
        : rendering(renderingParam(p)), estimateFocal(p.getBool(QStringLiteral("estimateFocal"), focalByDefault)),
          maxEvaluations(std::max(1, p.getInt(QStringLiteral("maxEvaluations"), 300))),
          expectedError(std::max(1e-3, p.getDouble(QStringLiteral("expectedError"), 2.0))),
          backgroundWeight(p.getDouble(QStringLiteral("backgroundWeight"), 2.0)),
          pairsWeight(std::clamp(p.getDouble(QStringLiteral("pairsWeight"), 0.0), 0.0, 1.0))
    {
        tolerance = std::clamp(p.getDouble(QStringLiteral("tolerance"), 0.1), 1e-4, expectedError);
    }
};

struct Refinement
{
    CameraShot shot;   // at the working resolution
    double before = 0, after = 0;           // mutual information, in bits
    double pairsBefore = 0, pairsAfter = 0; // mean distance of the point pairs, in working pixels
    int evaluations = 0;
    bool canceled = false;
};

// Point pairs for the combined objective: world points and the pixels that show them, in the
// working resolution's vcg pixels (y up).
struct Pairs
{
    std::vector<vcg::Point3d> points;
    std::vector<vcg::Point2d> pixels;

    double meanDistance(const vcg::Shotd &shot) const
    {
        double sum = 0;
        for (size_t i = 0; i < points.size(); ++i)
            sum += (shot.Project(points[i]) - pixels[i]).Norm();
        return points.empty() ? 0.0 : sum / double(points.size());
    }
};

// Maximizes the mutual information between a view's photo and the rendering through its camera,
// painted by the projectors, by NEWUOA. With point pairs it minimizes old MeshLab's combined
// score instead, (1 - w) (2 - MI) + w d, where d is the pairs' mean distance in working pixels
// and w the pairs' weight: a sum of bits and pixels, so w is a matter of taste rather than of
// units. It optimizes over vcg::ShotResection's parametrization about the
// pivot. It runs in rounds, each starting where the last ended with the scales measured again,
// until a round gains nothing or the evaluations run out. `progress` reports each evaluation
// and returns false to cancel: a canceled run then returns the worst value without rendering,
// so that the rest of NEWUOA's evaluations cost nothing.
template <class Progress>
Refinement refine(const Document &doc, const View &view, const std::vector<Projector> &projectors,
                  const std::vector<vcg::Point3d> &samples, const vcg::Point3d &pivot, const Settings &settings,
                  const Pairs &pairs, Progress &&progress)
{
    Renderer renderer(doc, settings.rendering, view.width, view.height);
    const auto information = [&](const CameraShot &shot) {
        renderer.render(shot, projectors);
        return mutualInformation(view, renderer.image(), settings.backgroundWeight);
    };
    const double w = pairs.points.empty() ? 0.0 : settings.pairsWeight;
    // The score minimized, with the information it was made of
    const auto score = [&](const vcg::Shotd &shot, double *miOut = nullptr) {
        const double mi = w < 1 ? information(toCameraShot(shot)) : 0.0;
        if (miOut)
            *miOut = mi;
        return (1 - w) * (2 - mi) + w * pairs.meanDistance(shot);
    };
    Refinement r;
    r.shot = view.shot;
    vcg::Shotd shot = vcg::Shotd::Construct(view.shot.toVcgShot());
    double best = score(shot, &r.before);
    r.after = r.before;
    r.pairsBefore = r.pairsAfter = pairs.meanDistance(shot);
    while (r.evaluations < settings.maxEvaluations) {
        const vcg::ShotResectiond::Parametrization parameters(shot, samples, pivot, settings.estimateFocal);
        auto objective = [&](int, double *x) {
            if (r.canceled)
                return 0.0;
            ++r.evaluations;
            if (!progress(r.evaluations, r.after)) {
                r.canceled = true;
                return 0.0;
            }
            return score(parameters.ToShot(x));
        };
        double x[7] = {0, 0, 0, 0, 0, 0, 0};
        min_newuoa(parameters.Size(), x, objective, settings.expectedError, settings.tolerance,
                   settings.maxEvaluations - r.evaluations);
        if (r.canceled)
            break;
        const vcg::Shotd fitted = parameters.ToShot(x);
        double mi = 0;
        const double value = score(fitted, &mi);
        if (!(value < best - 1e-6))
            break;
        shot = fitted;
        best = value;
        r.after = mi;
        r.pairsAfter = pairs.meanDistance(fitted);
        r.shot = toCameraShot(fitted);
    }
    return r;
}

// The root mean square distance, in pixels, between where two cameras project the samples.
double displacement(const CameraShot &a, const CameraShot &b, const std::vector<vcg::Point3d> &samples)
{
    double moved = 0;
    for (const vcg::Point3d &s : samples) {
        const QVector3D q{float(s[0]), float(s[1]), float(s[2])};
        moved += double((a.project(q) - b.project(q)).lengthSquared());
    }
    return samples.empty() ? 0.0 : std::sqrt(moved / double(samples.size()));
}

} // namespace

QString parseCorrespondences(const QString &text, const QSize &viewport, std::vector<vcg::Point3d> &points,
                             std::vector<vcg::Point2d> &pixels)
{
    const QJsonDocument json = QJsonDocument::fromJson(text.toUtf8());
    if (!json.isArray())
        return QObject::tr("Correspondences must be a JSON array of {\"point\": [x, y, z], \"pixel\": [u, v]}.");
    for (const QJsonValue &value : json.array()) {
        const QJsonArray point = value.toObject().value(QStringLiteral("point")).toArray();
        const QJsonArray pixel = value.toObject().value(QStringLiteral("pixel")).toArray();
        if (point.size() != 3 || pixel.size() != 2)
            return QObject::tr("Correspondence %1 needs a point of three numbers and a pixel of two.").arg(points.size() + 1);
        points.emplace_back(point[0].toDouble(), point[1].toDouble(), point[2].toDouble());
        pixels.emplace_back(pixel[0].toDouble(), viewport.height() - pixel[1].toDouble());
    }
    return {};
}

MeshFilterRunResult alignRasterByMutualInformation(const FilterParams &p, Document &doc)
{
    const int ri = doc.currentRasterIndex();
    if (ri < 0)
        return fail(QObject::tr("No current raster."));
    if (!hasFaces(doc))
        return fail(QObject::tr("No visible layer has faces to render."));
    View view;
    const QString error = makeView(doc, ri, std::max(16, p.getInt(QStringLiteral("resolution"), 800)), view);
    if (!error.isEmpty())
        return fail(error);
    const Document::RasterEntry &raster = doc.raster(ri);

    // What the camera is fitted against: the visible layers' vertices it sees, for scaling the
    // parameters, and the centre of their box, which the camera turns about
    vcg::Box3d box;
    const std::vector<vcg::Point3d> samples = seenBy(raster.shot, sampleLayers(doc, 4000, box));
    if (samples.size() < 3)
        return fail(QObject::tr("The raster's camera sees too little of the visible layers to align it."));

    const Settings settings(p, false);
    vcg::CallBackPos *cb = doc.progressCallback();
    QElapsedTimer timer;
    timer.start();
    // Point pairs, in the working resolution
    Pairs pairs;
    const QString pairsError = parseCorrespondences(p.getString(QStringLiteral("correspondences"), QStringLiteral("[]")),
                                                    raster.shot.viewportPx(), pairs.points, pairs.pixels);
    if (!pairsError.isEmpty())
        return fail(pairsError);
    const double sx = double(view.width) / raster.shot.viewportPx().width();
    const double sy = double(view.height) / raster.shot.viewportPx().height();
    for (vcg::Point2d &pixel : pairs.pixels)
        pixel = vcg::Point2d(pixel[0] * sx, pixel[1] * sy);

    const Refinement r = refine(doc, view, {}, samples, box.Center(), settings, pairs, [&](int evaluations, double best) {
        return !cb || cb(std::min(99, 100 * evaluations / settings.maxEvaluations),
                         qPrintable(QObject::tr("Mutual information: %1 bits").arg(best, 0, 'f', 4)));
    });
    if (r.canceled)
        return fail(QObject::tr("Operation canceled."));

    const CameraShot aligned = atRasterSize(raster.shot, r.shot);
    const double moved = displacement(aligned, raster.shot, samples);
    const QString message =
        QObject::tr("Aligned raster '%1' by mutual information at %2 x %3: %4 bits before, %5 after, "
                    "in %6 evaluations and %7 s; the camera moved its projections by %8 px.")
            .arg(raster.name).arg(view.width).arg(view.height)
            .arg(r.before, 0, 'f', 4).arg(r.after, 0, 'f', 4).arg(r.evaluations)
            .arg(timer.elapsed() / 1000.0, 0, 'f', 2).arg(moved, 0, 'f', 2);
    doc.setRasterShot(ri, aligned, message);
    MeshFilterRunResult result{true, true, {}};
    result.infoMessages = {message};
    result.outputValues.insert(QStringLiteral("informationBefore"), r.before);
    result.outputValues.insert(QStringLiteral("informationAfter"), r.after);
    result.outputValues.insert(QStringLiteral("evaluations"), r.evaluations);
    result.outputValues.insert(QStringLiteral("displacement"), moved);
    if (!pairs.points.empty()) {
        // The pairs' mean distance in the raster's own pixels
        const double toRaster = double(raster.shot.viewportPx().height()) / view.height;
        result.outputValues.insert(QStringLiteral("pairsBefore"), r.pairsBefore * toRaster);
        result.outputValues.insert(QStringLiteral("pairsAfter"), r.pairsAfter * toRaster);
        doc.writeLog(QObject::tr("The %1 point pairs were %2 px away on average before, %3 after.")
                         .arg(pairs.points.size()).arg(r.pairsBefore * toRaster, 0, 'f', 2)
                         .arg(r.pairsAfter * toRaster, 0, 'f', 2),
                     Document::LogSource::Application);
    }
    return result;
}

MeshFilterRunResult alignRastersGloballyByMutualInformation(const FilterParams &p, Document &doc)
{
    if (!hasFaces(doc))
        return fail(QObject::tr("No visible layer has faces to render."));
    // The visible rasters are refined; the hidden ones with a camera are anchors, already right,
    // that guide the others and never move
    const int resolution = std::max(16, p.getInt(QStringLiteral("resolution"), 800));
    std::vector<View> views;
    std::vector<bool> refined;
    for (int ri = 0; ri < doc.rasterCount(); ++ri) {
        const Document::RasterEntry &raster = doc.raster(ri);
        if (!raster.visible && (!raster.shot.isValid() || raster.shot.isOrthographic()))
            continue;
        View view;
        const QString error = makeView(doc, ri, resolution, view);
        if (!error.isEmpty())
            return fail(error);
        views.push_back(std::move(view));
        refined.push_back(!raster.visible);
    }
    const size_t n = views.size();
    if (std::find(refined.begin(), refined.end(), false) == refined.end())
        return fail(QObject::tr("No visible raster to refine."));
    if (n < 2)
        return fail(QObject::tr("Global refinement needs at least two rasters with a camera."));

    const Settings settings(p, true);
    const int maxIterations = std::max(1, p.getInt(QStringLiteral("maxIterations"), 5));
    const double threshold = p.getDouble(QStringLiteral("threshold"), 1.2);
    vcg::Box3d box;
    const std::vector<vcg::Point3d> allSamples = sampleLayers(doc, 5000, box);
    const vcg::Point3d pivot = box.Center();
    vcg::CallBackPos *cb = doc.progressCallback();
    bool canceled = false;
    const auto report = [&](int percent, const QString &what) {
        if (cb && !cb(std::clamp(percent, 0, 99), qPrintable(what)))
            canceled = true;
        return !canceled;
    };
    QElapsedTimer timer;
    timer.start();

    // Each raster's depth from its own camera, for the projectors, kept until its camera moves
    std::vector<std::vector<float>> depths(n);
    const auto depthOf = [&](size_t j) -> const std::vector<float> & {
        if (depths[j].empty()) {
            Renderer renderer(doc, settings.rendering, views[j].width, views[j].height);
            renderer.render(views[j].shot);
            depths[j] = renderer.depth();
        }
        return depths[j];
    };
    // The arc from raster j into raster i: w = MI(I_i, proj(I_j, I_i)) * OV(I_j, I_i), where OV is
    // the share of the layers seen from i that j paints. Arcs under 20% overlap are not made.
    struct Arc { size_t from; double weight; };
    std::vector<std::vector<Arc>> arcs(n);
    const auto arcWeight = [&](size_t i, size_t j, double *overlap) {
        Renderer renderer(doc, settings.rendering, views[i].width, views[i].height);
        renderer.render(views[i].shot, { Projector{ &views[j], depthOf(j), 1.0 } });
        *overlap = renderer.projectedShare();
        return mutualInformation(views[i], renderer.image(), settings.backgroundWeight) * *overlap;
    };
    size_t arcCount = 0;
    for (size_t i = 0; i < n && !canceled; ++i) {
        if (refined[i])
            continue;
        for (size_t j = 0; j < n && report(int(10 * (i * n + j) / (n * n)), QObject::tr("Measuring overlaps")); ++j) {
            double overlap = 0;
            const double weight = j == i ? 0 : arcWeight(i, j, &overlap);
            if (j != i && overlap > 0.2) {
                arcs[i].push_back({ j, weight });
                ++arcCount;
            }
        }
    }
    if (canceled)
        return fail(QObject::tr("Operation canceled."));
    doc.writeLog(QObject::tr("Global refinement: %1 arcs among %2 rasters.").arg(arcCount).arg(n),
                 Document::LogSource::Application);

    std::vector<CameraShot> original(n);
    for (size_t k = 0; k < n; ++k)
        original[k] = doc.raster(views[k].raster).shot;
    QVariantList variations;
    int evaluations = 0, iteration = 0;
    while (iteration < maxIterations && !canceled) {
        ++iteration;
        std::vector<CameraShot> before(n);
        for (size_t k = 0; k < n; ++k)
            before[k] = atRasterSize(original[k], views[k].shot);
        std::vector<bool> done = refined;
        for (size_t step = 0; std::find(done.begin(), done.end(), false) != done.end() && !canceled; ++step) {
            // The node with the most arcs from refined nodes; ties go to the one with more arcs
            size_t node = n;
            size_t bestRefined = 0;
            for (size_t i = 0; i < n; ++i) {
                if (done[i])
                    continue;
                size_t fromRefined = 0;
                for (const Arc &arc : arcs[i])
                    fromRefined += done[arc.from];
                if (node == n || fromRefined > bestRefined
                    || (fromRefined == bestRefined && arcs[i].size() > arcs[node].size())) {
                    node = i;
                    bestRefined = fromRefined;
                }
            }
            done[node] = true;
            if (arcs[node].empty())
                continue;   // nothing projects onto what it sees: nothing to guide it
            // Every image with an arc into the node paints the mesh, weighted by the arc
            std::vector<Projector> projectors;
            for (const Arc &arc : arcs[node])
                projectors.push_back({ &views[arc.from], depthOf(arc.from), arc.weight });
            const std::vector<vcg::Point3d> samples = seenBy(views[node].shot, allSamples);
            if (samples.size() < 3)
                continue;
            const Refinement r = refine(doc, views[node], projectors, samples, pivot, settings, Pairs(),
                                        [&](int, double) {
                return report(10 + int(90.0 * ((iteration - 1) * n + step) / (maxIterations * n)),
                              QObject::tr("Refining raster '%1', iteration %2")
                                  .arg(doc.raster(views[node].raster).name).arg(iteration));
            });
            evaluations += r.evaluations;
            if (r.canceled)
                break;
            views[node].shot = r.shot;
            depths[node].clear();
            // The arcs that involve the node are weighed again
            for (size_t i = 0; i < n; ++i)
                for (Arc &arc : arcs[i])
                    if (i == node || arc.from == node) {
                        double overlap = 0;
                        arc.weight = arcWeight(i, arc.from, &overlap);
                    }
        }
        if (canceled)
            break;
        // The variation: how far, on average, the samples' projections moved in each raster
        // that sees them, in its own pixels
        double sum = 0;
        size_t count = 0;
        for (size_t k = 0; k < n; ++k) {
            const CameraShot after = atRasterSize(original[k], views[k].shot);
            for (const vcg::Point3d &s : seenBy(before[k], allSamples)) {
                const QVector3D q{float(s[0]), float(s[1]), float(s[2])};
                sum += double((after.project(q) - before[k].project(q)).length());
                ++count;
            }
        }
        const double variation = count ? sum / double(count) : 0.0;
        variations.append(variation);
        doc.writeLog(QObject::tr("Global refinement, iteration %1: the projections moved %2 px on average.")
                         .arg(iteration).arg(variation, 0, 'f', 3),
                     Document::LogSource::Application);
        if (variation < threshold)
            break;
    }
    if (canceled)
        return fail(QObject::tr("Operation canceled."));

    for (size_t k = 0; k < n; ++k)
        if (!refined[k])
            doc.setRasterShot(views[k].raster, atRasterSize(original[k], views[k].shot));
    const QString message =
        QObject::tr("Refined %1 rasters globally by mutual information over %2 arcs: %3 iterations, "
                    "%4 renderings, %5 s.")
            .arg(std::count(refined.begin(), refined.end(), false)).arg(arcCount).arg(iteration)
            .arg(evaluations).arg(timer.elapsed() / 1000.0, 0, 'f', 1);
    doc.writeLog(message, Document::LogSource::Application);
    MeshFilterRunResult result{true, true, {}};
    result.infoMessages = {message};
    result.outputValues.insert(QStringLiteral("iterations"), iteration);
    result.outputValues.insert(QStringLiteral("variations"), variations);
    result.outputValues.insert(QStringLiteral("arcs"), int(arcCount));
    result.outputValues.insert(QStringLiteral("evaluations"), evaluations);
    return result;
}

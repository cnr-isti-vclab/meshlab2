#include "vertexdisplacementfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"

#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/math/perlin_noise.h>
#include <wrap/io_trimesh/io_mask.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace {

enum class FractalKind { Fbm, Standard, Heterogeneous, Hybrid, Ridged };

struct FractalParameters {
    float seed;
    int octaves;
    float lacunarity;
    float increment;
    float offset;
    float gain;
    float maxHeight;
    float scale;
    int smoothingSteps;
};

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

// The progress callback returns false when the user cancels. Only the noise loop may
// bail: it runs before any vertex is moved. Once displacement starts the mesh is being
// mutated in place, and there is no undo checkpoint yet.
MeshFilterRunResult interrupted()
{
    return { false, false, QObject::tr("Filter interrupted by user.") };
}

MeshFilterRunResult success(const QStringList &messages)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = messages;
    return result;
}

bool fractalKind(const QString &id, FractalKind &kind)
{
    if (id == QStringLiteral("displace_vertices_by_fractal_brownian_motion"))
        kind = FractalKind::Fbm;
    else if (id == QStringLiteral("displace_vertices_by_standard_multifractal_noise"))
        kind = FractalKind::Standard;
    else if (id == QStringLiteral("displace_vertices_by_heterogeneous_multifractal_noise"))
        kind = FractalKind::Heterogeneous;
    else if (id == QStringLiteral("displace_vertices_by_hybrid_multifractal_noise"))
        kind = FractalKind::Hybrid;
    else if (id == QStringLiteral("displace_vertices_by_ridged_multifractal_noise"))
        kind = FractalKind::Ridged;
    else
        return false;
    return true;
}

float fractalNoise(
    FractalKind kind,
    vcg::Point3f p,
    int octaves,
    float lacunarity,
    float offset,
    float gain,
    const std::array<float, 21> &weights)
{
    auto perlin = [&] { return float(vcg::math::Perlin::Noise(p.X(), p.Y(), p.Z())); };
    float noise = 0.0f;
    float signal = 0.0f;
    float weight = 0.0f;
    int firstOctave = 0;

    switch (kind) {
    case FractalKind::Fbm:
        break;
    case FractalKind::Standard:
        noise = 1.0f;
        break;
    case FractalKind::Heterogeneous:
        noise = (offset + perlin()) * weights[0];
        p *= lacunarity;
        firstOctave = 1;
        break;
    case FractalKind::Hybrid:
        noise = offset + perlin();
        weight = noise;
        p *= lacunarity;
        firstOctave = 1;
        break;
    case FractalKind::Ridged:
        signal = std::pow(offset - std::abs(perlin()), 2.0f);
        noise = signal;
        p *= lacunarity;
        firstOctave = 1;
        break;
    }

    for (int octave = firstOctave; octave < octaves; ++octave) {
        const float n = perlin();
        switch (kind) {
        case FractalKind::Fbm:
            noise += n * weights[octave];
            break;
        case FractalKind::Standard:
            noise *= offset + n * weights[octave];
            break;
        case FractalKind::Heterogeneous:
            noise += (offset + n) * weights[octave] * noise;
            break;
        case FractalKind::Hybrid:
            weight = std::min(weight, 1.0f);
            signal = (offset + n) * weights[octave];
            noise += weight * signal;
            weight *= signal;
            break;
        case FractalKind::Ridged:
            weight = std::clamp(signal * gain, 0.0f, 1.0f);
            signal = std::pow(offset - std::abs(n), 2.0f) * weight * weights[octave];
            noise += signal;
            break;
        }
        p *= lacunarity;
    }

    return noise;
}

MeshFilterRunResult displaceFractally(
    Document &doc,
    int meshIndex,
    FractalKind kind,
    const FilterParams &params)
{
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    const FractalParameters p {
        float(params.getDouble(QStringLiteral("seed"), 1.0)),
        params.getInt(QStringLiteral("octaves"), 8),
        float(params.getDouble(QStringLiteral("lacunarity"), 2.0)),
        float(params.getDouble(QStringLiteral("fractalIncrement"), 0.9)),
        float(params.getDouble(QStringLiteral("offset"), 0.9)),
        float(params.getDouble(QStringLiteral("gain"), 2.0)),
        float(params.getDouble(QStringLiteral("maxHeight"), 0.0)),
        float(params.getDouble(QStringLiteral("scale"), 1.0)),
        params.getInt(QStringLiteral("normalSmoothingSteps"), 5)
    };
    if (!std::isfinite(p.seed) || !std::isfinite(p.octaves)
        || !std::isfinite(p.lacunarity) || !std::isfinite(p.increment)
        || !std::isfinite(p.offset) || !std::isfinite(p.gain)
        || !std::isfinite(p.maxHeight) || !std::isfinite(p.scale)
        || p.octaves < 1 || p.octaves > 20 || p.lacunarity <= 0.0f
        || p.maxHeight < 0.0f || p.scale <= 0.0f || p.smoothingSteps < 0) {
        return fail(QObject::tr("Invalid fractal displacement parameters."));
    }

    const float diagonal = mesh.bbox.Diag();
    if (!(diagonal > 0.0f) || !std::isfinite(diagonal))
        return fail(QObject::tr("The current mesh must have a non-zero bounding box."));

    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
    if (p.smoothingSteps > 0)
        vcg::tri::Smooth<VCGMesh>::VertexNormalLaplacian(mesh, p.smoothingSteps, false);

    std::array<float, 21> weights {};
    float frequency = 1.0f;
    for (int i = 0; i <= p.octaves; ++i) {
        weights[size_t(i)] = std::pow(frequency, -p.increment);
        frequency *= p.lacunarity;
    }

    const float factor = 1.0f / p.scale;
    const float translation = p.seed / factor;
    const vcg::Point3f offset(translation, translation, translation);
    const vcg::Point3f center = mesh.bbox.Center();
    std::vector<float> noise(mesh.vert.size(), 0.0f);
    float maxNoise = -std::numeric_limits<float>::infinity();
    vcg::CallBackPos *cb = doc.progressCallback();

    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &vertex = mesh.vert[i];
        if (vertex.IsD())
            continue;
        const vcg::Point3f sample = (vertex.cP() + offset - center) * factor;
        noise[i] = fractalNoise(
            kind,
            sample,
            p.octaves,
            p.lacunarity,
            p.offset,
            p.gain,
            weights);
        maxNoise = std::max(maxNoise, noise[i]);
        if (cb && (i & 1023u) == 0u
            && !(*cb)(int(50 * i / std::max<size_t>(1, mesh.vert.size())),
                      "Computing fractal noise..."))
            return interrupted();
    }
    if (!std::isfinite(maxNoise) || std::abs(maxNoise) <= std::numeric_limits<float>::epsilon())
        return fail(QObject::tr("The fractal noise has a degenerate range for these parameters."));

    const float heightScale = p.maxHeight / maxNoise;
    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        VCGVertex &vertex = mesh.vert[i];
        if (vertex.IsD())
            continue;
        vertex.P() += vertex.cN() * (noise[i] * heightScale);
        // Progress only — deliberately not cancellable. This loop mutates vertex
        // positions and Document::runFilter takes no snapshot, so an early return
        // would leave the mesh partly displaced with nothing to undo. It is also
        // trivial next to the noise computation above, which is where cancelling pays.
        if (cb && (i & 1023u) == 0u)
            cb(50 + int(50 * i / std::max<size_t>(1, mesh.vert.size())), "Displacing vertices...");
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
    entry.ioMask |= vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Applied fractal displacement to '%1'").arg(entry.name));
    return success({
        QObject::tr("Displaced %1 vertices.").arg(mesh.VN()),
        QObject::tr("Maximum height: %1").arg(QString::number(p.maxHeight, 'g', 6))
    });
}

MeshFilterRunResult displaceRandomly(Document &doc, int meshIndex, const FilterParams &params)
{
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    const float maximum = float(params.getDouble(QStringLiteral("maxDisplacement"), 0.0));
    if (!std::isfinite(maximum) || maximum < 0.0f)
        return fail(QObject::tr("Maximum displacement must be finite and non-negative."));

    const bool updateNormals = params.getBool(QStringLiteral("recomputeNormals"), true);
    const RandomSeed seed = params.getRandomSeed();
    // Braces, not parens: the parenthesised form is a most-vexing-parse and would
    // declare a function rather than construct the generator.
    std::mt19937 rng{std::mt19937::result_type(seed.value)};
    std::uniform_real_distribution<float> distribution(-maximum, maximum);
    for (VCGVertex &vertex : mesh.vert) {
        if (!vertex.IsD())
            vertex.P() += vcg::Point3f(distribution(rng), distribution(rng), distribution(rng));
    }

    if (updateNormals) {
        // ...PerFaceNormalized: the plain PerVertexNormalizedPerFace leaves face
        // normals area-weighted (length 2*area), and everything downstream assumes
        // unit face normals. Matches the fractal path above.
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
        entry.ioMask |= vcg::tri::io::Mask::IOM_VERTNORMAL | vcg::tri::io::Mask::IOM_FACENORMAL;
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    entry.ioMask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Randomly displaced vertices of '%1'").arg(entry.name));
    return success({
        QObject::tr("Displaced %1 vertices.").arg(mesh.VN()),
        seed.message()
    });
}

} // namespace

QString VertexDisplacementFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.vertex_displacement");
}

QString VertexDisplacementFilterPlugin::name() const
{
    return QObject::tr("Vertex Displacement Filters");
}

MeshFilterRunResult VertexDisplacementFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(meshIndex).mesh.VN() <= 0)
        return fail(QObject::tr("The current mesh has no vertices."));

    if (filterId == QStringLiteral("displace_vertices_randomly"))
        return displaceRandomly(doc, meshIndex, params);

    FractalKind kind;
    if (!fractalKind(filterId, kind))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    return displaceFractally(doc, meshIndex, kind, params);
}

void registerVertexDisplacementFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<VertexDisplacementFilterPlugin>());
}

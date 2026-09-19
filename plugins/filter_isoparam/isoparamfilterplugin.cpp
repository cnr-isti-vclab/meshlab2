#include <map>
#include "isoparamfilterplugin.h"

#include "document.h"
#include "filterparam.h"
#include "layerdata.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

// The reference implementation narrates itself with ~120 printf/fprintf calls -- a few
// hundred lines per run. Intercepting them here, before the headers are included, routes
// the whole narration into the document log without touching a line of the vendored code.
// These are header-inline, so every call in them compiles in this translation unit.
namespace isoparam_capture {

std::mutex &mutex();
void append(const char *text);

inline int captured_printf(const char *format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    append(buffer);
    return n;
}

inline int captured_fprintf(std::FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    // Only the console is narration. SaveBaseDomain writes the abstract domain itself with
    // fprintf, and capturing that would quietly produce an empty file.
    if (stream != stdout && stream != stderr) {
        const int n = std::vfprintf(stream, format, args);
        va_end(args);
        return n;
    }
    char buffer[1024];
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    append(buffer);
    return n;
}

} // namespace isoparam_capture

#define printf isoparam_capture::captured_printf
#define fprintf isoparam_capture::captured_fprintf

// Order matters: the rest of the vendored tree assumes iso_parametrization.h has already
// been seen and does not include it itself.
#include "upstream/iso_parametrization.h"
#include "upstream/parametrizator.h"
#include "upstream/diam_parametrization.h"
#include "upstream/diamond_sampler.h"
#include "upstream/iso_transfer.h"

#undef printf
#undef fprintf

// After the vendored headers: it builds on IsoParametrization and DiamondParametrizator.
#include "atlaslayout.h"

#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/topology.h>

namespace isoparam_capture {

namespace {
std::mutex g_mutex;
QStringList *g_sink = nullptr;
QString g_partial;
}

std::mutex &mutex() { return g_mutex; }

void append(const char *text)
{
    if (!g_sink || !text)
        return;
    // The reference code prints in fragments and newlines land where they land, so lines
    // are reassembled here rather than one log entry per printf.
    g_partial += QString::fromLocal8Bit(text);
    int nl;
    while ((nl = int(g_partial.indexOf(QLatin1Char('\n')))) >= 0) {
        const QString line = g_partial.left(nl).trimmed();
        g_partial.remove(0, nl + 1);
        if (!line.isEmpty())
            *g_sink << line;
    }
}

// Redirects the vendored narration into `sink` for its lifetime.
class Scope
{
public:
    explicit Scope(QStringList &sink) { g_sink = &sink; g_partial.clear(); }
    ~Scope()
    {
        if (g_sink && !g_partial.trimmed().isEmpty())
            *g_sink << g_partial.trimmed();
        g_sink = nullptr;
        g_partial.clear();
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};

} // namespace isoparam_capture

namespace {

constexpr QLatin1StringView kFilterAbstractDomain("parametrize_by_abstract_domain");
constexpr QLatin1StringView kFilterRemesh("remesh_by_abstract_domain");
constexpr QLatin1StringView kFilterAtlas("create_atlased_mesh_from_abstract_domain");
constexpr QLatin1StringView kFilterTransfer("transfer_abstract_domain_to_another_layer");
constexpr QLatin1StringView kFilterMeasure("measure_abstract_domain");
const QString kDomainKey = QStringLiteral("meshlab2.filter.isoparam/abstract_domain");

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.errorMessage = message;
    return result;
}

// The abstract domain and the parametrization built on it. Immutable once installed, as
// LayerData requires: the later filters of this family read it and never edit it.
class AbstractDomainData : public LayerData
{
public:
    AbstractDomainData(std::unique_ptr<AbstractMesh> abstractMesh,
                       std::unique_ptr<ParamMesh> paramMesh,
                       std::unique_ptr<IsoParametrization> iso,
                       int domainFaces, float stretch, float distortion)
        : m_abstractMesh(std::move(abstractMesh))
        , m_paramMesh(std::move(paramMesh))
        , m_iso(std::move(iso))
        , m_domainFaces(domainFaces)
        , m_stretch(stretch)
        , m_distortion(distortion)
    {
    }

    QString describe() const override
    {
        return QObject::tr("abstract domain, %1 faces, stretch %2, distortion %3%")
            .arg(m_domainFaces)
            .arg(double(m_stretch), 0, 'f', 4)
            .arg(double(m_distortion), 0, 'f', 2);
    }

    // The domain is a parametrization *of this geometry*; move the vertices and it means
    // nothing. Core drops it for us when the layer's geometry changes.
    bool survivesGeometryChange() const override { return false; }

    std::size_t approximateBytes() const override
    {
        // Dominated by the two meshes the parametrization holds.
        return m_iso ? std::size_t(m_domainFaces) * 512u : 0u;
    }

    IsoParametrization *parametrization() const { return m_iso.get(); }
    const AbstractMesh *abstractMesh() const { return m_abstractMesh.get(); }
    // What the parametrizator reported when it stopped. Read by "Measure Abstract Domain";
    // describe() above shows the same three in the layer panel.
    int domainFaces() const { return m_domainFaces; }
    float stretch() const { return m_stretch; }
    float distortion() const { return m_distortion; }

private:
    // IsoParametrization only holds pointers to these two -- MeshLab allocates them with a
    // bare new and never frees them. Owning them here, and declaring them before m_iso so
    // they outlive it, makes the whole thing a value with a lifetime.
    std::unique_ptr<AbstractMesh> m_abstractMesh;
    std::unique_ptr<ParamMesh> m_paramMesh;
    std::unique_ptr<IsoParametrization> m_iso;
    int m_domainFaces;
    float m_stretch;
    float m_distortion;
};

QString describeReturnCode(IsoParametrizator::ReturnCode code)
{
    switch (code) {
    case IsoParametrizator::MultiComponent:
        return QObject::tr("the mesh has more than one connected component");
    case IsoParametrizator::NonSizeCons:
        return QObject::tr("the mesh is too small for the requested abstract domain");
    case IsoParametrizator::NonManifoldE:
        return QObject::tr("the mesh has non-manifold edges");
    case IsoParametrizator::NonManifoldV:
        return QObject::tr("the mesh has non-manifold vertices");
    case IsoParametrizator::NonWatertigh:
        return QObject::tr("the mesh is not watertight");
    case IsoParametrizator::FailParam:
        return QObject::tr("the parametrization did not converge");
    case IsoParametrizator::Done:
        break;
    }
    return QObject::tr("unknown failure");
}

} // namespace

// Fetches the domain a previous run of the main filter left on a layer, with the message
// the user needs when it is not there -- which is the normal case for anyone who reaches
// one of these three filters first.
const AbstractDomainData *domainOf(const Document &doc, int meshIndex, QString &error)
{
    const LayerDataPtr data = doc.layerData(meshIndex, kDomainKey);
    if (!data) {
        error = QObject::tr(
            "'%1' has no abstract domain. Run \"Parametrize by Abstract Domain\" on it first; "
            "note that the domain is dropped whenever the layer's geometry changes.")
            .arg(meshIndex >= 0 && meshIndex < doc.meshCount() ? doc.mesh(meshIndex).name
                                                               : QString());
        return nullptr;
    }
    return static_cast<const AbstractDomainData *>(data.get());
}

// Uniform remeshing: every domain triangle is subdivided recursively, so the sampling
// rate is a resolution multiplier rather than a target count.
MeshFilterRunResult runRemesh(const FilterParams &params, Document &doc, int index)
{
    QString error;
    const AbstractDomainData *domain = domainOf(doc, index, error);
    if (!domain)
        return fail(error);

    const int samplingRate = params.getInt(QStringLiteral("samplingRate"), 10);
    if (samplingRate < 2)
        return fail(QObject::tr("The sampling rate must be at least 2."));

    doc.beginFilterProgress(QObject::tr("Remesh by Abstract Domain"));
    QStringList narration;
    VCGMesh remeshed;
    bool done = false;
    int diamonds = 0, inFace = 0, inEdge = 0, inStar = 0, merged = 0;
    {
        const std::lock_guard<std::mutex> guard(isoparam_capture::mutex());
        const isoparam_capture::Scope capture(narration);
        DiamSampler sampler;
        sampler.Init(domain->parametrization());
        done = sampler.SamplePos(samplingRate);
        if (done) {
            sampler.GetMesh<VCGMesh>(remeshed);
            sampler.getResData(diamonds, inFace, inEdge, inStar, merged);
        }
    }
    for (const QString &line : narration)
        doc.writeLog(QStringLiteral("[isoparam] %1").arg(line),
                     Document::LogSource::VCG, Document::LogLevel::Debug);

    if (!done || remeshed.FN() <= 0) {
        const QString message = QObject::tr("Remeshing over the abstract domain failed.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(remeshed);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(remeshed);
    const int newIndex = doc.addMesh(
        remeshed,
        {},
        vcg::tri::io::Mask::IOM_VERTCOORD | vcg::tri::io::Mask::IOM_FACEINDEX
            | vcg::tri::io::Mask::IOM_VERTNORMAL | vcg::tri::io::Mask::IOM_FACENORMAL);
    if (newIndex < 0) {
        const QString message = QObject::tr("Could not add the remeshed layer.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.setMeshTransform(newIndex, doc.mesh(index).transform);

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices.push_back(newIndex);
    result.infoMessages
        << QObject::tr("Remeshed to %1 vertices and %2 faces.")
               .arg(remeshed.VN()).arg(remeshed.FN())
        << QObject::tr("Placed through %1 face domains, %2 half-diamonds, %3 half-stars.")
               .arg(inFace).arg(inEdge).arg(inStar)
        << QObject::tr("Merged %1 vertices.").arg(merged);
    doc.finishFilterProgress(true, QObject::tr("Remeshed over the abstract domain."));
    return result;
}

// The domain's diamonds become charts -- singly, or six at a time around a regular domain
// vertex -- and one of the vcglib packers lays them out in a single atlas. The
// parametrization arrives as per-wedge UVs on a new layer.
MeshFilterRunResult runAtlasedMesh(const FilterParams &params, Document &doc, int index)
{
    QString error;
    const AbstractDomainData *domain = domainOf(doc, index, error);
    if (!domain)
        return fail(error);

    const QString shapeId = params.getEnum(QStringLiteral("chartShape"));
    atlaslayout::Params layout;
    layout.shape = shapeId == QStringLiteral("hexagon")  ? atlaslayout::ChartShape::Hexagon
                 : shapeId == QStringLiteral("star")     ? atlaslayout::ChartShape::Star
                 : shapeId == QStringLiteral("halfstar") ? atlaslayout::ChartShape::HalfStar
                 : shapeId == QStringLiteral("rhombus")  ? atlaslayout::ChartShape::Rhombus
                                                         : atlaslayout::ChartShape::Square;
    layout.mergeIrregularStars = params.getBool(QStringLiteral("mergeIrregularStars"), false);
    layout.border = float(params.getDouble(QStringLiteral("borderSize"), 0.1));
    layout.packing.algorithm = params.getEnum(QStringLiteral("packingAlgorithm"));
    layout.packing.textureSize = params.getInt(QStringLiteral("textureSize"), 1024);
    layout.packing.gutterWidth = params.getInt(QStringLiteral("gutterWidth"), 4);
    layout.packing.rotationNum = params.getInt(QStringLiteral("rotationNum"), 4);
    layout.packing.permutations = params.getBool(QStringLiteral("permutations"), false);
    const RandomSeed seed = params.getRandomSeed();
    layout.packing.randomSeed = seed.value;

    doc.beginFilterProgress(QObject::tr("Create Atlased Mesh from Abstract Domain"));
    QStringList narration;
    atlaslayout::Stats stats;
    VCGMesh atlased;
    atlased.face.EnableWedgeTexCoord();
    bool laidOut = false;
    {
        const std::lock_guard<std::mutex> guard(isoparam_capture::mutex());
        const isoparam_capture::Scope capture(narration);
        laidOut = atlaslayout::build(*domain->parametrization(), layout, atlased, stats);
    }
    for (const QString &line : narration)
        doc.writeLog(QStringLiteral("[isoparam] %1").arg(line),
                     Document::LogSource::VCG, Document::LogLevel::Debug);

    if (!laidOut) {
        const QString message = QObject::tr("The packer could not lay the charts out.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    if (atlased.FN() <= 0) {
        const QString message = QObject::tr("The atlased mesh came out empty.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(atlased);
    vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(atlased);
    const int newIndex = doc.addMesh(
        atlased,
        {},
        vcg::tri::io::Mask::IOM_VERTCOORD | vcg::tri::io::Mask::IOM_FACEINDEX
            | vcg::tri::io::Mask::IOM_WEDGTEXCOORD | vcg::tri::io::Mask::IOM_FACENORMAL);
    if (newIndex < 0) {
        const QString message = QObject::tr("Could not add the atlased layer.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.setMeshTransform(newIndex, doc.mesh(index).transform);

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices.push_back(newIndex);
    result.infoMessages
        << QObject::tr("Atlased mesh: %1 vertices, %2 faces, per-wedge UVs.")
               .arg(atlased.VN()).arg(atlased.FN())
        << (layout.shape == atlaslayout::ChartShape::HalfStar
                ? QObject::tr("%1 charts, one per domain vertex. Atlas covered: %2%.")
                      .arg(stats.mergedCharts).arg(100.0 * stats.coverage, 0, 'f', 1)
                : QObject::tr("%1 charts: %2 merged over %3 diamonds, %4 single diamonds. "
                              "Atlas covered: %5%.")
                      .arg(stats.mergedCharts + stats.diamondCharts)
                      .arg(stats.mergedCharts).arg(stats.mergedDiamonds)
                      .arg(stats.diamondCharts)
                      .arg(100.0 * stats.coverage, 0, 'f', 1));
    if (stats.irregularStarCharts > 0)
        result.infoMessages << QObject::tr(
            "%1 of the star charts sit on an irregular domain vertex, so they came out as "
            "pentagons or heptagons rather than hexagons.").arg(stats.irregularStarCharts);
    if (stats.unplacedCharts > 0)
        result.infoMessages << QObject::tr(
            "%1 charts did not fit and were collapsed; raise the atlas size or pick a "
            "packer that scales to fit.").arg(stats.unplacedCharts);
    result.infoMessages << seed.message();
    doc.finishFilterProgress(true, QObject::tr("Built the atlased mesh."));
    return result;
}

// Reports what the abstract domain on a layer actually is. Nothing here modifies anything;
// the point is that the domain is otherwise opaque -- it lives in layer data, the layer
// panel shows three numbers, and the three filters that consume it each expose only the
// part they need. The measures are the ones a user has to know to choose between the atlas
// chart shapes, plus the paper's own structural invariants, checked rather than assumed.
//
// Section 4 of Pietroni, Tarini and Cignoni (IEEE TVCG 2010) is the reference: the domain
// is N unit-sided equilateral sub-domains, a position in it is a triple (i, alpha, beta)
// with i in [0..N-1] and (alpha, beta) the first two barycentric coordinates, the
// connectivity makes it "2-manifold, closed, and well oriented", Theta is a bijection, and
// N "typically ranges between a minimum of 4 and a maximum of a few hundreds".
MeshFilterRunResult runMeasureDomain(const FilterParams &params, Document &doc, int index)
{
    Q_UNUSED(params);
    QString error;
    const AbstractDomainData *domain = domainOf(doc, index, error);
    if (!domain)
        return fail(error);

    const AbstractMesh *abs = domain->abstractMesh();
    IsoParametrization *iso = domain->parametrization();
    if (!abs || !iso || !iso->ParaMesh())
        return fail(QObject::tr("The abstract domain on this layer is incomplete."));
    const ParamMesh &para = *iso->ParaMesh();

    // ---- domain structure ----
    int subDomains = 0;
    for (const auto &f : abs->face)
        if (!f.IsD())
            ++subDomains;
    int domainVertices = 0;
    for (const auto &v : abs->vert)
        if (!v.IsD())
            ++domainVertices;

    // Every side of every sub-domain is shared with exactly one other (paper, sec. 4), so
    // a closed domain has 3F/2 edges and no border. Count the borders rather than trusting
    // it: a border here means the invariant is broken, and the half-diamond partition -- one
    // domain per edge -- is what the atlas builds its charts from.
    int borderEdges = 0;
    std::vector<int> valence(abs->vert.size(), 0);
    for (const auto &f : abs->face) {
        if (f.IsD())
            continue;
        for (int j = 0; j < 3; ++j) {
            if (vcg::face::IsBorder(f, j))
                ++borderEdges;
            valence[std::size_t(vcg::tri::Index(*const_cast<AbstractMesh *>(abs), f.cV(j)))] += 1;
        }
    }
    const int domainEdges = (3 * subDomains + borderEdges) / 2;
    // V - E + F = 2 - 2g on a closed orientable surface. Reported as the domain's own
    // genus: the method puts no restriction on it, and it should match the surface's.
    const int euler = domainVertices - domainEdges + subDomains;
    const bool closed = borderEdges == 0;

    std::map<int, int> valenceHistogram;
    for (std::size_t i = 0; i < abs->vert.size(); ++i)
        if (!abs->vert[i].IsD())
            ++valenceHistogram[valence[i]];

    // ---- how the mapping sits in the domain ----
    std::vector<int> verticesPerSubDomain(std::size_t(std::max(1, subDomains)), 0);
    int outOfRange = 0;
    int paramVertices = 0;
    for (const auto &v : para.vert) {
        if (v.IsD())
            continue;
        ++paramVertices;
        const int I = v.cT().N();
        const vcg::Point2f bary = v.cT().P();
        // The (i, alpha, beta) triple: i names a sub-domain, and (alpha, beta) must land
        // inside the unit triangle. A tolerance because these are optimized floats.
        const float tol = 1e-4f;
        if (I < 0 || I >= subDomains || bary.X() < -tol || bary.Y() < -tol
            || bary.X() + bary.Y() > 1.0f + tol) {
            ++outOfRange;
            continue;
        }
        ++verticesPerSubDomain[std::size_t(I)];
    }

    int paramFaces = 0;
    int spanningFaces = 0;
    for (const auto &f : para.face) {
        if (f.IsD())
            continue;
        ++paramFaces;
        // A face whose corners name different sub-domains straddles them, which the paper
        // allows: an interpolation domain is what makes such a face expressible at all.
        if (f.cV(0)->cT().N() != f.cV(1)->cT().N() || f.cV(1)->cT().N() != f.cV(2)->cT().N())
            ++spanningFaces;
    }

    int emptySubDomains = 0;
    int minPer = paramVertices;
    int maxPer = 0;
    for (int i = 0; i < subDomains; ++i) {
        const int n = verticesPerSubDomain[std::size_t(i)];
        if (n == 0)
            ++emptySubDomains;
        minPer = std::min(minPer, n);
        maxPer = std::max(maxPer, n);
    }
    if (subDomains == 0)
        minPer = 0;
    const double meanPer =
        subDomains > 0 ? double(paramVertices - outOfRange) / double(subDomains) : 0.0;

    const Document::MeshEntry &entry = doc.mesh(index);
    const double facesPerSubDomain =
        subDomains > 0 ? double(entry.mesh.FN()) / double(subDomains) : 0.0;

    // ---- the report ----
    const auto yesNo = [](bool b) { return b ? QObject::tr("yes") : QObject::tr("no"); };
    QStringList info;
    info << QObject::tr("Layer: %1").arg(entry.name);
    info << QObject::tr("Sub-domains (equilateral triangles): %1").arg(subDomains);
    info << QObject::tr("Domain V: %1  E: %2  F: %3").arg(domainVertices).arg(domainEdges).arg(subDomains);
    info << QObject::tr("Closed and 2-manifold: %1%2")
                .arg(yesNo(closed))
                .arg(closed ? QString() : QObject::tr(" (%1 border side(s))").arg(borderEdges));
    if (closed && euler % 2 == 0)
        info << QObject::tr("Euler characteristic: %1 (genus %2)").arg(euler).arg((2 - euler) / 2);
    else
        info << QObject::tr("Euler characteristic: %1").arg(euler);

    QStringList valenceParts;
    for (const auto &bucket : valenceHistogram) {
        valenceParts << QObject::tr("%1x valence %2").arg(bucket.second).arg(bucket.first);
    }
    info << QObject::tr("Domain vertex valences: %1").arg(valenceParts.join(QStringLiteral(", ")));
    info << QObject::tr("Irregular domain vertices (valence != 6): %1")
                .arg(domainVertices - valenceHistogram[6]);

    // The three partitions of sec. 4.2, which is what the atlas filter's Chart shape picks
    // between. Quoting the counts saves the user running it to find out.
    info << QObject::tr("Charts the atlas would build -- Square/Rhombus: %1 (one per "
                        "half-diamond, i.e. per domain edge); Polygon: %2 (one per "
                        "half-star, i.e. per domain vertex); Hexagon and Star merge these "
                        "and end up with fewer.")
                .arg(domainEdges)
                .arg(domainVertices);

    info << QObject::tr("Parametrized mesh: %1 vertices, %2 faces (layer has %3 faces)")
                .arg(paramVertices).arg(paramFaces).arg(entry.mesh.FN());
    info << QObject::tr("Layer faces per sub-domain: %1")
                .arg(facesPerSubDomain, 0, 'f', 1);
    info << QObject::tr("Param vertices per sub-domain: min %1, mean %2, max %3")
                .arg(minPer).arg(meanPer, 0, 'f', 1).arg(maxPer);
    info << QObject::tr("Faces straddling two or more sub-domains: %1 of %2")
                .arg(spanningFaces).arg(paramFaces);

    info << QObject::tr("Stretch efficiency: %1 (1.0 is isometric)")
                .arg(double(domain->stretch()), 0, 'f', 4);
    info << QObject::tr("Area distortion: %1%").arg(double(domain->distortion()), 0, 'f', 2);

    // The checks. A domain that fails one of these is not usable, and until now the only
    // symptom was a downstream filter behaving oddly.
    QStringList problems;
    if (subDomains < 4)
        problems << QObject::tr("only %1 sub-domains; the method's minimum is 4").arg(subDomains);
    if (!closed)
        problems << QObject::tr("%1 border side(s): the domain is not closed").arg(borderEdges);
    if (emptySubDomains > 0)
        problems << QObject::tr("%1 sub-domain(s) have no parametrized vertex, so the "
                                "mapping cannot cover them").arg(emptySubDomains);
    if (outOfRange > 0)
        problems << QObject::tr("%1 vertex/vertices name a sub-domain outside [0,%2) or sit "
                                "outside their triangle").arg(outOfRange).arg(subDomains);
    if (problems.isEmpty())
        info << QObject::tr("Structural checks: all passed.");
    else
        info << QObject::tr("Structural checks FAILED: %1.").arg(problems.join(QStringLiteral("; ")));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = false;
    result.infoMessages = info;
    result.outputValues[QStringLiteral("sub_domains")] = subDomains;
    result.outputValues[QStringLiteral("domain_vertices")] = domainVertices;
    result.outputValues[QStringLiteral("domain_edges")] = domainEdges;
    result.outputValues[QStringLiteral("domain_border_sides")] = borderEdges;
    result.outputValues[QStringLiteral("domain_euler_characteristic")] = euler;
    result.outputValues[QStringLiteral("irregular_domain_vertices")] =
        domainVertices - valenceHistogram[6];
    result.outputValues[QStringLiteral("param_vertices")] = paramVertices;
    result.outputValues[QStringLiteral("param_faces")] = paramFaces;
    result.outputValues[QStringLiteral("faces_straddling_sub_domains")] = spanningFaces;
    result.outputValues[QStringLiteral("layer_faces_per_sub_domain")] = facesPerSubDomain;
    result.outputValues[QStringLiteral("param_vertices_per_sub_domain_min")] = minPer;
    result.outputValues[QStringLiteral("param_vertices_per_sub_domain_max")] = maxPer;
    result.outputValues[QStringLiteral("empty_sub_domains")] = emptySubDomains;
    result.outputValues[QStringLiteral("vertices_outside_their_sub_domain")] = outOfRange;
    result.outputValues[QStringLiteral("stretch_efficiency")] = double(domain->stretch());
    result.outputValues[QStringLiteral("area_distortion_percent")] = double(domain->distortion());
    result.outputValues[QStringLiteral("structurally_valid")] = problems.isEmpty();
    return result;
}

// Carries a domain onto a second, similar layer by closest point.
MeshFilterRunResult runTransfer(const FilterParams &params, Document &doc)
{
    const int sourceIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    const int targetIndex = params.getMesh(QStringLiteral("targetMesh"), doc.currentMeshIndex());
    if (sourceIndex < 0 || sourceIndex >= doc.meshCount()
        || targetIndex < 0 || targetIndex >= doc.meshCount())
        return fail(QObject::tr("Both a source and a target layer are needed."));
    if (sourceIndex == targetIndex)
        return fail(QObject::tr("The source and target layers must be different."));

    QString error;
    const AbstractDomainData *domain = domainOf(doc, sourceIndex, error);
    if (!domain)
        return fail(error);

    Document::MeshEntry &target = doc.mesh(targetIndex);
    if (target.mesh.FN() <= 0)
        return fail(QObject::tr("The target layer needs faces."));

    doc.beginFilterProgress(QObject::tr("Transfer Abstract Domain to Another Layer"));

    VCGMeshFFAdjScope ffAdj(target.mesh);
    VCGMeshMarkScope faceMark(target.mesh);
    target.mesh.vert.EnableTexCoord();
    target.mesh.face.EnableWedgeTexCoord();
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(target.mesh);

    // MeshLab moves the domain: it transfers, clears the source's parametrization and
    // re-points the same meshes at the target, which leaves the source without one. We
    // cannot do that -- an installed domain is immutable and undo snapshots may share it --
    // so the target gets its own copy of the abstract mesh and a param mesh built from
    // itself. The source keeps its domain, which is the better behaviour anyway.
    QStringList narration;
    auto abstractCopy = std::make_unique<AbstractMesh>();
    auto paramCopy = std::make_unique<ParamMesh>();
    auto iso = std::make_unique<IsoParametrization>();
    bool ok = false;
    {
        const std::lock_guard<std::mutex> guard(isoparam_capture::mutex());
        const isoparam_capture::Scope capture(narration);
        IsoTransfer transfer;
        transfer.Transfer<VCGMesh>(*domain->parametrization(), target.mesh);

        vcg::tri::Append<AbstractMesh, AbstractMesh>::MeshCopyConst(
            *abstractCopy, *domain->abstractMesh());
        iso->AbsMesh() = abstractCopy.get();
        ok = iso->SetParamMesh<VCGMesh>(&target.mesh, paramCopy.get());
    }
    for (const QString &line : narration)
        doc.writeLog(QStringLiteral("[isoparam] %1").arg(line),
                     Document::LogSource::VCG, Document::LogLevel::Debug);

    if (!ok) {
        const QString message =
            QObject::tr("The transferred domain could not be initialized on '%1'. The two "
                        "layers must be similar in shape and already aligned.")
                .arg(target.name);
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    target.ioMask |= vcg::tri::io::Mask::IOM_VERTTEXCOORD
        | vcg::tri::io::Mask::IOM_WEDGTEXCOORD;
    doc.markMeshGeometryChanged(
        targetIndex, QObject::tr("Transferred an abstract domain onto '%1'.").arg(target.name));
    doc.setLayerData(targetIndex, kDomainKey,
                     std::make_shared<AbstractDomainData>(
                         std::move(abstractCopy), std::move(paramCopy), std::move(iso),
                         int(domain->parametrization()->AbsMesh()->fn), 0.0f, 0.0f));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Transferred the abstract domain from '%1' to '%2'.")
               .arg(doc.mesh(sourceIndex).name, target.name)
        << QObject::tr("'%1' keeps its own domain.").arg(doc.mesh(sourceIndex).name);
    doc.finishFilterProgress(true, QObject::tr("Abstract domain transferred."));
    return result;
}

QString IsoParamFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.isoparam");
}

QString IsoParamFilterPlugin::name() const
{
    return QObject::tr("Isoparametrization Filters");
}

MeshFilterRunResult IsoParamFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));

    if (filterId == QString::fromLatin1(kFilterRemesh))
        return runRemesh(params, doc, index);
    if (filterId == QString::fromLatin1(kFilterAtlas))
        return runAtlasedMesh(params, doc, index);
    if (filterId == QString::fromLatin1(kFilterMeasure))
        return runMeasureDomain(params, doc, index);
    if (filterId == QString::fromLatin1(kFilterTransfer))
        return runTransfer(params, doc);
    if (filterId != QString::fromLatin1(kFilterAbstractDomain))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    Document::MeshEntry &entry = doc.mesh(index);
    if (entry.mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const int minFaces = params.getInt(QStringLiteral("minDomainFaces"), 150);
    const int maxFaces = params.getInt(QStringLiteral("maxDomainFaces"), 200);
    if (maxFaces < minFaces) {
        return fail(QObject::tr("The largest abstract domain (%1 faces) must not be "
                                "smaller than the smallest (%2).")
                        .arg(maxFaces).arg(minFaces));
    }
    const int accuracy = params.getInt(QStringLiteral("convergenceAccuracy"), 1);
    const bool doubleStep = params.getBool(QStringLiteral("doubleStep"), true);

    // What the algorithm reads and writes on the mesh it is given.
    VCGMeshFFAdjScope ffAdj(entry.mesh);
    VCGMeshMarkScope faceMark(entry.mesh);
    VCGMeshVertexMarkScope vertexMark(entry.mesh);
    entry.mesh.vert.EnableTexCoord();
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(entry.mesh);

    doc.beginFilterProgress(QObject::tr("Parametrize by Abstract Domain"));

    IsoParametrizator parametrizator;
    IsoParametrizator::StopMode stopMode = IsoParametrizator::SM_Corr;
    const QString criteria = params.getEnum(QStringLiteral("stopCriteria"));
    if (criteria == QStringLiteral("heuristic"))
        stopMode = IsoParametrizator::SM_Euristic;
    else if (criteria == QStringLiteral("regularity"))
        stopMode = IsoParametrizator::SM_Reg;
    else if (criteria == QStringLiteral("l2"))
        stopMode = IsoParametrizator::SM_L2;

    vcg::CallBackPos *callback =
        doc.progressCallback() ? doc.progressCallback() : vcg::DummyCallBackPos;
    parametrizator.SetParameters(callback, minFaces, maxFaces - minFaces, stopMode, accuracy);

    QStringList narration;
    IsoParametrizator::ReturnCode code = IsoParametrizator::FailParam;
    {
        const std::lock_guard<std::mutex> guard(isoparam_capture::mutex());
        const isoparam_capture::Scope capture(narration);
        vcg::tri::ParamEdgeCollapseParameter pecp;
        code = parametrizator.Parametrize<VCGMesh>(&entry.mesh, pecp, doubleStep);
    }
    for (const QString &line : narration)
        doc.writeLog(QStringLiteral("[isoparam] %1").arg(line),
                     Document::LogSource::VCG, Document::LogLevel::Debug);

    if (code != IsoParametrizator::Done) {
        const QString message =
            QObject::tr("Could not build an abstract domain: %1.").arg(describeReturnCode(code));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    float aggregate = 0.0f;
    float stretch = 0.0f;
    int domainFaces = 0;
    parametrizator.getValues(aggregate, stretch, domainFaces);

    auto abstractMesh = std::make_unique<AbstractMesh>();
    auto paramMesh = std::make_unique<ParamMesh>();
    parametrizator.ExportMeshes(*paramMesh, *abstractMesh);

    auto iso = std::make_unique<IsoParametrization>();
    if (!iso->Init(abstractMesh.get(), paramMesh.get())) {
        const QString message = QObject::tr("The abstract domain could not be initialized.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    iso->CopyParametrization<VCGMesh>(&entry.mesh);

    // Paint the sub-domain each face belongs to onto the layer as a per-face scalar, so the
    // domain regions can be seen on the mesh they were built from. The face's centre
    // decides: a region boundary is a path across the surface and does not follow mesh
    // edges, so a face can straddle one, and picking by centre keeps the regions crisp
    // where the per-vertex indices CopyParametrization writes would dither along the seam.
    bool facesIndexed = paramMesh->face.size() == entry.mesh.face.size();
    if (facesIndexed) {
        const IsoParametrization::CoordType centre(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
        for (std::size_t i = 0; i < entry.mesh.face.size(); ++i) {
            if (entry.mesh.face[i].IsD())
                continue;
            int domain = 0;
            vcg::Point2f uv;
            iso->Phi(&paramMesh->face[i], centre, domain, uv);
            entry.mesh.face[i].Q() = float(domain);
        }
    } else {
        doc.writeLog(QStringLiteral("[isoparam] the parametrized mesh has %1 faces against "
                                    "the layer's %2, so the per-face domain index was "
                                    "skipped")
                         .arg(paramMesh->face.size()).arg(entry.mesh.face.size()),
                     Document::LogSource::VCG, Document::LogLevel::Warning);
    }

    // Announce the mesh change *before* attaching the domain, not after. The UVs written
    // above are a change to this layer, and markMeshGeometryChanged drops plugin data that
    // does not survive one -- which this deliberately does not. Attaching first would
    // install the domain and then immediately throw it away.
    entry.ioMask |= vcg::tri::io::Mask::IOM_VERTTEXCOORD
        | vcg::tri::io::Mask::IOM_VERTQUALITY;
    if (facesIndexed)
        entry.ioMask |= vcg::tri::io::Mask::IOM_FACEQUALITY;
    doc.markMeshGeometryChanged(
        index, QObject::tr("Built an abstract domain for '%1'.").arg(entry.name));

    doc.setLayerData(index, kDomainKey,
                     std::make_shared<AbstractDomainData>(
                         std::move(abstractMesh), std::move(paramMesh), std::move(iso),
                         domainFaces, stretch, aggregate * 100.0f));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Abstract domain: %1 faces.").arg(domainFaces)
        << QObject::tr("One-way stretch efficiency: %1.").arg(double(stretch), 0, 'f', 4)
        << QObject::tr("Area and angle distortion: %1%.").arg(double(aggregate) * 100.0, 0, 'f', 2);
    if (facesIndexed) {
        result.infoMessages << QObject::tr(
            "Per-face scalar set to the domain region each face falls in, 0 to %1.")
                                   .arg(domainFaces - 1);
        result.visualizationHints.push_back(
            {index, MeshFilterVisualizationAttribute::FaceQuality});
    }
    doc.finishFilterProgress(true, QObject::tr("Abstract domain built."));
    return result;
}

void registerIsoParamFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<IsoParamFilterPlugin>());
}

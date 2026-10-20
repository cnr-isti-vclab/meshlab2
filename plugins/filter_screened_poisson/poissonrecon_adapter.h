#ifndef MESHLAB2_SCREENED_POISSON_POISSONRECON_ADAPTER_H
#define MESHLAB2_SCREENED_POISSON_POISSONRECON_ADAPTER_H

#include "document.h"
#include "vcgmesh.h"

#include "Src/PreProcessor.h"
#include "Src/Reconstructors.h"

#include <QMatrix4x4>
#include <limits>
#include <vector>

namespace ScreenedPoisson
{

using Real = float;
constexpr unsigned int Dim = 3;
using Position = PoissonRecon::Reconstructor::Position<Real, Dim>;
using Normal = PoissonRecon::Reconstructor::Normal<Real, Dim>;
using Gradient = PoissonRecon::Reconstructor::Gradient<Real, Dim>;
using Weight = PoissonRecon::Reconstructor::Weight<Real>;
using Color = PoissonRecon::Point<Real, Dim>;
using Face = PoissonRecon::Reconstructor::Face<2>;

struct SelectionOptions
{
    bool mergeVisible = false;
    bool confidenceFromQuality = false;
};

// A point the reconstruction can use: its normal is finite and not null. The others -- the vertices
// of a triangle mesh that belong to no face, a point cloud without normals -- are not fed to it; the
// layers are only read, so they are passed over and never deleted.
inline bool hasUsableNormal(const VCGVertex &vertex)
{
    return vcg::SquaredNorm(vertex.cN()) >= std::numeric_limits<float>::min() * 10.0f;
}

struct VertexRecord
{
    Position position;
    Gradient gradient;
    Weight density = 0;
};

struct VertexColorRecord
{
    Position position;
    Gradient gradient;
    Weight density = 0;
    Color color;
};

std::vector<int> selectedMeshIndices(const Document &doc, bool mergeVisible);
bool allSelectedMeshesHaveVertexColor(const Document &doc, const std::vector<int> &meshIndices);
// How many points the stream hands to the solver, and how many of the layers' points it leaves out.
qsizetype countInputSamples(const Document &doc, const std::vector<int> &meshIndices);
qsizetype countPointsWithoutNormal(const Document &doc, const std::vector<int> &meshIndices);

class DocumentOrientedPointStream
    : public PoissonRecon::Reconstructor::InputOrientedSampleStream<Real, Dim>
{
public:
    DocumentOrientedPointStream(const Document &doc, std::vector<int> meshIndices, SelectionOptions options = {});

    void reset(void) override;
    bool read(Position &p, Normal &n) override;

private:
    bool nextSample(Position &p, Normal &n, Color *color);

    const Document &m_doc;
    std::vector<int> m_meshIndices;
    SelectionOptions m_options;
    std::size_t m_meshCursor = 0;
    std::size_t m_vertexCursor = 0;
};

class DocumentOrientedPointColorStream
    : public PoissonRecon::Reconstructor::InputOrientedSampleStream<Real, Dim, Color>
{
public:
    DocumentOrientedPointColorStream(const Document &doc, std::vector<int> meshIndices, SelectionOptions options = {});

    void reset(void) override;
    bool read(Position &p, Normal &n, Color &c) override;

private:
    bool nextSample(Position &p, Normal &n, Color *color);

    const Document &m_doc;
    std::vector<int> m_meshIndices;
    SelectionOptions m_options;
    std::size_t m_meshCursor = 0;
    std::size_t m_vertexCursor = 0;
};

class VectorLevelSetVertexStream
    : public PoissonRecon::Reconstructor::OutputLevelSetVertexStream<Real, Dim>
{
public:
    size_t write(const Position &p, const Gradient &g, const Weight &w) override;
    size_t size(void) const override { return m_vertices.size(); }

    const std::vector<VertexRecord> &vertices() const { return m_vertices; }

private:
    std::vector<VertexRecord> m_vertices;
};

class VectorLevelSetVertexColorStream
    : public PoissonRecon::Reconstructor::OutputLevelSetVertexStream<Real, Dim, Color>
{
public:
    size_t write(const Position &p, const Gradient &g, const Weight &w, const Color &c) override;
    size_t size(void) const override { return m_vertices.size(); }

    const std::vector<VertexColorRecord> &vertices() const { return m_vertices; }

private:
    std::vector<VertexColorRecord> m_vertices;
};

class VectorFaceStream : public PoissonRecon::Reconstructor::OutputFaceStream<2>
{
public:
    size_t write(const Face &f) override;
    size_t size(void) const override { return m_faces.size(); }

    const std::vector<Face> &faces() const { return m_faces; }

private:
    std::vector<Face> m_faces;
};

void appendToMesh(const std::vector<VertexRecord> &vertices, const std::vector<Face> &faces, VCGMesh &mesh);
void appendToMesh(const std::vector<VertexColorRecord> &vertices, const std::vector<Face> &faces, VCGMesh &mesh);

} // namespace ScreenedPoisson

#endif

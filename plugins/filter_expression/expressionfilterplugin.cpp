#include "expressionfilterplugin.h"

#include "document.h"
#include "filter_refine.h"
#include "meshfilterpluginmanager.h"
#include <QVector3D>
#include <muParser.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <typeindex>
#include <vcg/complex/algorithms/create/marching_cubes.h>
#include <vcg/complex/algorithms/create/mc_closed_isosurface.h>
#include <vcg/complex/algorithms/create/mc_trivial_walker.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/refine.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/math/matrix33.h>
#include <algorithm>
#include <limits>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {
constexpr QLatin1StringView kFilterVertSelection("select_vertices_by_expression");
constexpr QLatin1StringView kFilterFaceSelection("select_faces_by_expression");
constexpr QLatin1StringView kFilterEdgeSelection("select_edges_by_expression");
constexpr QLatin1StringView kFilterEdgeColor("compute_edge_color_by_expression");
constexpr QLatin1StringView kFilterEdgeQuality("compute_edge_scalar_by_expression");
constexpr QLatin1StringView kFilterGeomFunc("compute_vertex_coordinates_by_expression");
constexpr QLatin1StringView kFilterFaceColor("compute_face_color_by_expression");
constexpr QLatin1StringView kFilterFaceQuality("compute_face_scalar_by_expression");
constexpr QLatin1StringView kFilterVertColor("compute_vertex_color_by_expression");
constexpr QLatin1StringView kFilterVertQuality("compute_vertex_scalar_by_expression");
constexpr QLatin1StringView kFilterVertTex("parametrize_per_vertex_by_expression");
constexpr QLatin1StringView kFilterWedgeTex("parametrize_per_wedge_by_expression");
constexpr QLatin1StringView kFilterVertNormal("compute_vertex_normals_by_expression");
constexpr QLatin1StringView kFilterFaceNormal("compute_face_normals_by_expression");
constexpr QLatin1StringView kFilterDefVertScalar("define_custom_vertex_scalar_attribute");
constexpr QLatin1StringView kFilterDefFaceScalar("define_custom_face_scalar_attribute");
constexpr QLatin1StringView kFilterDefVertPoint("define_custom_vertex_point_attribute");
constexpr QLatin1StringView kFilterDefFacePoint("define_custom_face_point_attribute");
constexpr QLatin1StringView kFilterDefEdgeScalar("define_custom_edge_scalar_attribute");
constexpr QLatin1StringView kFilterDefEdgePoint("define_custom_edge_point_attribute");
constexpr QLatin1StringView kFilterGrid("create_grid");
constexpr QLatin1StringView kFilterRefine("refine_by_user_expression");
constexpr QLatin1StringView kFilterIso("create_isosurface_from_expression");

QString parserOperatorsReferenceMarkdown()
{
    return QObject::tr(
        "You can use muparser built-in operators and functions, such as `&&`, `||`, `<`, `<=`, "
        "`>`, `>=`, `!=`, `==`, and the ternary `cond ? a : b`. "
        "Custom random helpers are also available: `rnd()` in `[0,1]` and `randInt(a)` in `[0,a)`.\n");
}

QString perVertexVariablesReferenceMarkdown()
{
    return QObject::tr(
        "Variables for per-vertex expressions:\n"
        "- Per-vertex: `x,y,z` (position), `nx,ny,nz` (normal), `r,g,b,a` (color), "
        "`q` (quality), `vi` (vertex index), `vtu,vtv,ti` (texture coordinates/index), "
        "`vsel` (1 if selected, 0 if not selected).\n"
        "- Bounding box: `xmin,ymin,zmin`, `xmax,ymax,zmax`, `xmid,ymid,zmid`, "
        "`xdim,ydim,zdim`, `bbdiag`.\n"
        "- User-defined attributes: all custom vertex attributes are available. "
        "Point3 attributes are available as three variables with `_x`, `_y`, `_z` appended.\n");
}

QString perFaceVariablesReferenceMarkdown()
{
    return QObject::tr(
        "Variables for per-face expressions:\n"
        "- Per-face: `fi` (face index), `fr,fg,fb,fa` (face color), `fq` (face quality), "
        "`fnx,fny,fnz` (face normal), `fsel` (1 if selected, 0 if not selected).\n"
        "- Per-vertex on the face: `x0,y0,z0`, `x1,y1,z1`, `x2,y2,z2`; "
        "`nx0,ny0,nz0`, `nx1,ny1,nz1`, `nx2,ny2,nz2`; "
        "`r0,g0,b0,a0`, `r1,g1,b1,a1`, `r2,g2,b2,a2`; "
        "`vi0,vi1,vi2`; `q0,q1,q2`; "
        "`wtu0,wtv0`, `wtu1,wtv1`, `wtu2,wtv2`; `ti`; "
        "`vsel0,vsel1,vsel2`.\n"
        "- Bounding box: `xmin,ymin,zmin`, `xmax,ymax,zmax`, `xmid,ymid,zmid`, "
        "`xdim,ydim,zdim`, `bbdiag`.\n"
        "- User-defined attributes: all custom face scalar attributes are available. "
        "Point3 attributes are available as three variables with `_x`, `_y`, `_z` appended.\n");
}

QString edgeRefineVariablesReferenceMarkdown()
{
    return QObject::tr(
        "Variables for edge-refinement expressions:\n"
        "- Edge endpoints: `x0,y0,z0` and `x1,y1,z1`.\n"
        "- Endpoint normals: `nx0,ny0,nz0` and `nx1,ny1,nz1`.\n"
        "- Endpoint colors: `r0,g0,b0` and `r1,g1,b1`.\n"
        "- Endpoint quality: `q0,q1`.\n");
}


uint8_t clampToByte(double value)
{
    if (!std::isfinite(value))
        return 0;
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

QString parserErrorString(const mu::Parser::exception_type &e)
{
    return QString::fromStdString(meshlab::filters::parserStringToStd(e.GetMsg()));
}

bool isValidIdentifier(const std::string &name)
{
    if (name.empty())
        return false;
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || first == '_'))
        return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || u == '_'))
            return false;
    }
    return true;
}

const std::unordered_set<std::string> &reservedVariables()
{
    static const std::unordered_set<std::string> kReserved = {
        "x",
        "y",
        "z",
        "nx",
        "ny",
        "nz",
        "r",
        "g",
        "b",
        "a",
        "q",
        "vi",
        "vtu",
        "vtv",
        "vsel",
        "x0",
        "y0",
        "z0",
        "x1",
        "y1",
        "z1",
        "x2",
        "y2",
        "z2",
        "nx0",
        "ny0",
        "nz0",
        "nx1",
        "ny1",
        "nz1",
        "nx2",
        "ny2",
        "nz2",
        "r0",
        "g0",
        "b0",
        "a0",
        "r1",
        "g1",
        "b1",
        "a1",
        "r2",
        "g2",
        "b2",
        "a2",
        "q0",
        "q1",
        "q2",
        "fr",
        "fg",
        "fb",
        "fa",
        "fnx",
        "fny",
        "fnz",
        "fq",
        "fi",
        "vi0",
        "vi1",
        "vi2",
        "wtu0",
        "wtv0",
        "wtu1",
        "wtv1",
        "wtu2",
        "wtv2",
        "fsel",
        "vsel0",
        "vsel1",
        "vsel2",
        "ti",
        "xmin",
        "ymin",
        "zmin",
        "xmax",
        "ymax",
        "zmax",
        "xmid",
        "ymid",
        "zmid",
        "xdim",
        "ydim",
        "zdim",
        "bbdiag",
        "rnd",
        "randInt"
    };
    return kReserved;
}

// The names of the attributes stored as a T.
//
// vcglib's GetAllPer*Attribute<T> cannot be used for this: it matches on element size
// alone, so a float attribute is also listed under <int> and an int under <float>. Bound
// twice under one name, the second binding wins and an expression reads a float's bit
// pattern as an integer -- 1.0 came back as 1065353216. The type each attribute was
// created with is recorded next to it, which is what deepCopyMesh already goes by.
//
// An attribute whose type was never recorded (typeid(void): it reached the mesh through a
// path that keeps only the size, such as a VMI load) is taken to be a float or a point by
// its size, as every attribute was before integers were readable.
template <class T, class AttributeSet>
std::vector<std::string> attributeNamesOfType(const AttributeSet &attributes)
{
    constexpr bool kTakeUnrecordedOfThisSize =
        std::is_same_v<T, float> || std::is_same_v<T, vcg::Point3f>;
    std::vector<std::string> names;
    for (const vcg::PointerToAttribute &attribute : attributes) {
        if (attribute._name.empty())
            continue;
        const bool recorded = attribute._type != std::type_index(typeid(void));
        if (recorded ? attribute._type == std::type_index(typeid(T))
                     : kTakeUnrecordedOfThisSize && attribute._sizeof == int(sizeof(T)))
            names.push_back(attribute._name);
    }
    return names;
}

// The three element kinds a custom attribute can live on. Binding attributes to expression
// variables and defining one are the same code for each kind; these say where a kind's
// attributes are kept and which Allocator calls reach them.
struct VertexElements
{
    template <class T> using Handle = VCGMesh::PerVertexAttributeHandle<T>;
    static auto &elements(VCGMesh &m) { return m.vert; }
    static const auto &attributes(const VCGMesh &m) { return m.vert_attr; }
    template <class T> static Handle<T> find(VCGMesh &m, const std::string &name)
    { return vcg::tri::Allocator<VCGMesh>::FindPerVertexAttribute<T>(m, name); }
    template <class T> static Handle<T> add(VCGMesh &m, const std::string &name)
    { return vcg::tri::Allocator<VCGMesh>::AddPerVertexAttribute<T>(m, name); }
};

struct FaceElements
{
    template <class T> using Handle = VCGMesh::PerFaceAttributeHandle<T>;
    static auto &elements(VCGMesh &m) { return m.face; }
    static const auto &attributes(const VCGMesh &m) { return m.face_attr; }
    template <class T> static Handle<T> find(VCGMesh &m, const std::string &name)
    { return vcg::tri::Allocator<VCGMesh>::FindPerFaceAttribute<T>(m, name); }
    template <class T> static Handle<T> add(VCGMesh &m, const std::string &name)
    { return vcg::tri::Allocator<VCGMesh>::AddPerFaceAttribute<T>(m, name); }
};

struct EdgeElements
{
    template <class T> using Handle = VCGMesh::PerEdgeAttributeHandle<T>;
    static auto &elements(VCGMesh &m) { return m.edge; }
    static const auto &attributes(const VCGMesh &m) { return m.edge_attr; }
    template <class T> static Handle<T> find(VCGMesh &m, const std::string &name)
    { return vcg::tri::Allocator<VCGMesh>::FindPerEdgeAttribute<T>(m, name); }
    template <class T> static Handle<T> add(VCGMesh &m, const std::string &name)
    { return vcg::tri::Allocator<VCGMesh>::AddPerEdgeAttribute<T>(m, name); }
};

// One element kind's custom attributes as expression variables: a float or int attribute
// `name` reads as `name`, a point attribute as `name_x`, `name_y` and `name_z`. Integer
// attributes (identifiers, labels) are readable but not writable: every define filter
// writes float, so an expression can test an int but never produce one.
//
// The variables are collected once per mesh and every parser is pointed at the same
// slots: a filter with several expressions (one per color channel or coordinate) binds
// several parsers, and each must see the values load() writes.
template <class Elements>
class BoundAttributes
{
public:
    void bind(mu::Parser &parser, VCGMesh &mesh)
    {
        if (m_mesh != &mesh) {
            m_mesh = &mesh;
            m_variables.clear();
            collect(mesh, m_scalars, { "" });
            collect(mesh, m_ints, { "" });
            collect(mesh, m_points, { "_x", "_y", "_z" });
            m_values.assign(m_variables.size(), 0.0);
        }
        for (std::size_t i = 0; i < m_variables.size(); ++i)
            parser.DefineVar(m_variables[i], &m_values[i]);
    }

    template <class Iterator>
    void load(Iterator element)
    {
        std::size_t k = 0;
        for (auto &handle : m_scalars)
            m_values[k++] = handle[element];
        for (auto &handle : m_ints)
            m_values[k++] = handle[element];
        for (auto &handle : m_points) {
            const vcg::Point3f p = handle[element];
            for (int c = 0; c < 3; ++c)
                m_values[k++] = p[c];
        }
    }

private:
    template <class T>
    void collect(VCGMesh &mesh,
                 std::vector<typename Elements::template Handle<T>> &handles,
                 std::initializer_list<const char *> suffixes)
    {
        handles.clear();
        for (const std::string &name : attributeNamesOfType<T>(Elements::attributes(mesh))) {
            if (!isValidIdentifier(name))
                continue;
            std::vector<std::string> variables;
            for (const char *suffix : suffixes)
                variables.push_back(name + suffix);
            const bool reserved = std::any_of(variables.begin(), variables.end(),
                [](const std::string &v) { return reservedVariables().count(v) > 0; });
            if (reserved)
                continue;
            const auto handle = Elements::template find<T>(mesh, name);
            if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
                continue;
            handles.push_back(handle);
            m_variables.insert(m_variables.end(), variables.begin(), variables.end());
        }
    }

    const VCGMesh *m_mesh = nullptr;
    std::vector<typename Elements::template Handle<float>> m_scalars;
    std::vector<typename Elements::template Handle<int>> m_ints;
    std::vector<typename Elements::template Handle<vcg::Point3f>> m_points;
    std::vector<std::string> m_variables;
    std::vector<double> m_values;
};

struct ParserRuntime
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double nx = 0.0;
    double ny = 0.0;
    double nz = 0.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
    double q = 0.0;
    double vi = 0.0;
    double vtu = 0.0;
    double vtv = 0.0;
    double vsel = 0.0;

    double x0 = 0.0;
    double y0 = 0.0;
    double z0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double z1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double z2 = 0.0;
    double nx0 = 0.0;
    double ny0 = 0.0;
    double nz0 = 0.0;
    double nx1 = 0.0;
    double ny1 = 0.0;
    double nz1 = 0.0;
    double nx2 = 0.0;
    double ny2 = 0.0;
    double nz2 = 0.0;
    double r0 = 0.0;
    double g0 = 0.0;
    double b0 = 0.0;
    double a0 = 0.0;
    double r1 = 0.0;
    double g1 = 0.0;
    double b1 = 0.0;
    double a1 = 0.0;
    double r2 = 0.0;
    double g2 = 0.0;
    double b2 = 0.0;
    double a2 = 0.0;
    double q0 = 0.0;
    double q1 = 0.0;
    double q2 = 0.0;
    double fr = 255.0;
    double fg = 255.0;
    double fb = 255.0;
    double fa = 255.0;
    double fnx = 0.0;
    double fny = 0.0;
    double fnz = 0.0;
    double fq = 0.0;
    double fi = 0.0;
    double vi0 = 0.0;
    double vi1 = 0.0;
    double vi2 = 0.0;
    double wtu0 = 0.0;
    double wtv0 = 0.0;
    double wtu1 = 0.0;
    double wtv1 = 0.0;
    double wtu2 = 0.0;
    double wtv2 = 0.0;
    double fsel = 0.0;
    double vsel0 = 0.0;
    double vsel1 = 0.0;
    double vsel2 = 0.0;
    double ti = 0.0;

    // Edge context. The two endpoints reuse the face context's corner slots
    // (x0..z1, nx0..nz1, r0..a1, q0/q1, vi0/vi1, vsel0/vsel1) because a parser is
    // only ever configured for one element kind, so there is nothing to collide
    // with. Only the edge's own data and the derived conveniences are new.
    double er = 255.0;
    double eg = 255.0;
    double eb = 255.0;
    double ea = 255.0;
    double eq = 0.0;
    double ei = 0.0;
    double esel = 0.0;
    double elen = 0.0;
    double emx = 0.0;
    double emy = 0.0;
    double emz = 0.0;
    double edx = 0.0;
    double edy = 0.0;
    double edz = 0.0;
    // Position along the edge's chain (Clean::EdgeMeshChains), filled once per run by the
    // first edge parser: es and earc are taken at the edge's midpoint.
    double es = 0.0;
    double earc = 0.0;
    double eidx = 0.0;
    double ech = 0.0;
    std::vector<int> chainOfEdge;
    std::vector<int> positionInChain;
    std::vector<float> arcLengthAtStart;
    std::vector<float> chainLength;

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    double xmid = 0.0;
    double ymid = 0.0;
    double zmid = 0.0;
    double xdim = 0.0;
    double ydim = 0.0;
    double zdim = 0.0;
    double bbdiag = 0.0;

    BoundAttributes<VertexElements> vertexAttributes;
    BoundAttributes<FaceElements> faceAttributes;
    BoundAttributes<EdgeElements> edgeAttributes;
};

struct CurrentMeshRef
{
    int index = -1;
    Document::MeshEntry *entry = nullptr;
};

std::optional<CurrentMeshRef> currentMesh(Document &doc, QString &error)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount()) {
        error = QObject::tr("No current mesh selected.");
        return std::nullopt;
    }
    return CurrentMeshRef{ index, &doc.mesh(index) };
}

void setBBoxRuntime(ParserRuntime &runtime, const VCGMesh &mesh)
{
    runtime.xmin = mesh.bbox.min.X();
    runtime.ymin = mesh.bbox.min.Y();
    runtime.zmin = mesh.bbox.min.Z();
    runtime.xmax = mesh.bbox.max.X();
    runtime.ymax = mesh.bbox.max.Y();
    runtime.zmax = mesh.bbox.max.Z();
    runtime.xdim = mesh.bbox.DimX();
    runtime.ydim = mesh.bbox.DimY();
    runtime.zdim = mesh.bbox.DimZ();
    runtime.bbdiag = mesh.bbox.Diag();
    const vcg::Point3f center = mesh.bbox.Center();
    runtime.xmid = center.X();
    runtime.ymid = center.Y();
    runtime.zmid = center.Z();
}

void defineCommonBBoxVars(mu::Parser &parser, ParserRuntime &runtime)
{
    parser.DefineVar("xmin", &runtime.xmin);
    parser.DefineVar("ymin", &runtime.ymin);
    parser.DefineVar("zmin", &runtime.zmin);
    parser.DefineVar("xmax", &runtime.xmax);
    parser.DefineVar("ymax", &runtime.ymax);
    parser.DefineVar("zmax", &runtime.zmax);
    parser.DefineVar("xmid", &runtime.xmid);
    parser.DefineVar("ymid", &runtime.ymid);
    parser.DefineVar("zmid", &runtime.zmid);
    parser.DefineVar("xdim", &runtime.xdim);
    parser.DefineVar("ydim", &runtime.ydim);
    parser.DefineVar("zdim", &runtime.zdim);
    parser.DefineVar("bbdiag", &runtime.bbdiag);
}

void setPerVertexVariables(mu::Parser &parser, ParserRuntime &runtime, VCGMesh &mesh)
{
    parser.DefineVar("x", &runtime.x);
    parser.DefineVar("y", &runtime.y);
    parser.DefineVar("z", &runtime.z);
    parser.DefineVar("nx", &runtime.nx);
    parser.DefineVar("ny", &runtime.ny);
    parser.DefineVar("nz", &runtime.nz);
    parser.DefineVar("r", &runtime.r);
    parser.DefineVar("g", &runtime.g);
    parser.DefineVar("b", &runtime.b);
    parser.DefineVar("a", &runtime.a);
    parser.DefineVar("q", &runtime.q);
    parser.DefineVar("vi", &runtime.vi);
    parser.DefineVar("vtu", &runtime.vtu);
    parser.DefineVar("vtv", &runtime.vtv);
    parser.DefineVar("ti", &runtime.ti);
    parser.DefineVar("vsel", &runtime.vsel);
    defineCommonBBoxVars(parser, runtime);
    meshlab::filters::defineParserCustomFunctions(parser);
    runtime.vertexAttributes.bind(parser, mesh);
}

void setPerFaceVariables(mu::Parser &parser, ParserRuntime &runtime, VCGMesh &mesh)
{
    parser.DefineVar("x0", &runtime.x0);
    parser.DefineVar("y0", &runtime.y0);
    parser.DefineVar("z0", &runtime.z0);
    parser.DefineVar("x1", &runtime.x1);
    parser.DefineVar("y1", &runtime.y1);
    parser.DefineVar("z1", &runtime.z1);
    parser.DefineVar("x2", &runtime.x2);
    parser.DefineVar("y2", &runtime.y2);
    parser.DefineVar("z2", &runtime.z2);
    parser.DefineVar("nx0", &runtime.nx0);
    parser.DefineVar("ny0", &runtime.ny0);
    parser.DefineVar("nz0", &runtime.nz0);
    parser.DefineVar("nx1", &runtime.nx1);
    parser.DefineVar("ny1", &runtime.ny1);
    parser.DefineVar("nz1", &runtime.nz1);
    parser.DefineVar("nx2", &runtime.nx2);
    parser.DefineVar("ny2", &runtime.ny2);
    parser.DefineVar("nz2", &runtime.nz2);
    parser.DefineVar("r0", &runtime.r0);
    parser.DefineVar("g0", &runtime.g0);
    parser.DefineVar("b0", &runtime.b0);
    parser.DefineVar("a0", &runtime.a0);
    parser.DefineVar("r1", &runtime.r1);
    parser.DefineVar("g1", &runtime.g1);
    parser.DefineVar("b1", &runtime.b1);
    parser.DefineVar("a1", &runtime.a1);
    parser.DefineVar("r2", &runtime.r2);
    parser.DefineVar("g2", &runtime.g2);
    parser.DefineVar("b2", &runtime.b2);
    parser.DefineVar("a2", &runtime.a2);
    parser.DefineVar("q0", &runtime.q0);
    parser.DefineVar("q1", &runtime.q1);
    parser.DefineVar("q2", &runtime.q2);
    parser.DefineVar("fr", &runtime.fr);
    parser.DefineVar("fg", &runtime.fg);
    parser.DefineVar("fb", &runtime.fb);
    parser.DefineVar("fa", &runtime.fa);
    parser.DefineVar("fnx", &runtime.fnx);
    parser.DefineVar("fny", &runtime.fny);
    parser.DefineVar("fnz", &runtime.fnz);
    parser.DefineVar("fq", &runtime.fq);
    parser.DefineVar("fi", &runtime.fi);
    parser.DefineVar("vi0", &runtime.vi0);
    parser.DefineVar("vi1", &runtime.vi1);
    parser.DefineVar("vi2", &runtime.vi2);
    parser.DefineVar("wtu0", &runtime.wtu0);
    parser.DefineVar("wtv0", &runtime.wtv0);
    parser.DefineVar("wtu1", &runtime.wtu1);
    parser.DefineVar("wtv1", &runtime.wtv1);
    parser.DefineVar("wtu2", &runtime.wtu2);
    parser.DefineVar("wtv2", &runtime.wtv2);
    parser.DefineVar("fsel", &runtime.fsel);
    parser.DefineVar("vsel0", &runtime.vsel0);
    parser.DefineVar("vsel1", &runtime.vsel1);
    parser.DefineVar("vsel2", &runtime.vsel2);
    parser.DefineVar("ti", &runtime.ti);
    defineCommonBBoxVars(parser, runtime);
    meshlab::filters::defineParserCustomFunctions(parser);
    runtime.faceAttributes.bind(parser, mesh);
}

// An edge's endpoints are exposed with the same names the face context gives its
// first two corners, so an expression written against one reads the same way in the
// other. What edges add is their own colour and scalar, plus length, midpoint and
// direction -- all derivable from the endpoints, but they are what anyone actually
// reaches for on a polyline.
void setPerEdgeVariables(mu::Parser &parser, ParserRuntime &runtime, VCGMesh &mesh)
{
    parser.DefineVar("x0", &runtime.x0);
    parser.DefineVar("y0", &runtime.y0);
    parser.DefineVar("z0", &runtime.z0);
    parser.DefineVar("x1", &runtime.x1);
    parser.DefineVar("y1", &runtime.y1);
    parser.DefineVar("z1", &runtime.z1);
    parser.DefineVar("nx0", &runtime.nx0);
    parser.DefineVar("ny0", &runtime.ny0);
    parser.DefineVar("nz0", &runtime.nz0);
    parser.DefineVar("nx1", &runtime.nx1);
    parser.DefineVar("ny1", &runtime.ny1);
    parser.DefineVar("nz1", &runtime.nz1);
    parser.DefineVar("r0", &runtime.r0);
    parser.DefineVar("g0", &runtime.g0);
    parser.DefineVar("b0", &runtime.b0);
    parser.DefineVar("a0", &runtime.a0);
    parser.DefineVar("r1", &runtime.r1);
    parser.DefineVar("g1", &runtime.g1);
    parser.DefineVar("b1", &runtime.b1);
    parser.DefineVar("a1", &runtime.a1);
    parser.DefineVar("q0", &runtime.q0);
    parser.DefineVar("q1", &runtime.q1);
    parser.DefineVar("vi0", &runtime.vi0);
    parser.DefineVar("vi1", &runtime.vi1);
    parser.DefineVar("vsel0", &runtime.vsel0);
    parser.DefineVar("vsel1", &runtime.vsel1);
    parser.DefineVar("er", &runtime.er);
    parser.DefineVar("eg", &runtime.eg);
    parser.DefineVar("eb", &runtime.eb);
    parser.DefineVar("ea", &runtime.ea);
    parser.DefineVar("eq", &runtime.eq);
    parser.DefineVar("ei", &runtime.ei);
    parser.DefineVar("esel", &runtime.esel);
    parser.DefineVar("elen", &runtime.elen);
    parser.DefineVar("emx", &runtime.emx);
    parser.DefineVar("emy", &runtime.emy);
    parser.DefineVar("emz", &runtime.emz);
    parser.DefineVar("edx", &runtime.edx);
    parser.DefineVar("edy", &runtime.edy);
    parser.DefineVar("edz", &runtime.edz);
    parser.DefineVar("es", &runtime.es);
    parser.DefineVar("earc", &runtime.earc);
    parser.DefineVar("eidx", &runtime.eidx);
    parser.DefineVar("ech", &runtime.ech);
    if (runtime.chainOfEdge.size() != mesh.edge.size())
        vcg::tri::Clean<VCGMesh>::EdgeMeshChains(mesh, runtime.chainOfEdge, runtime.positionInChain,
                                                 runtime.arcLengthAtStart, runtime.chainLength);
    defineCommonBBoxVars(parser, runtime);
    meshlab::filters::defineParserCustomFunctions(parser);
    runtime.edgeAttributes.bind(parser, mesh);
}

void setVertexRuntime(
    ParserRuntime &runtime,
    VCGMesh::VertexIterator vi,
    VCGMesh &mesh)
{
    runtime.x = vi->P()[0];
    runtime.y = vi->P()[1];
    runtime.z = vi->P()[2];
    runtime.nx = vi->N()[0];
    runtime.ny = vi->N()[1];
    runtime.nz = vi->N()[2];
    runtime.r = vi->C()[0];
    runtime.g = vi->C()[1];
    runtime.b = vi->C()[2];
    runtime.a = vi->C()[3];
    runtime.q = vi->Q();
    runtime.vi = double(vi - mesh.vert.begin());
    runtime.vsel = vi->IsS() ? 1.0 : 0.0;

    if (vcg::tri::HasPerVertexTexCoord(mesh)) {
        runtime.vtu = vi->T().U();
        runtime.vtv = vi->T().V();
        runtime.ti = vi->T().N();
    } else {
        runtime.vtu = runtime.vtv = runtime.ti = 0.0;
    }

    runtime.vertexAttributes.load(vi);
}

void setFaceRuntime(
    ParserRuntime &runtime,
    VCGMesh::FaceIterator fi,
    VCGMesh &mesh)
{
    runtime.x0 = fi->V(0)->P()[0];
    runtime.y0 = fi->V(0)->P()[1];
    runtime.z0 = fi->V(0)->P()[2];
    runtime.x1 = fi->V(1)->P()[0];
    runtime.y1 = fi->V(1)->P()[1];
    runtime.z1 = fi->V(1)->P()[2];
    runtime.x2 = fi->V(2)->P()[0];
    runtime.y2 = fi->V(2)->P()[1];
    runtime.z2 = fi->V(2)->P()[2];

    runtime.nx0 = fi->V(0)->N()[0];
    runtime.ny0 = fi->V(0)->N()[1];
    runtime.nz0 = fi->V(0)->N()[2];
    runtime.nx1 = fi->V(1)->N()[0];
    runtime.ny1 = fi->V(1)->N()[1];
    runtime.nz1 = fi->V(1)->N()[2];
    runtime.nx2 = fi->V(2)->N()[0];
    runtime.ny2 = fi->V(2)->N()[1];
    runtime.nz2 = fi->V(2)->N()[2];

    runtime.r0 = fi->V(0)->C()[0];
    runtime.g0 = fi->V(0)->C()[1];
    runtime.b0 = fi->V(0)->C()[2];
    runtime.a0 = fi->V(0)->C()[3];
    runtime.r1 = fi->V(1)->C()[0];
    runtime.g1 = fi->V(1)->C()[1];
    runtime.b1 = fi->V(1)->C()[2];
    runtime.a1 = fi->V(1)->C()[3];
    runtime.r2 = fi->V(2)->C()[0];
    runtime.g2 = fi->V(2)->C()[1];
    runtime.b2 = fi->V(2)->C()[2];
    runtime.a2 = fi->V(2)->C()[3];

    runtime.q0 = fi->V(0)->Q();
    runtime.q1 = fi->V(1)->Q();
    runtime.q2 = fi->V(2)->Q();

    runtime.fq = vcg::tri::HasPerFaceQuality(mesh) ? fi->Q() : 0.0;
    if (vcg::tri::HasPerFaceColor(mesh)) {
        runtime.fr = fi->C()[0];
        runtime.fg = fi->C()[1];
        runtime.fb = fi->C()[2];
        runtime.fa = fi->C()[3];
    } else {
        runtime.fr = runtime.fg = runtime.fb = runtime.fa = 255.0;
    }
    runtime.fnx = fi->N()[0];
    runtime.fny = fi->N()[1];
    runtime.fnz = fi->N()[2];

    runtime.fi = double(fi - mesh.face.begin());
    runtime.vi0 = double(fi->V(0) - &mesh.vert[0]);
    runtime.vi1 = double(fi->V(1) - &mesh.vert[0]);
    runtime.vi2 = double(fi->V(2) - &mesh.vert[0]);

    if (vcg::tri::HasPerWedgeTexCoord(mesh)) {
        runtime.wtu0 = fi->WT(0).U();
        runtime.wtv0 = fi->WT(0).V();
        runtime.wtu1 = fi->WT(1).U();
        runtime.wtv1 = fi->WT(1).V();
        runtime.wtu2 = fi->WT(2).U();
        runtime.wtv2 = fi->WT(2).V();
        runtime.ti = fi->WT(0).N();
    } else {
        runtime.wtu0 = runtime.wtv0 = runtime.wtu1 = runtime.wtv1 = runtime.wtu2 = runtime.wtv2 = 0.0;
        runtime.ti = 0.0;
    }

    runtime.fsel = fi->IsS() ? 1.0 : 0.0;
    runtime.vsel0 = fi->V(0)->IsS() ? 1.0 : 0.0;
    runtime.vsel1 = fi->V(1)->IsS() ? 1.0 : 0.0;
    runtime.vsel2 = fi->V(2)->IsS() ? 1.0 : 0.0;

    runtime.faceAttributes.load(fi);
}

void setEdgeRuntime(
    ParserRuntime &runtime,
    VCGMesh::EdgeIterator ei,
    VCGMesh &mesh)
{
    const VCGVertex *v0 = ei->cV(0);
    const VCGVertex *v1 = ei->cV(1);
    const vcg::Point3f p0 = v0 ? v0->cP() : vcg::Point3f(0.0f, 0.0f, 0.0f);
    const vcg::Point3f p1 = v1 ? v1->cP() : vcg::Point3f(0.0f, 0.0f, 0.0f);

    runtime.x0 = p0[0];
    runtime.y0 = p0[1];
    runtime.z0 = p0[2];
    runtime.x1 = p1[0];
    runtime.y1 = p1[1];
    runtime.z1 = p1[2];
    runtime.nx0 = v0 ? v0->cN()[0] : 0.0;
    runtime.ny0 = v0 ? v0->cN()[1] : 0.0;
    runtime.nz0 = v0 ? v0->cN()[2] : 0.0;
    runtime.nx1 = v1 ? v1->cN()[0] : 0.0;
    runtime.ny1 = v1 ? v1->cN()[1] : 0.0;
    runtime.nz1 = v1 ? v1->cN()[2] : 0.0;
    runtime.r0 = v0 ? v0->cC()[0] : 255.0;
    runtime.g0 = v0 ? v0->cC()[1] : 255.0;
    runtime.b0 = v0 ? v0->cC()[2] : 255.0;
    runtime.a0 = v0 ? v0->cC()[3] : 255.0;
    runtime.r1 = v1 ? v1->cC()[0] : 255.0;
    runtime.g1 = v1 ? v1->cC()[1] : 255.0;
    runtime.b1 = v1 ? v1->cC()[2] : 255.0;
    runtime.a1 = v1 ? v1->cC()[3] : 255.0;
    runtime.q0 = v0 ? v0->cQ() : 0.0;
    runtime.q1 = v1 ? v1->cQ() : 0.0;
    runtime.vi0 = v0 ? double(vcg::tri::Index(mesh, v0)) : -1.0;
    runtime.vi1 = v1 ? double(vcg::tri::Index(mesh, v1)) : -1.0;
    runtime.vsel0 = (v0 && v0->IsS()) ? 1.0 : 0.0;
    runtime.vsel1 = (v1 && v1->IsS()) ? 1.0 : 0.0;

    runtime.er = ei->cC()[0];
    runtime.eg = ei->cC()[1];
    runtime.eb = ei->cC()[2];
    runtime.ea = ei->cC()[3];
    runtime.eq = ei->cQ();
    runtime.ei = double(ei - mesh.edge.begin());
    runtime.esel = ei->IsS() ? 1.0 : 0.0;

    const vcg::Point3f delta = p1 - p0;
    const double len = double(delta.Norm());
    runtime.elen = len;
    runtime.emx = 0.5 * (double(p0[0]) + double(p1[0]));
    runtime.emy = 0.5 * (double(p0[1]) + double(p1[1]));
    runtime.emz = 0.5 * (double(p0[2]) + double(p1[2]));
    // A zero-length edge has no direction; report the zero vector rather than a NaN,
    // so an expression using edx still evaluates instead of poisoning the whole run.
    const double inv = len > 0.0 ? 1.0 / len : 0.0;
    runtime.edx = double(delta[0]) * inv;
    runtime.edy = double(delta[1]) * inv;
    runtime.edz = double(delta[2]) * inv;

    const std::size_t index = std::size_t(ei - mesh.edge.begin());
    const int chain = runtime.chainOfEdge[index];
    runtime.ech = chain;
    runtime.eidx = runtime.positionInChain[index];
    runtime.earc = double(runtime.arcLengthAtStart[index]) + 0.5 * len;
    const double chainLength = chain >= 0 ? double(runtime.chainLength[std::size_t(chain)]) : 0.0;
    runtime.es = chainLength > 0.0 ? runtime.earc / chainLength : 0.0;

    runtime.edgeAttributes.load(ei);
}

// With selectedOnly, Normalize and Map work on the selected elements alone: their range is
// the selection's own and nothing outside it is rewritten, where the vcglib helpers span and
// recolour the whole mesh.
template <class Container>
std::pair<float, float> selectedQualityRange(const Container &elements)
{
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (const auto &e : elements) {
        if (!e.IsD() && e.IsS()) {
            lo = std::min(lo, float(e.cQ()));
            hi = std::max(hi, float(e.cQ()));
        }
    }
    return { lo, hi };
}

template <class Container>
void normalizeSelectedQuality(Container &elements)
{
    const auto [lo, hi] = selectedQualityRange(elements);
    const float span = hi - lo;
    for (auto &e : elements) {
        if (!e.IsD() && e.IsS())
            e.Q() = span > 0.0f ? (e.Q() - lo) / span : 0.0f;
    }
}

template <class Container>
void rampSelectedQuality(Container &elements)
{
    const auto [lo, hi] = selectedQualityRange(elements);
    for (auto &e : elements) {
        if (!e.IsD() && e.IsS())
            e.C() = vcg::GetColorMapping(e.Q(), lo, hi, vcg::ColorMap::RGB);
    }
}

bool checkCustomAttributeName(const QString &name, QString &error)
{
    const std::string n = name.trimmed().toStdString();
    if (!isValidIdentifier(n)) {
        error = QObject::tr(
            "Invalid attribute name: only letters, numbers and underscores are allowed, and the name must not start with a number.");
        return false;
    }
    if (reservedVariables().count(n) > 0) {
        error = QObject::tr("Attribute name '%1' is reserved by filter variables.").arg(name);
        return false;
    }
    return true;
}

// The expression context of one element kind: its variables, and their values for one element.
template <class Elements>
void setElementVariables(mu::Parser &parser, ParserRuntime &runtime, VCGMesh &mesh)
{
    if constexpr (std::is_same_v<Elements, VertexElements>)
        setPerVertexVariables(parser, runtime, mesh);
    else if constexpr (std::is_same_v<Elements, FaceElements>)
        setPerFaceVariables(parser, runtime, mesh);
    else
        setPerEdgeVariables(parser, runtime, mesh);
}

template <class Elements, class Iterator>
void setElementRuntime(ParserRuntime &runtime, Iterator element, VCGMesh &mesh)
{
    if constexpr (std::is_same_v<Elements, VertexElements>)
        setVertexRuntime(runtime, element, mesh);
    else if constexpr (std::is_same_v<Elements, FaceElements>)
        setFaceRuntime(runtime, element, mesh);
    else
        setEdgeRuntime(runtime, element, mesh);
}

// The body of every Define Custom ... Attribute filter: create the attribute (or refill it,
// if it exists with the same type) and evaluate one expression per component -- one for a
// scalar, three for a point -- on every element of the kind. Returns an error, or an empty
// string with the number of elements written in processed.
template <class Elements, class T>
QString defineCustomAttribute(VCGMesh &mesh, ParserRuntime &runtime, const QString &name,
                              const QStringList &expressions, int &processed)
{
    QString error;
    if (!checkCustomAttributeName(name, error))
        return error;
    const std::string stdName = name.toStdString();
    const auto &existing = Elements::attributes(mesh);
    const bool exists = std::any_of(existing.begin(), existing.end(),
        [&](const vcg::PointerToAttribute &a) { return a._name == stdName; });
    auto handle = exists ? Elements::template find<T>(mesh, stdName)
                         : Elements::template add<T>(mesh, stdName);
    if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
        return QObject::tr("Attribute '%1' already exists with a different type.").arg(name);

    std::vector<mu::Parser> parsers(std::size_t(expressions.size()));
    processed = 0;
    try {
        for (std::size_t c = 0; c < parsers.size(); ++c) {
            setElementVariables<Elements>(parsers[c], runtime, mesh);
            parsers[c].SetExpr(expressions[int(c)].toStdString());
        }
        auto &elements = Elements::elements(mesh);
        for (auto it = elements.begin(); it != elements.end(); ++it) {
            setElementRuntime<Elements>(runtime, it, mesh);
            if constexpr (std::is_same_v<T, vcg::Point3f>)
                handle[it] = vcg::Point3f(float(parsers[0].Eval()), float(parsers[1].Eval()),
                                          float(parsers[2].Eval()));
            else
                handle[it] = float(parsers[0].Eval());
            ++processed;
        }
    } catch (mu::Parser::exception_type &e) {
        return parserErrorString(e);
    }
    return {};
}

using DefineCustomAttributeFn = QString (*)(VCGMesh &, ParserRuntime &, const QString &,
                                            const QStringList &, int &);

struct DefineCustomAttributeFilter
{
    QLatin1StringView id;
    DefineCustomAttributeFn define;
    bool point;
    const char *description; // "per-vertex scalar", for the history entry
    const char *elements;    // "vertices", for the result message
};

const DefineCustomAttributeFilter kDefineCustomAttributeFilters[] = {
    { kFilterDefVertScalar, &defineCustomAttribute<VertexElements, float>, false,
      QT_TR_NOOP("per-vertex scalar"), QT_TR_NOOP("vertices") },
    { kFilterDefFaceScalar, &defineCustomAttribute<FaceElements, float>, false,
      QT_TR_NOOP("per-face scalar"), QT_TR_NOOP("faces") },
    { kFilterDefEdgeScalar, &defineCustomAttribute<EdgeElements, float>, false,
      QT_TR_NOOP("per-edge scalar"), QT_TR_NOOP("edges") },
    { kFilterDefVertPoint, &defineCustomAttribute<VertexElements, vcg::Point3f>, true,
      QT_TR_NOOP("per-vertex point"), QT_TR_NOOP("vertices") },
    { kFilterDefFacePoint, &defineCustomAttribute<FaceElements, vcg::Point3f>, true,
      QT_TR_NOOP("per-face point"), QT_TR_NOOP("faces") },
    { kFilterDefEdgePoint, &defineCustomAttribute<EdgeElements, vcg::Point3f>, true,
      QT_TR_NOOP("per-edge point"), QT_TR_NOOP("edges") },
};
}

QString ExpressionFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.expression");
}

QString ExpressionFilterPlugin::name() const
{
    return QObject::tr("Expression Filters");
}

MeshFilterRunResult ExpressionFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    using Mask = vcg::tri::io::Mask;

    auto fail = [](const QString &msg) {
        MeshFilterRunResult r;
        r.success = false;
        r.errorMessage = msg;
        return r;
    };

    // Randomness in these filters is opt-in: it only happens if the user's formula
    // calls rnd() or randInt(). Seed the shared generator here anyway, so that a
    // formula which does use them is reproducible when randomSeed is pinned. The
    // seed is not echoed in the result messages, since the vast majority of runs
    // never touch the generator.
    meshlab::filters::seedParserRandom(params.getRandomSeed().value);

    if (filterId == QString::fromLatin1(kFilterGrid)) {
        const int w = params.getInt(QStringLiteral("numVertU"));
        const int h = params.getInt(QStringLiteral("numVertV"));
        double su = params.getDouble(QStringLiteral("sizeU"));
        double sv = params.getDouble(QStringLiteral("sizeV"));
        const QVector3D a = params.getPoint3f(QStringLiteral("axis"));
        const int fitLayer = params.getMesh(QStringLiteral("fitLayer"), -1);
        const bool center = params.getBool(QStringLiteral("center"));
        if (w <= 1 || h <= 1)
            return fail(QObject::tr("Grid vertex counts must be greater than 1."));
        if (!std::isfinite(su) || !std::isfinite(sv) || su <= 0.0 || sv <= 0.0)
            return fail(QObject::tr("Grid sizes must be finite and greater than zero."));

        vcg::Point3f normal(a.x(), a.y(), a.z());
        if (normal.SquaredNorm() <= 1e-20f)
            return fail(QObject::tr("The grid normal must be non-zero."));
        normal.Normalize();

        QStringList notes;

        // Fitting to a layer answers both questions at once -- how big, and where -- from
        // the box the viewport draws around that layer, which is its local box carried
        // through its matrix. Hence the centre through map() and the diagonal through
        // mapVector(): a rotation leaves the diagonal's length alone, a scale does not.
        vcg::Point3f origin(0.0f, 0.0f, 0.0f);
        bool centred = center;
        if (fitLayer >= 0 && fitLayer < doc.meshCount()) {
            const Document::MeshEntry &ref = doc.mesh(fitLayer);
            if (ref.mesh.VN() <= 0)
                return fail(QObject::tr("Fit layer '%1' has no vertices.").arg(ref.name));
            const vcg::Point3f c = ref.mesh.bbox.Center();
            const vcg::Point3f d = ref.mesh.bbox.max - ref.mesh.bbox.min;
            const QVector3D wc = ref.transform.map(QVector3D(c[0], c[1], c[2]));
            const float diag = ref.transform.mapVector(QVector3D(d[0], d[1], d[2])).length();
            if (!(diag > 0.0f))
                return fail(QObject::tr("Fit layer '%1' has an empty bounding box.").arg(ref.name));
            su = sv = double(diag);
            origin = vcg::Point3f(wc.x(), wc.y(), wc.z());
            centred = true;
            notes << QObject::tr("Sized %1 and centred on the bounding box of '%2'.")
                         .arg(diag)
                         .arg(ref.name);
        }

        VCGMesh generated;
        vcg::tri::Grid(generated, w, h, float(su), float(sv));
        // vcg::tri::Grid winds its faces so that they look away from +Z. The old code hid
        // that behind a 180-degree turn about Y, which also flipped the grid onto -X; now
        // that the plane's facing is a parameter, the winding is corrected at the source so
        // the faces end up pointing along the requested normal and U, V stay +X, +Y.
        vcg::tri::Clean<VCGMesh>::FlipMesh(generated);

        // vcg::tri::Grid lays the samples out over [0,su] x [0,sv] in the XY plane, so
        // centring is a shift by half the extent -- not by half a step, which is what the
        // previous arithmetic did and which left the grid a few percent off centre.
        const vcg::Point3f shift = centred
            ? vcg::Point3f(float(-su) * 0.5f, float(-sv) * 0.5f, 0.0f)
            : vcg::Point3f(0.0f, 0.0f, 0.0f);
        // The shortest rotation taking +Z to the requested normal, so U and V stay as close
        // as they can to X and Y and the grid does not spin for no reason.
        const vcg::Matrix33f turn =
            vcg::RotationMatrix(vcg::Point3f(0.0f, 0.0f, 1.0f), normal, true);
        for (auto &v : generated.vert)
            v.P() = origin + turn * (v.P() + shift);
        vcg::tri::UpdateBounding<VCGMesh>::Box(generated);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(generated);

        const int newIndex = doc.addMesh(
            generated,
            {},
            Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to add generated grid mesh."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices = { newIndex };
        result.infoMessages = {
            QObject::tr("Generated grid '%1' (%2 vertices, %3 faces).")
                .arg(doc.mesh(newIndex).name)
                .arg(doc.mesh(newIndex).mesh.VN())
                .arg(doc.mesh(newIndex).mesh.FN())
        };
        result.infoMessages += notes;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterIso)) {
        const double voxelSize = params.getDouble(QStringLiteral("voxelSize"));
        const double minX = params.getDouble(QStringLiteral("minX"));
        const double minY = params.getDouble(QStringLiteral("minY"));
        const double minZ = params.getDouble(QStringLiteral("minZ"));
        const double maxX = params.getDouble(QStringLiteral("maxX"));
        const double maxY = params.getDouble(QStringLiteral("maxY"));
        const double maxZ = params.getDouble(QStringLiteral("maxZ"));
        const QString expression = params.getString(QStringLiteral("expr"));
        if (!std::isfinite(voxelSize) || voxelSize <= 0.0)
            return fail(QObject::tr("Voxel size must be finite and greater than zero."));
        if (!(minX < maxX && minY < maxY && minZ < maxZ))
            return fail(QObject::tr("Min range values must be lower than max range values."));

        using Scalar = float;
        using VolumeType = vcg::SimpleVolume<vcg::SimpleVoxel<Scalar>>;
        using WalkerType = vcg::tri::TrivialWalker<VCGMesh, VolumeType>;
        using MarchingCubesType = vcg::tri::MarchingCubes<VCGMesh, WalkerType>;

        const vcg::Box3f range{
            vcg::Point3f(float(minX), float(minY), float(minZ)),
            vcg::Point3f(float(maxX), float(maxY), float(maxZ))
        };
        const vcg::Point3f dims = range.max - range.min;
        // Samples run from min to max inclusive, the step being the voxel size adjusted on
        // each axis so that a whole number of steps spans the range. The volume's own mapping
        // then puts every vertex where its samples were taken, and a closed surface ends
        // exactly on the range's faces.
        vcg::Point3i size;
        vcg::Box3f volumeBox(range.min, range.min);
        for (int a = 0; a < 3; ++a) {
            const int steps = int(std::lround(double(dims[a]) / voxelSize));
            if (steps < 1)
                return fail(QObject::tr("Sampling volume is too small for marching cubes."));
            size[a] = steps + 1;
            volumeBox.max[a] += dims[a] / float(steps) * float(size[a]);
        }

        mu::Parser parser;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        meshlab::filters::defineParserCustomFunctions(parser);
        parser.DefineVar("x", &x);
        parser.DefineVar("y", &y);
        parser.DefineVar("z", &z);
        try {
            parser.SetExpr(expression.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        VolumeType volume;
        volume.Init(size, volumeBox);
        for (int i = 0; i < size[0]; ++i) {
            for (int j = 0; j < size[1]; ++j) {
                for (int k = 0; k < size[2]; ++k) {
                    vcg::Point3f sample;
                    volume.IPiToPf(vcg::Point3i(i, j, k), sample);
                    x = sample[0];
                    y = sample[1];
                    z = sample[2];
                    try {
                        volume.Val(i, j, k) = float(parser.Eval());
                    } catch (mu::Parser::exception_type &e) {
                        return fail(parserErrorString(e));
                    }
                }
            }
        }

        VCGMesh generated;
        QStringList notes;
        if (params.getBool(QStringLiteral("closeBoundary"))) {
            vcg::tri::BuildClosedIsosurface(generated, volume, 0.0f);
            notes << QObject::tr("Closed the surface at the boundary of the sampling range.");
        } else {
            WalkerType walker;
            MarchingCubesType mc(generated, walker);
            walker.BuildMesh<MarchingCubesType>(generated, volume, mc, 0.0f, nullptr);
        }
        if (generated.VN() <= 0 || generated.FN() <= 0)
            return fail(QObject::tr("Implicit surface extraction produced an empty mesh."));

        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(generated);
        vcg::tri::UpdateBounding<VCGMesh>::Box(generated);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(generated);

        const int newIndex = doc.addMesh(
            generated,
            {},
            Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to add generated implicit surface mesh."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices = { newIndex };
        result.infoMessages = {
            QObject::tr("Generated implicit surface '%1' (%2 vertices, %3 faces).")
                .arg(doc.mesh(newIndex).name)
                .arg(doc.mesh(newIndex).mesh.VN())
                .arg(doc.mesh(newIndex).mesh.FN())
        };
        result.infoMessages += notes;
        return result;
    }

    QString meshError;
    const std::optional<CurrentMeshRef> current = currentMesh(doc, meshError);
    if (!current)
        return fail(meshError);

    const int meshIndex = current->index;
    Document::MeshEntry &entry = *current->entry;
    VCGMesh &mesh = entry.mesh;

    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    ParserRuntime runtime;
    setBBoxRuntime(runtime, mesh);

    if (filterId == QString::fromLatin1(kFilterEdgeSelection)) {
        const QString expr = params.getString(QStringLiteral("condSelect"));
        mu::Parser parser;
        setPerEdgeVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(expr.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        for (auto ei = mesh.edge.begin(); ei != mesh.edge.end(); ++ei) {
            if (ei->IsD())
                continue;
            setEdgeRuntime(runtime, ei, mesh);
            bool selected = false;
            try {
                selected = (parser.Eval() != 0.0);
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            if (selected)
                ei->SetS();
            else
                ei->ClearS();
        }
        doc.markMeshSelectionChanged(
            meshIndex,
            QObject::tr("Conditional edge selection on '%1'").arg(entry.name));

        // The counts are appended by the framework, so that this filter says the same thing
        // in the same shape as every other selection filter.
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterEdgeColor)) {
        const QString exprR = params.getString(QStringLiteral("r"));
        const QString exprG = params.getString(QStringLiteral("g"));
        const QString exprB = params.getString(QStringLiteral("b"));
        const QString exprA = params.getString(QStringLiteral("a"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser pr;
        mu::Parser pg;
        mu::Parser pb;
        mu::Parser pa;
        setPerEdgeVariables(pr, runtime, mesh);
        setPerEdgeVariables(pg, runtime, mesh);
        setPerEdgeVariables(pb, runtime, mesh);
        setPerEdgeVariables(pa, runtime, mesh);
        try {
            pr.SetExpr(exprR.toStdString());
            pg.SetExpr(exprG.toStdString());
            pb.SetExpr(exprB.toStdString());
            pa.SetExpr(exprA.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto ei = mesh.edge.begin(); ei != mesh.edge.end(); ++ei) {
            if (ei->IsD())
                continue;
            if (onSelected && !ei->IsS())
                continue;
            setEdgeRuntime(runtime, ei, mesh);
            try {
                ei->C() = vcg::Color4b(
                    clampToByte(pr.Eval()),
                    clampToByte(pg.Eval()),
                    clampToByte(pb.Eval()),
                    clampToByte(pa.Eval()));
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        entry.ioMask |= Mask::IOM_EDGECOLOR;
        doc.markMeshColorChanged(
            meshIndex,
            QObject::tr("Applied per-edge color function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 edges.").arg(processed) };
        result.visualizationHints.push_back({
            meshIndex,
            MeshFilterVisualizationAttribute::EdgeColor
        });
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterEdgeQuality)) {
        const QString exprQ = params.getString(QStringLiteral("q"));
        const bool normalize = params.getBool(QStringLiteral("normalize"));
        const bool mapToColor = params.getBool(QStringLiteral("map"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser parser;
        setPerEdgeVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(exprQ.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto ei = mesh.edge.begin(); ei != mesh.edge.end(); ++ei) {
            if (ei->IsD())
                continue;
            if (onSelected && !ei->IsS())
                continue;
            setEdgeRuntime(runtime, ei, mesh);
            try {
                ei->Q() = float(parser.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }

        // vcglib has no EdgeNormalize, and the one-liner is clearer than adding one.
        if (normalize && onSelected) {
            normalizeSelectedQuality(mesh.edge);
        } else if (normalize) {
            const auto minmax = vcg::tri::Stat<VCGMesh>::ComputePerEdgeQualityMinMax(mesh);
            const float span = minmax.second - minmax.first;
            for (auto ei = mesh.edge.begin(); ei != mesh.edge.end(); ++ei) {
                if (ei->IsD())
                    continue;
                ei->Q() = span > 0.0f ? (ei->Q() - minmax.first) / span : 0.0f;
            }
        }
        entry.ioMask |= Mask::IOM_EDGEQUALITY;
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 edges.").arg(processed) };
        if (mapToColor) {
            if (onSelected)
                rampSelectedQuality(mesh.edge);
            else
                vcg::tri::UpdateColor<VCGMesh>::PerEdgeQualityRamp(mesh);
            entry.ioMask |= Mask::IOM_EDGECOLOR;
            doc.markMeshColorChanged(meshIndex);
            result.visualizationHints.push_back({
                meshIndex,
                MeshFilterVisualizationAttribute::EdgeColor
            });
        } else {
            result.visualizationHints.push_back({
                meshIndex,
                MeshFilterVisualizationAttribute::EdgeQuality
            });
        }
        doc.markMeshScalarChanged(
            meshIndex,
            QObject::tr("Applied per-edge scalar function to '%1'.").arg(entry.name));
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertSelection)) {
        const QString expr = params.getString(QStringLiteral("condSelect"));
        mu::Parser parser;
        setPerVertexVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(expr.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            setVertexRuntime(runtime, vi, mesh);
            bool selected = false;
            try {
                selected = (parser.Eval() != 0.0);
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            if (selected)
                vi->SetS();
            else
                vi->ClearS();
        }
        entry.ioMask |= Mask::IOM_VERTFLAGS;
        doc.markMeshSelectionChanged(
            meshIndex,
            QObject::tr("Conditional vertex selection on '%1'").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterFaceSelection)) {
        const QString expr = params.getString(QStringLiteral("condSelect"));
        mu::Parser parser;
        setPerFaceVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(expr.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            setFaceRuntime(runtime, fi, mesh);
            bool selected = false;
            try {
                selected = (parser.Eval() != 0.0);
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            if (selected)
                fi->SetS();
            else
                fi->ClearS();
        }
        entry.ioMask |= Mask::IOM_FACEFLAGS;
        doc.markMeshSelectionChanged(
            meshIndex,
            QObject::tr("Conditional face selection on '%1'").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterGeomFunc)
        || filterId == QString::fromLatin1(kFilterVertColor)
        || filterId == QString::fromLatin1(kFilterVertNormal)) {
        // The color filter names its channels r, g, b and a, as the face and edge color
        // filters do; the coordinates and normals filters name theirs x, y and z, and have
        // no alpha.
        const bool isColor = filterId == QString::fromLatin1(kFilterVertColor);
        const QString exprX = params.getString(isColor ? QStringLiteral("r") : QStringLiteral("x"));
        const QString exprY = params.getString(isColor ? QStringLiteral("g") : QStringLiteral("y"));
        const QString exprZ = params.getString(isColor ? QStringLiteral("b") : QStringLiteral("z"));
        const QString exprA = isColor ? params.getString(QStringLiteral("a")) : QStringLiteral("255");
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser px;
        mu::Parser py;
        mu::Parser pz;
        mu::Parser pa;
        setPerVertexVariables(px, runtime, mesh);
        setPerVertexVariables(py, runtime, mesh);
        setPerVertexVariables(pz, runtime, mesh);
        setPerVertexVariables(pa, runtime, mesh);
        try {
            px.SetExpr(exprX.toStdString());
            py.SetExpr(exprY.toStdString());
            pz.SetExpr(exprZ.toStdString());
            pa.SetExpr(exprA.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            if (onSelected && !vi->IsS())
                continue;
            setVertexRuntime(runtime, vi, mesh);
            double outX = 0.0;
            double outY = 0.0;
            double outZ = 0.0;
            double outA = 255.0;
            try {
                outX = px.Eval();
                outY = py.Eval();
                outZ = pz.Eval();
                outA = pa.Eval();
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }

            if (filterId == QString::fromLatin1(kFilterGeomFunc)) {
                vi->P() = vcg::Point3f(float(outX), float(outY), float(outZ));
            } else if (filterId == QString::fromLatin1(kFilterVertNormal)) {
                vi->N() = vcg::Point3f(float(outX), float(outY), float(outZ));
            } else {
                vi->C() = vcg::Color4b(clampToByte(outX), clampToByte(outY), clampToByte(outZ), clampToByte(outA));
            }
            ++processed;
        }

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 vertices.").arg(processed) };

        if (filterId == QString::fromLatin1(kFilterGeomFunc)) {
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Applied per-vertex geometric function to '%1'.").arg(entry.name));
        } else if (filterId == QString::fromLatin1(kFilterVertNormal)) {
            entry.ioMask |= Mask::IOM_VERTNORMAL;
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Applied per-vertex normal function to '%1'.").arg(entry.name));
        } else {
            entry.ioMask |= Mask::IOM_VERTCOLOR;
            doc.markMeshColorChanged(
                meshIndex,
                QObject::tr("Applied per-vertex color function to '%1'.").arg(entry.name));
        }
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertQuality)) {
        const QString exprQ = params.getString(QStringLiteral("q"));
        const bool normalize = params.getBool(QStringLiteral("normalize"));
        const bool mapToColor = params.getBool(QStringLiteral("map"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser parser;
        setPerVertexVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(exprQ.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            if (onSelected && !vi->IsS())
                continue;
            setVertexRuntime(runtime, vi, mesh);
            try {
                vi->Q() = float(parser.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        if (normalize && onSelected)
            normalizeSelectedQuality(mesh.vert);
        else if (normalize)
            vcg::tri::UpdateQuality<VCGMesh>::VertexNormalize(mesh);
        if (mapToColor) {
            if (onSelected)
                rampSelectedQuality(mesh.vert);
            else
                vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(mesh);
            entry.ioMask |= Mask::IOM_VERTCOLOR;
            doc.markMeshColorChanged(meshIndex);
        }
        entry.ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshScalarChanged(
            meshIndex,
            QObject::tr("Applied per-vertex quality function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 vertices.").arg(processed) };
        result.visualizationHints.push_back({
            meshIndex,
            MeshFilterVisualizationAttribute::VertexQuality
        });
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertTex)) {
        const QString exprU = params.getString(QStringLiteral("u"));
        const QString exprV = params.getString(QStringLiteral("v"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser parserU;
        mu::Parser parserV;
        setPerVertexVariables(parserU, runtime, mesh);
        setPerVertexVariables(parserV, runtime, mesh);
        try {
            parserU.SetExpr(exprU.toStdString());
            parserV.SetExpr(exprV.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            if (onSelected && !vi->IsS())
                continue;
            setVertexRuntime(runtime, vi, mesh);
            try {
                vi->T().U() = float(parserU.Eval());
                vi->T().V() = float(parserV.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        entry.ioMask |= Mask::IOM_VERTTEXCOORD;
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Applied per-vertex texture function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 vertices.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterWedgeTex)) {
        const QString exprU0 = params.getString(QStringLiteral("u0"));
        const QString exprV0 = params.getString(QStringLiteral("v0"));
        const QString exprU1 = params.getString(QStringLiteral("u1"));
        const QString exprV1 = params.getString(QStringLiteral("v1"));
        const QString exprU2 = params.getString(QStringLiteral("u2"));
        const QString exprV2 = params.getString(QStringLiteral("v2"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser pu0;
        mu::Parser pv0;
        mu::Parser pu1;
        mu::Parser pv1;
        mu::Parser pu2;
        mu::Parser pv2;
        setPerFaceVariables(pu0, runtime, mesh);
        setPerFaceVariables(pv0, runtime, mesh);
        setPerFaceVariables(pu1, runtime, mesh);
        setPerFaceVariables(pv1, runtime, mesh);
        setPerFaceVariables(pu2, runtime, mesh);
        setPerFaceVariables(pv2, runtime, mesh);
        try {
            pu0.SetExpr(exprU0.toStdString());
            pv0.SetExpr(exprV0.toStdString());
            pu1.SetExpr(exprU1.toStdString());
            pv1.SetExpr(exprV1.toStdString());
            pu2.SetExpr(exprU2.toStdString());
            pv2.SetExpr(exprV2.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (onSelected && !fi->IsS())
                continue;
            setFaceRuntime(runtime, fi, mesh);
            try {
                fi->WT(0).U() = float(pu0.Eval());
                fi->WT(0).V() = float(pv0.Eval());
                fi->WT(1).U() = float(pu1.Eval());
                fi->WT(1).V() = float(pv1.Eval());
                fi->WT(2).U() = float(pu2.Eval());
                fi->WT(2).V() = float(pv2.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            // A face given UVs reads a texture: one that had none (index -1) gets the first.
            if (fi->WT(0).N() < 0)
                for (int k = 0; k < 3; ++k)
                    fi->WT(k).N() = 0;
            ++processed;
        }
        entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Applied per-wedge texture function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
        result.visualizationHints.push_back({
            meshIndex,
            MeshFilterVisualizationAttribute::FaceQuality
        });
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterFaceNormal)) {
        const QString exprX = params.getString(QStringLiteral("x"));
        const QString exprY = params.getString(QStringLiteral("y"));
        const QString exprZ = params.getString(QStringLiteral("z"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser px;
        mu::Parser py;
        mu::Parser pz;
        setPerFaceVariables(px, runtime, mesh);
        setPerFaceVariables(py, runtime, mesh);
        setPerFaceVariables(pz, runtime, mesh);
        try {
            px.SetExpr(exprX.toStdString());
            py.SetExpr(exprY.toStdString());
            pz.SetExpr(exprZ.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (onSelected && !fi->IsS())
                continue;
            setFaceRuntime(runtime, fi, mesh);
            try {
                fi->N() = vcg::Point3f(float(px.Eval()), float(py.Eval()), float(pz.Eval()));
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        entry.ioMask |= Mask::IOM_FACENORMAL;
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Applied per-face normal function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterFaceColor)) {
        const QString exprR = params.getString(QStringLiteral("r"));
        const QString exprG = params.getString(QStringLiteral("g"));
        const QString exprB = params.getString(QStringLiteral("b"));
        const QString exprA = params.getString(QStringLiteral("a"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser pr;
        mu::Parser pg;
        mu::Parser pb;
        mu::Parser pa;
        setPerFaceVariables(pr, runtime, mesh);
        setPerFaceVariables(pg, runtime, mesh);
        setPerFaceVariables(pb, runtime, mesh);
        setPerFaceVariables(pa, runtime, mesh);
        try {
            pr.SetExpr(exprR.toStdString());
            pg.SetExpr(exprG.toStdString());
            pb.SetExpr(exprB.toStdString());
            pa.SetExpr(exprA.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (onSelected && !fi->IsS())
                continue;
            setFaceRuntime(runtime, fi, mesh);
            try {
                fi->C() = vcg::Color4b(
                    clampToByte(pr.Eval()),
                    clampToByte(pg.Eval()),
                    clampToByte(pb.Eval()),
                    clampToByte(pa.Eval()));
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        entry.ioMask |= Mask::IOM_FACECOLOR;
        doc.markMeshColorChanged(
            meshIndex,
            QObject::tr("Applied per-face color function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterFaceQuality)) {
        const QString exprQ = params.getString(QStringLiteral("q"));
        const bool normalize = params.getBool(QStringLiteral("normalize"));
        const bool mapToColor = params.getBool(QStringLiteral("map"));
        const bool onSelected = params.getBool(QStringLiteral("selectedOnly"));

        mu::Parser parser;
        setPerFaceVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(exprQ.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (onSelected && !fi->IsS())
                continue;
            setFaceRuntime(runtime, fi, mesh);
            try {
                fi->Q() = float(parser.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        if (normalize && onSelected)
            normalizeSelectedQuality(mesh.face);
        else if (normalize)
            vcg::tri::UpdateQuality<VCGMesh>::FaceNormalize(mesh);
        if (mapToColor) {
            if (onSelected)
                rampSelectedQuality(mesh.face);
            else
                vcg::tri::UpdateColor<VCGMesh>::PerFaceQualityRamp(mesh);
            entry.ioMask |= Mask::IOM_FACECOLOR;
            doc.markMeshColorChanged(meshIndex);
        }
        entry.ioMask |= Mask::IOM_FACEQUALITY;
        doc.markMeshScalarChanged(
            meshIndex,
            QObject::tr("Applied per-face quality function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
        return result;
    }

    for (const DefineCustomAttributeFilter &define : kDefineCustomAttributeFilters) {
        if (filterId != QString::fromLatin1(define.id))
            continue;
        const QString name = params.getString(QStringLiteral("name")).trimmed();
        const QStringList expressions = define.point
            ? QStringList{ params.getString(QStringLiteral("x_expr")),
                           params.getString(QStringLiteral("y_expr")),
                           params.getString(QStringLiteral("z_expr")) }
            : QStringList{ params.getString(QStringLiteral("expr")) };
        int processed = 0;
        const QString error = define.define(mesh, runtime, name, expressions, processed);
        if (!error.isEmpty())
            return fail(error);
        doc.markMeshMaterialChanged(
            meshIndex,
            QObject::tr("Defined custom %1 attribute '%2' on '%3'.")
                .arg(QObject::tr(define.description), name, entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Processed %1 %2.").arg(processed).arg(QObject::tr(define.elements))
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterRefine)) {
        const std::string condSelect =
            params.getString(QStringLiteral("condSelect"))
                .toStdString();
        const std::string exprX =
            params.getString(QStringLiteral("x")).toStdString();
        const std::string exprY =
            params.getString(QStringLiteral("y")).toStdString();
        const std::string exprZ =
            params.getString(QStringLiteral("z")).toStdString();

        bool midpointError = false;
        bool edgeError = false;
        std::string message;
        meshlab::filters::MidPointCustom<VCGMesh> midpoint(mesh, exprX, exprY, exprZ, midpointError, message);
        meshlab::filters::CustomEdge<VCGMesh> edge(condSelect, edgeError, message);
        if (midpointError || edgeError)
            return fail(QString::fromStdString(message));

        vcg::tri::RefineE<
            VCGMesh,
            meshlab::filters::MidPointCustom<VCGMesh>,
            meshlab::filters::CustomEdge<VCGMesh>>(mesh, midpoint, edge, false, nullptr);
        vcg::tri::UpdateFlags<VCGMesh>::VertexClearV(mesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Applied user-defined refine to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Refined mesh now has %1 vertices and %2 faces.")
                .arg(mesh.VN())
                .arg(mesh.FN())
        };
        return result;
    }

    return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerExpressionFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<ExpressionFilterPlugin>());
}

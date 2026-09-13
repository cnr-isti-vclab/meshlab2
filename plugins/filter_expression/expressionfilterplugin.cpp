#include "expressionfilterplugin.h"

#include "document.h"
#include "filter_refine.h"
#include "meshfilterpluginmanager.h"
#include <muParser.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/create/marching_cubes.h>
#include <vcg/complex/algorithms/create/mc_trivial_walker.h>
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
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
constexpr QLatin1StringView kFilterVertSelection("select_vertices_by_expression");
constexpr QLatin1StringView kFilterFaceSelection("select_faces_by_expression");
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
    return QString::fromStdString(qmeshlab::filters::parserStringToStd(e.GetMsg()));
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

    std::vector<VCGMesh::PerVertexAttributeHandle<float>> vScalarHandles;
    std::vector<double> vScalarValues;
    std::vector<VCGMesh::PerVertexAttributeHandle<vcg::Point3f>> vPointHandles;
    std::vector<double> vPointValues;

    std::vector<VCGMesh::PerFaceAttributeHandle<float>> fScalarHandles;
    std::vector<double> fScalarValues;
    std::vector<VCGMesh::PerFaceAttributeHandle<vcg::Point3f>> fPointHandles;
    std::vector<double> fPointValues;
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

void bindVertexCustomAttributes(mu::Parser &parser, ParserRuntime &runtime, VCGMesh &mesh)
{
    runtime.vScalarHandles.clear();
    runtime.vScalarValues.clear();
    runtime.vPointHandles.clear();
    runtime.vPointValues.clear();

    std::vector<std::string> names;
    vcg::tri::Allocator<VCGMesh>::GetAllPerVertexAttribute<float>(mesh, names);
    runtime.vScalarHandles.reserve(names.size());
    runtime.vScalarValues.reserve(names.size());
    for (const std::string &name : names) {
        if (!isValidIdentifier(name) || reservedVariables().count(name) > 0)
            continue;
        const auto handle = vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<float>(mesh, name);
        if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
            continue;
        runtime.vScalarHandles.push_back(handle);
        runtime.vScalarValues.push_back(0.0);
        parser.DefineVar(name, &runtime.vScalarValues.back());
    }

    names.clear();
    vcg::tri::Allocator<VCGMesh>::GetAllPerVertexAttribute<vcg::Point3f>(mesh, names);
    runtime.vPointHandles.reserve(names.size());
    runtime.vPointValues.reserve(names.size() * 3);
    for (const std::string &name : names) {
        if (!isValidIdentifier(name))
            continue;
        const std::array<std::string, 3> components = {
            name + "_x",
            name + "_y",
            name + "_z"
        };
        if (reservedVariables().count(components[0]) > 0
            || reservedVariables().count(components[1]) > 0
            || reservedVariables().count(components[2]) > 0) {
            continue;
        }
        const auto handle =
            vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<vcg::Point3f>(mesh, name);
        if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
            continue;
        runtime.vPointHandles.push_back(handle);
        for (int c = 0; c < 3; ++c) {
            runtime.vPointValues.push_back(0.0);
            parser.DefineVar(components[size_t(c)], &runtime.vPointValues.back());
        }
    }
}

void bindFaceCustomAttributes(mu::Parser &parser, ParserRuntime &runtime, VCGMesh &mesh)
{
    runtime.fScalarHandles.clear();
    runtime.fScalarValues.clear();
    runtime.fPointHandles.clear();
    runtime.fPointValues.clear();

    std::vector<std::string> names;
    vcg::tri::Allocator<VCGMesh>::GetAllPerFaceAttribute<float>(mesh, names);
    runtime.fScalarHandles.reserve(names.size());
    runtime.fScalarValues.reserve(names.size());
    for (const std::string &name : names) {
        if (!isValidIdentifier(name) || reservedVariables().count(name) > 0)
            continue;
        const auto handle = vcg::tri::Allocator<VCGMesh>::GetPerFaceAttribute<float>(mesh, name);
        if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
            continue;
        runtime.fScalarHandles.push_back(handle);
        runtime.fScalarValues.push_back(0.0);
        parser.DefineVar(name, &runtime.fScalarValues.back());
    }

    names.clear();
    vcg::tri::Allocator<VCGMesh>::GetAllPerFaceAttribute<vcg::Point3f>(mesh, names);
    runtime.fPointHandles.reserve(names.size());
    runtime.fPointValues.reserve(names.size() * 3);
    for (const std::string &name : names) {
        if (!isValidIdentifier(name))
            continue;
        const std::array<std::string, 3> components = {
            name + "_x",
            name + "_y",
            name + "_z"
        };
        if (reservedVariables().count(components[0]) > 0
            || reservedVariables().count(components[1]) > 0
            || reservedVariables().count(components[2]) > 0) {
            continue;
        }
        const auto handle = vcg::tri::Allocator<VCGMesh>::GetPerFaceAttribute<vcg::Point3f>(mesh, name);
        if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
            continue;
        runtime.fPointHandles.push_back(handle);
        for (int c = 0; c < 3; ++c) {
            runtime.fPointValues.push_back(0.0);
            parser.DefineVar(components[size_t(c)], &runtime.fPointValues.back());
        }
    }
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
    qmeshlab::filters::defineParserCustomFunctions(parser);
    bindVertexCustomAttributes(parser, runtime, mesh);
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
    qmeshlab::filters::defineParserCustomFunctions(parser);
    bindFaceCustomAttributes(parser, runtime, mesh);
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

    for (size_t i = 0; i < runtime.vScalarHandles.size(); ++i)
        runtime.vScalarValues[i] = runtime.vScalarHandles[i][vi];
    for (size_t i = 0; i < runtime.vPointHandles.size(); ++i) {
        const vcg::Point3f p = runtime.vPointHandles[i][vi];
        runtime.vPointValues[i * 3 + 0] = p.X();
        runtime.vPointValues[i * 3 + 1] = p.Y();
        runtime.vPointValues[i * 3 + 2] = p.Z();
    }
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

    for (size_t i = 0; i < runtime.fScalarHandles.size(); ++i)
        runtime.fScalarValues[i] = runtime.fScalarHandles[i][fi];
    for (size_t i = 0; i < runtime.fPointHandles.size(); ++i) {
        const vcg::Point3f p = runtime.fPointHandles[i][fi];
        runtime.fPointValues[i * 3 + 0] = p.X();
        runtime.fPointValues[i * 3 + 1] = p.Y();
        runtime.fPointValues[i * 3 + 2] = p.Z();
    }
}

bool ensureVertexSelectionReady(VCGMesh &mesh, QString &error)
{
    const size_t selectedVertices = vcg::tri::UpdateSelection<VCGMesh>::VertexCount(mesh);
    const size_t selectedFaces = vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh);
    if (selectedVertices == 0 && selectedFaces == 0) {
        error = QObject::tr("Cannot apply only on selection: there is no selection.");
        return false;
    }
    if (selectedVertices == 0 && selectedFaces > 0) {
        vcg::tri::UpdateSelection<VCGMesh>::VertexClear(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceLoose(mesh);
    }
    return true;
}

bool ensureFaceSelectionReady(VCGMesh &mesh, QString &error)
{
    const size_t selectedFaces = vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh);
    if (selectedFaces == 0) {
        error = QObject::tr("Cannot apply only on selection: there is no face selection.");
        return false;
    }
    return true;
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
    qmeshlab::filters::seedParserRandom(params.getRandomSeed().value);

    if (filterId == QString::fromLatin1(kFilterGrid)) {
        const int w = params.getInt(QStringLiteral("numVertX"));
        const int h = params.getInt(QStringLiteral("numVertY"));
        const double sx = params.getDouble(QStringLiteral("absScaleX"));
        const double sy = params.getDouble(QStringLiteral("absScaleY"));
        const bool center = params.getBool(QStringLiteral("center"));
        if (w <= 1 || h <= 1)
            return fail(QObject::tr("Grid vertex counts must be greater than 1."));
        if (!std::isfinite(sx) || !std::isfinite(sy) || sx <= 0.0 || sy <= 0.0)
            return fail(QObject::tr("Grid scale values must be finite and greater than zero."));

        VCGMesh generated;
        vcg::tri::Grid(generated, w, h, float(sx), float(sy));
        if (center) {
            const float halfW = float(w - 1) * 0.5f;
            const float halfH = float(h - 1) * 0.5f;
            const float stepX = float(sx) / float(w);
            const float stepY = float(sy) / float(h);
            for (auto vi = generated.vert.begin(); vi != generated.vert.end(); ++vi) {
                vi->P()[0] -= stepX * halfW;
                vi->P()[1] -= stepY * halfH;
            }
        }
        vcg::Matrix44f flip;
        flip.SetScale(-1.0f, 1.0f, -1.0f);
        vcg::tri::UpdatePosition<VCGMesh>::Matrix(generated, flip, false);
        vcg::tri::UpdateBounding<VCGMesh>::Box(generated);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(generated);

        const int newIndex = doc.addMesh(
            generated,
            QObject::tr("Grid"),
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
        const vcg::Point3i size = vcg::Point3i::Construct(dims * float(1.0 / voxelSize));
        if (size[0] < 2 || size[1] < 2 || size[2] < 2)
            return fail(QObject::tr("Sampling volume is too small for marching cubes."));

        mu::Parser parser;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        qmeshlab::filters::defineParserCustomFunctions(parser);
        parser.DefineVar("x", &x);
        parser.DefineVar("y", &y);
        parser.DefineVar("z", &z);
        try {
            parser.SetExpr(expression.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        VolumeType volume;
        volume.Init(size, range);
        for (int i = 0; i < size[0]; ++i) {
            for (int j = 0; j < size[1]; ++j) {
                for (int k = 0; k < size[2]; ++k) {
                    x = minX + voxelSize * double(i);
                    y = minY + voxelSize * double(j);
                    z = minZ + voxelSize * double(k);
                    try {
                        volume.Val(i, j, k) = float(parser.Eval());
                    } catch (mu::Parser::exception_type &e) {
                        return fail(parserErrorString(e));
                    }
                }
            }
        }

        VCGMesh generated;
        WalkerType walker;
        MarchingCubesType mc(generated, walker);
        walker.BuildMesh<MarchingCubesType>(generated, volume, mc, 0.0f, nullptr);
        if (generated.VN() <= 0 || generated.FN() <= 0)
            return fail(QObject::tr("Implicit surface extraction produced an empty mesh."));

        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(generated);
        vcg::tri::UpdateBounding<VCGMesh>::Box(generated);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(generated);

        const int newIndex = doc.addMesh(
            generated,
            QObject::tr("Implicit Surface"),
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

    if (filterId == QString::fromLatin1(kFilterVertSelection)) {
        const QString expr = params.getString(QStringLiteral("condSelect"));
        mu::Parser parser;
        setPerVertexVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(expr.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int selectedCount = 0;
        int totalCount = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            ++totalCount;
            setVertexRuntime(runtime, vi, mesh);
            bool selected = false;
            try {
                selected = (parser.Eval() != 0.0);
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            if (selected) {
                vi->SetS();
                ++selectedCount;
            } else {
                vi->ClearS();
            }
        }
        entry.ioMask |= Mask::IOM_VERTFLAGS;
        doc.markMeshSelectionChanged(
            meshIndex,
            QObject::tr("Conditional vertex selection on '%1': selected %2 / %3 vertices.")
                .arg(entry.name)
                .arg(selectedCount)
                .arg(totalCount));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Selected %1 / %2 vertices.")
                .arg(selectedCount)
                .arg(totalCount)
        };
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

        int selectedCount = 0;
        int totalCount = 0;
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            ++totalCount;
            setFaceRuntime(runtime, fi, mesh);
            bool selected = false;
            try {
                selected = (parser.Eval() != 0.0);
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            if (selected) {
                fi->SetS();
                ++selectedCount;
            } else {
                fi->ClearS();
            }
        }
        entry.ioMask |= Mask::IOM_FACEFLAGS;
        doc.markMeshSelectionChanged(
            meshIndex,
            QObject::tr("Conditional face selection on '%1': selected %2 / %3 faces.")
                .arg(entry.name)
                .arg(selectedCount)
                .arg(totalCount));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Selected %1 / %2 faces.")
                .arg(selectedCount)
                .arg(totalCount)
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterGeomFunc)
        || filterId == QString::fromLatin1(kFilterVertColor)
        || filterId == QString::fromLatin1(kFilterVertNormal)) {
        const QString exprX = params.getString(QStringLiteral("x"));
        const QString exprY = params.getString(QStringLiteral("y"));
        const QString exprZ = params.getString(QStringLiteral("z"));
        const QString exprA = params.getString(QStringLiteral("a"));
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureVertexSelectionReady(mesh, meshError))
            return fail(meshError);

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
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Applied per-vertex color function to '%1'.").arg(entry.name));
        }
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertQuality)) {
        const QString exprQ = params.getString(QStringLiteral("q"));
        const bool normalize = params.getBool(QStringLiteral("normalize"));
        const bool mapToColor = params.getBool(QStringLiteral("map"));
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureVertexSelectionReady(mesh, meshError))
            return fail(meshError);

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
        if (normalize)
            vcg::tri::UpdateQuality<VCGMesh>::VertexNormalize(mesh);
        if (mapToColor) {
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(mesh);
            entry.ioMask |= Mask::IOM_VERTCOLOR;
        }
        entry.ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(
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
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureVertexSelectionReady(mesh, meshError))
            return fail(meshError);

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
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureFaceSelectionReady(mesh, meshError))
            return fail(meshError);

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
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureFaceSelectionReady(mesh, meshError))
            return fail(meshError);

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
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureFaceSelectionReady(mesh, meshError))
            return fail(meshError);

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
        doc.markMeshGeometryChanged(
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
        const bool onSelected = params.getBool(QStringLiteral("onselected"));
        if (onSelected && !ensureFaceSelectionReady(mesh, meshError))
            return fail(meshError);

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
        if (normalize)
            vcg::tri::UpdateQuality<VCGMesh>::FaceNormalize(mesh);
        if (mapToColor) {
            vcg::tri::UpdateColor<VCGMesh>::PerFaceQualityRamp(mesh);
            entry.ioMask |= Mask::IOM_FACECOLOR;
        }
        entry.ioMask |= Mask::IOM_FACEQUALITY;
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Applied per-face quality function to '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDefVertScalar)) {
        const QString name = params.getString(QStringLiteral("name")).trimmed();
        const QString expr = params.getString(QStringLiteral("expr"));
        if (!checkCustomAttributeName(name, meshError))
            return fail(meshError);

        const std::string stdName = name.toStdString();
        VCGMesh::PerVertexAttributeHandle<float> handle;
        if (vcg::tri::HasPerVertexAttribute(mesh, stdName)) {
            handle = vcg::tri::Allocator<VCGMesh>::FindPerVertexAttribute<float>(mesh, stdName);
            if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
                return fail(QObject::tr("Attribute '%1' already exists with a different type.").arg(name));
        } else {
            handle = vcg::tri::Allocator<VCGMesh>::AddPerVertexAttribute<float>(mesh, stdName);
        }

        mu::Parser parser;
        setPerVertexVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(expr.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            setVertexRuntime(runtime, vi, mesh);
            try {
                handle[vi] = float(parser.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        doc.markMeshMaterialChanged(
            meshIndex,
            QObject::tr("Defined custom per-vertex scalar attribute '%1' on '%2'.")
                .arg(name, entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 vertices.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDefFaceScalar)) {
        const QString name = params.getString(QStringLiteral("name")).trimmed();
        const QString expr = params.getString(QStringLiteral("expr"));
        if (!checkCustomAttributeName(name, meshError))
            return fail(meshError);

        const std::string stdName = name.toStdString();
        VCGMesh::PerFaceAttributeHandle<float> handle;
        if (vcg::tri::HasPerFaceAttribute(mesh, stdName)) {
            handle = vcg::tri::Allocator<VCGMesh>::FindPerFaceAttribute<float>(mesh, stdName);
            if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
                return fail(QObject::tr("Attribute '%1' already exists with a different type.").arg(name));
        } else {
            handle = vcg::tri::Allocator<VCGMesh>::AddPerFaceAttribute<float>(mesh, stdName);
        }

        mu::Parser parser;
        setPerFaceVariables(parser, runtime, mesh);
        try {
            parser.SetExpr(expr.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            setFaceRuntime(runtime, fi, mesh);
            try {
                handle[fi] = float(parser.Eval());
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        doc.markMeshMaterialChanged(
            meshIndex,
            QObject::tr("Defined custom per-face scalar attribute '%1' on '%2'.")
                .arg(name, entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDefVertPoint)) {
        const QString name = params.getString(QStringLiteral("name")).trimmed();
        const QString exprX = params.getString(QStringLiteral("x_expr"));
        const QString exprY = params.getString(QStringLiteral("y_expr"));
        const QString exprZ = params.getString(QStringLiteral("z_expr"));
        if (!checkCustomAttributeName(name, meshError))
            return fail(meshError);

        const std::string stdName = name.toStdString();
        VCGMesh::PerVertexAttributeHandle<vcg::Point3f> handle;
        if (vcg::tri::HasPerVertexAttribute(mesh, stdName)) {
            handle = vcg::tri::Allocator<VCGMesh>::FindPerVertexAttribute<vcg::Point3f>(mesh, stdName);
            if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
                return fail(QObject::tr("Attribute '%1' already exists with a different type.").arg(name));
        } else {
            handle = vcg::tri::Allocator<VCGMesh>::AddPerVertexAttribute<vcg::Point3f>(mesh, stdName);
        }

        mu::Parser px;
        mu::Parser py;
        mu::Parser pz;
        setPerVertexVariables(px, runtime, mesh);
        setPerVertexVariables(py, runtime, mesh);
        setPerVertexVariables(pz, runtime, mesh);
        try {
            px.SetExpr(exprX.toStdString());
            py.SetExpr(exprY.toStdString());
            pz.SetExpr(exprZ.toStdString());
        } catch (mu::Parser::exception_type &e) {
            return fail(parserErrorString(e));
        }

        int processed = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            setVertexRuntime(runtime, vi, mesh);
            try {
                handle[vi] = vcg::Point3f(float(px.Eval()), float(py.Eval()), float(pz.Eval()));
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        doc.markMeshMaterialChanged(
            meshIndex,
            QObject::tr("Defined custom per-vertex point attribute '%1' on '%2'.")
                .arg(name, entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 vertices.").arg(processed) };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDefFacePoint)) {
        const QString name = params.getString(QStringLiteral("name")).trimmed();
        const QString exprX = params.getString(QStringLiteral("x_expr"));
        const QString exprY = params.getString(QStringLiteral("y_expr"));
        const QString exprZ = params.getString(QStringLiteral("z_expr"));
        if (!checkCustomAttributeName(name, meshError))
            return fail(meshError);

        const std::string stdName = name.toStdString();
        VCGMesh::PerFaceAttributeHandle<vcg::Point3f> handle;
        if (vcg::tri::HasPerFaceAttribute(mesh, stdName)) {
            handle = vcg::tri::Allocator<VCGMesh>::FindPerFaceAttribute<vcg::Point3f>(mesh, stdName);
            if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle))
                return fail(QObject::tr("Attribute '%1' already exists with a different type.").arg(name));
        } else {
            handle = vcg::tri::Allocator<VCGMesh>::AddPerFaceAttribute<vcg::Point3f>(mesh, stdName);
        }

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
            setFaceRuntime(runtime, fi, mesh);
            try {
                handle[fi] = vcg::Point3f(float(px.Eval()), float(py.Eval()), float(pz.Eval()));
            } catch (mu::Parser::exception_type &e) {
                return fail(parserErrorString(e));
            }
            ++processed;
        }
        doc.markMeshMaterialChanged(
            meshIndex,
            QObject::tr("Defined custom per-face point attribute '%1' on '%2'.")
                .arg(name, entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = { QObject::tr("Processed %1 faces.").arg(processed) };
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
        qmeshlab::filters::MidPointCustom<VCGMesh> midpoint(mesh, exprX, exprY, exprZ, midpointError, message);
        qmeshlab::filters::CustomEdge<VCGMesh> edge(condSelect, edgeError, message);
        if (midpointError || edgeError)
            return fail(QString::fromStdString(message));

        vcg::tri::RefineE<
            VCGMesh,
            qmeshlab::filters::MidPointCustom<VCGMesh>,
            qmeshlab::filters::CustomEdge<VCGMesh>>(mesh, midpoint, edge, false, nullptr);
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

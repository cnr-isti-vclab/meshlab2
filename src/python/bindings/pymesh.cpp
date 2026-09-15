#include "pymesh.h"
#include "document.h"
#include "vcgmesh.h"

#include <QString>
#include <stdexcept>
#include <cstdint>

#include <nanobind/stl/string.h>

namespace nb = nanobind;

namespace {

// ---------------------------------------------------------------------------
// Nx3 float64 array (positions, normals, curvature dirs)
// ---------------------------------------------------------------------------
template <typename Func>
nb::ndarray<nb::numpy, double, nb::shape<-1, 3>> makeNx3(int n, Func getter)
{
    double *data = new double[n * 3];
    for (int i = 0; i < n; ++i) {
        const auto &p = getter(i);
        data[i * 3 + 0] = static_cast<double>(p[0]);
        data[i * 3 + 1] = static_cast<double>(p[1]);
        data[i * 3 + 2] = static_cast<double>(p[2]);
    }
    nb::capsule owner(data, [](void *p) noexcept { delete[] static_cast<double *>(p); });
    const size_t shape[2] = {size_t(n), 3};
    return nb::ndarray<nb::numpy, double, nb::shape<-1, 3>>(data, 2, shape, owner);
}

// ---------------------------------------------------------------------------
// Nx4 float64 color array (converts uint8 [0-255] to float64 [0-1])
// ---------------------------------------------------------------------------
template <typename Func>
nb::ndarray<nb::numpy, double, nb::shape<-1, 4>> makeNx4Color(int n, Func getter)
{
    double *data = new double[n * 4];
    for (int i = 0; i < n; ++i) {
        const auto &c = getter(i);
        data[i * 4 + 0] = static_cast<double>(c[0]) / 255.0;
        data[i * 4 + 1] = static_cast<double>(c[1]) / 255.0;
        data[i * 4 + 2] = static_cast<double>(c[2]) / 255.0;
        data[i * 4 + 3] = static_cast<double>(c[3]) / 255.0;
    }
    nb::capsule owner(data, [](void *p) noexcept { delete[] static_cast<double *>(p); });
    const size_t shape[2] = {size_t(n), 4};
    return nb::ndarray<nb::numpy, double, nb::shape<-1, 4>>(data, 2, shape, owner);
}

// ---------------------------------------------------------------------------
// Mx3 int32 array (face vertex indices)
// ---------------------------------------------------------------------------
nb::ndarray<nb::numpy, int32_t, nb::shape<-1, 3>> makeFaceArray(int m, const VCGMesh &cm)
{
    int32_t *data = new int32_t[m * 3];
    const VCGVertex *vertBase = cm.vert.data();
    for (int i = 0; i < m; ++i) {
        const auto &f = cm.face[i];
        data[i * 3 + 0] = static_cast<int32_t>(f.V(0) - vertBase);
        data[i * 3 + 1] = static_cast<int32_t>(f.V(1) - vertBase);
        data[i * 3 + 2] = static_cast<int32_t>(f.V(2) - vertBase);
    }
    nb::capsule owner(data, [](void *p) noexcept { delete[] static_cast<int32_t *>(p); });
    const size_t shape[2] = {size_t(m), 3};
    return nb::ndarray<nb::numpy, int32_t, nb::shape<-1, 3>>(data, 2, shape, owner);
}

// ---------------------------------------------------------------------------
// N float64 array (scalar/quality values)
// ---------------------------------------------------------------------------
template <typename Func>
nb::ndarray<nb::numpy, double, nb::shape<-1>> makeScalarArray(int n, Func getter)
{
    double *data = new double[n];
    for (int i = 0; i < n; ++i)
        data[i] = static_cast<double>(getter(i));
    nb::capsule owner(data, [](void *p) noexcept { delete[] static_cast<double *>(p); });
    const size_t shape[1] = {size_t(n)};
    return nb::ndarray<nb::numpy, double, nb::shape<-1>>(data, 1, shape, owner);
}

// ---------------------------------------------------------------------------
// Nx2 float64 array (vertex texcoords: u, v)
// ---------------------------------------------------------------------------
template <typename Func>
nb::ndarray<nb::numpy, double, nb::shape<-1, 2>> makeUVArray(int n, Func getter)
{
    double *data = new double[n * 2];
    for (int i = 0; i < n; ++i) {
        const auto &tc = getter(i);
        data[i * 2 + 0] = static_cast<double>(tc.U());
        data[i * 2 + 1] = static_cast<double>(tc.V());
    }
    nb::capsule owner(data, [](void *p) noexcept { delete[] static_cast<double *>(p); });
    const size_t shape[2] = {size_t(n), 2};
    return nb::ndarray<nb::numpy, double, nb::shape<-1, 2>>(data, 2, shape, owner);
}

// ---------------------------------------------------------------------------
// Mx3x2 float64 array (wedge texcoords: 3 corners x 2 UV per face)
// ---------------------------------------------------------------------------
nb::ndarray<nb::numpy, double, nb::shape<-1, 3, 2>> makeWedgeArray(int m, const VCGMesh &cm)
{
    double *data = new double[m * 3 * 2];
    for (int i = 0; i < m; ++i) {
        const auto &f = cm.face[i];
        double *row = data + i * 6;
        row[0] = static_cast<double>(f.WT(0).U());
        row[1] = static_cast<double>(f.WT(0).V());
        row[2] = static_cast<double>(f.WT(1).U());
        row[3] = static_cast<double>(f.WT(1).V());
        row[4] = static_cast<double>(f.WT(2).U());
        row[5] = static_cast<double>(f.WT(2).V());
    }
    nb::capsule owner(data, [](void *p) noexcept { delete[] static_cast<double *>(p); });
    const size_t shape[3] = {size_t(m), 3, 2};
    return nb::ndarray<nb::numpy, double, nb::shape<-1, 3, 2>>(data, 3, shape, owner);
}

} // namespace

// ---------------------------------------------------------------------------
// PyMesh
// ---------------------------------------------------------------------------

PyMesh::PyMesh(Document *doc, int index)
    : m_doc(doc)
    , m_index(index)
{
}

// Document's persistent MeshEntry::meshId -- not the layer's position. This is the
// handle the render-state JSON keys mesh_render_modes by; index() is the position.
std::uint64_t PyMesh::id() const { return m_doc->mesh(m_index).meshId; }

int PyMesh::index() const { return m_index; }

bool PyMesh::isVisible() const { return m_doc->mesh(m_index).visible; }

int PyMesh::vertexNumber() const { return m_doc->mesh(m_index).mesh.VN(); }

int PyMesh::faceNumber() const { return m_doc->mesh(m_index).mesh.FN(); }

int PyMesh::edgeNumber() const { return m_doc->mesh(m_index).mesh.EN(); }

std::string PyMesh::label() const
{
    return m_doc->mesh(m_index).name.toStdString();
}

bool PyMesh::isCompact() const
{
    const auto &cm = this->cm();
    return cm.VN() == int(cm.vert.size()) && cm.FN() == int(cm.face.size());
}

bool PyMesh::isPointCloud() const { return faceNumber() == 0; }

const VCGMesh &PyMesh::cm() const
{
    return m_doc->mesh(m_index).mesh;
}

auto PyMesh::vertexMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 3>>
{
    const auto &cm = this->cm();
    return makeNx3(cm.VN(), [&cm](int i) -> const vcg::Point3f & {
        return cm.vert[i].P();
    });
}

auto PyMesh::faceMatrix() const -> nb::ndarray<nb::numpy, int32_t, nb::shape<-1, 3>>
{
    return makeFaceArray(this->cm().FN(), this->cm());
}

auto PyMesh::vertexNormalMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 3>>
{
    const auto &cm = this->cm();
    return makeNx3(cm.VN(), [&cm](int i) -> const vcg::Point3f & {
        return cm.vert[i].N();
    });
}

auto PyMesh::vertexColorMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 4>>
{
    const auto &cm = this->cm();
    return makeNx4Color(cm.VN(), [&cm](int i) -> const vcg::Color4b & {
        return cm.vert[i].C();
    });
}

auto PyMesh::vertexScalarArray() const -> nb::ndarray<nb::numpy, double, nb::shape<-1>>
{
    const auto &cm = this->cm();
    return makeScalarArray(cm.VN(), [&cm](int i) {
        return cm.vert[i].Q();
    });
}

auto PyMesh::faceNormalMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 3>>
{
    const auto &cm = this->cm();
    return makeNx3(cm.FN(), [&cm](int i) -> const vcg::Point3f & {
        return cm.face[i].N();
    });
}

auto PyMesh::faceColorMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 4>>
{
    const auto &cm = this->cm();
    return makeNx4Color(cm.FN(), [&cm](int i) -> const vcg::Color4b & {
        return cm.face[i].C();
    });
}

auto PyMesh::vertexTexCoordMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 2>>
{
    const auto &cm = this->cm();
    if (!cm.vert.IsTexCoordEnabled())
        throw std::runtime_error("Vertex texture coordinates are not available");
    return makeUVArray(cm.VN(), [&cm](int i) -> const vcg::TexCoord2<float, 1> & {
        return cm.vert[i].T();
    });
}

auto PyMesh::wedgeTexCoordMatrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 3, 2>>
{
    const auto &cm = this->cm();
    if (!cm.face.IsWedgeTexCoordEnabled())
        throw std::runtime_error("Wedge texture coordinates are not available");
    return makeWedgeArray(cm.FN(), cm);
}

bool PyMesh::hasVertexTexCoord() const { return this->cm().vert.IsTexCoordEnabled(); }

bool PyMesh::hasWedgeTexCoord() const { return this->cm().face.IsWedgeTexCoordEnabled(); }

auto PyMesh::vertexCurvaturePrincipalDir1Matrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 3>>
{
    const auto &cm = this->cm();
    if (!cm.vert.IsCurvatureDirEnabled())
        throw std::runtime_error("Vertex curvature directions are not available");
    return makeNx3(cm.VN(), [&cm](int i) -> const vcg::Point3f & {
        return cm.vert[i].PD1();
    });
}

auto PyMesh::vertexCurvaturePrincipalDir2Matrix() const -> nb::ndarray<nb::numpy, double, nb::shape<-1, 3>>
{
    const auto &cm = this->cm();
    if (!cm.vert.IsCurvatureDirEnabled())
        throw std::runtime_error("Vertex curvature directions are not available");
    return makeNx3(cm.VN(), [&cm](int i) -> const vcg::Point3f & {
        return cm.vert[i].PD2();
    });
}

bool PyMesh::hasVertexCurvature() const { return this->cm().vert.IsCurvatureDirEnabled(); }

// ---------------------------------------------------------------------------
// Nanobind registration
// ---------------------------------------------------------------------------

void registerPyMesh(nb::module_ &m)
{
    nb::class_<PyMesh>(m, "Mesh")
        .def("id", &PyMesh::id,
             "Persistent mesh id (an opaque handle, minted from 1 and never reused). "
             "Stable across layer removals; use it to key render-state JSON.")
        .def("index", &PyMesh::index,
             "0-based position of this mesh in the layer list. Shifts when a layer "
             "below it is removed; pass it to MeshSet.mesh()/set_current_mesh().")
        .def("is_visible", &PyMesh::isVisible)
        .def("vertex_number", &PyMesh::vertexNumber)
        .def("face_number", &PyMesh::faceNumber)
        .def("edge_number", &PyMesh::edgeNumber)
        .def("label", &PyMesh::label)
        .def("is_compact", &PyMesh::isCompact)
        .def("is_point_cloud", &PyMesh::isPointCloud)
        .def("vertex_matrix", &PyMesh::vertexMatrix)
        .def("face_matrix", &PyMesh::faceMatrix)
        .def("vertex_normal_matrix", &PyMesh::vertexNormalMatrix)
        .def("vertex_color_matrix", &PyMesh::vertexColorMatrix)
        .def("vertex_scalar_array", &PyMesh::vertexScalarArray)
        .def("face_normal_matrix", &PyMesh::faceNormalMatrix)
        .def("face_color_matrix", &PyMesh::faceColorMatrix)
        .def("vertex_tex_coord_matrix", &PyMesh::vertexTexCoordMatrix)
        .def("wedge_tex_coord_matrix", &PyMesh::wedgeTexCoordMatrix)
        .def("has_vertex_tex_coord", &PyMesh::hasVertexTexCoord)
        .def("has_wedge_tex_coord", &PyMesh::hasWedgeTexCoord)
        .def("vertex_curvature_principal_dir1_matrix",
             &PyMesh::vertexCurvaturePrincipalDir1Matrix)
        .def("vertex_curvature_principal_dir2_matrix",
             &PyMesh::vertexCurvaturePrincipalDir2Matrix)
        .def("has_vertex_curvature", &PyMesh::hasVertexCurvature);
}

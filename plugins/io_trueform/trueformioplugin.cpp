#include "plugins/io_trueform/trueformioplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QFileInfo>
#include <QObject>
#include <QStringList>
#include <exception>
#include <string>
#include <vector>

// Qt's keyword macros collide with ordinary identifiers inside TrueForm and oneTBB:
// `emit` hits tbb::profiling::event::emit(), and `slots` hits local variables named
// slots. Both expand to nothing, so the declarations become syntactically invalid far
// from the real cause. Hide all three keyword macros across the include.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include <trueform/io.hpp>
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

namespace {

constexpr int kErrRead = -1;
constexpr int kErrEmpty = -2;
constexpr int kErrWrite = -3;
constexpr int kErrUnsupported = -4;

using Mask = vcg::tri::io::Mask;

QString extensionOf(const QString &filename)
{
    return QFileInfo(filename).suffix().toLower();
}

// Copy a trueform polygons_buffer into a VCGMesh, triangulating by fan. TrueForm's OBJ
// reader returns dynamic-size faces (n-gons); its STL reader always returns triangles.
template <typename Buffer>
bool copyToVcgMesh(const Buffer &buffer, VCGMesh &mesh)
{
    mesh.Clear();

    const auto points = buffer.points();
    const std::size_t pointCount = std::size_t(points.size());
    if (pointCount == 0)
        return false;

    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, int(pointCount));
    std::size_t vi = 0;
    for (const auto &p : points) {
        mesh.vert[vi].P() = vcg::Point3f(float(p[0]), float(p[1]), float(p[2]));
        ++vi;
    }

    for (const auto &face : buffer.faces()) {
        const std::size_t n = std::size_t(face.size());
        if (n < 3)
            continue;
        // Fan-triangulate: QMeshLab stores triangle meshes.
        for (std::size_t k = 2; k < n; ++k) {
            const int a = int(face[0]);
            const int b = int(face[k - 1]);
            const int c = int(face[k]);
            if (a < 0 || b < 0 || c < 0)
                continue;
            if (std::size_t(a) >= pointCount || std::size_t(b) >= pointCount
                || std::size_t(c) >= pointCount)
                continue;
            if (a == b || b == c || a == c)
                continue;
            vcg::tri::Allocator<VCGMesh>::AddFace(mesh, a, b, c);
        }
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
    return true;
}

// Build the trueform triangle soup that the writers consume from a VCGMesh.
tf::polygons_buffer<int, float, 3, 3> makeTrueFormTriangles(const VCGMesh &mesh)
{
    tf::polygons_buffer<int, float, 3, 3> out;

    std::vector<int> remap(mesh.vert.size(), -1);
    auto &points = out.points_buffer();
    int next = 0;
    for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
        if (mesh.vert[i].IsD())
            continue;
        const vcg::Point3f &p = mesh.vert[i].cP();
        points.emplace_back(p.X(), p.Y(), p.Z());
        remap[i] = next++;
    }

    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    auto &faces = out.faces_buffer();
    for (const VCGFace &f : mesh.face) {
        if (f.IsD() || !base)
            continue;
        int corner[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const ptrdiff_t raw = f.cV(k) - base;
            if (raw < 0 || std::size_t(raw) >= remap.size() || remap[std::size_t(raw)] < 0) {
                ok = false;
                break;
            }
            corner[k] = remap[std::size_t(raw)];
        }
        if (!ok)
            continue;
        faces.emplace_back(corner[0], corner[1], corner[2]);
    }
    return out;
}

// A second, independent reader/writer for OBJ and STL.
//
// This is not a replacement for io_vcg or io_obj_rapidobj: it exists because
// independent parsers fail on *different* malformed files, and a format as loosely
// specified as OBJ or STL has a long tail of variants in the wild. When one reader
// rejects a file, another often opens it.
//
// Two behavioural differences worth knowing:
//  - STL import **deduplicates vertices while loading**. STL is a triangle soup with no
//    shared vertices, so every other reader yields a mesh needing "Remove Duplicate
//    Vertices" afterwards; this one arrives welded.
//  - OBJ import reads **vertex positions and faces only** — no UVs, normals or
//    materials. For a textured OBJ, use io_vcg or io_obj_rapidobj instead.
class TrueFormIOPlugin final : public MeshIOPlugin
{
public:
    QString pluginId() const override { return QStringLiteral("meshlab2.io.trueform"); }

    QString name() const override { return QObject::tr("TrueForm OBJ/STL"); }

    QStringList supportedExtensions() const override
    {
        return { QStringLiteral("obj"), QStringLiteral("stl") };
    }

    bool canLoad(const QString &filename) const override
    {
        return supportedExtensions().contains(extensionOf(filename));
    }

    bool canSave(const QString &filename) const override
    {
        return supportedExtensions().contains(extensionOf(filename));
    }

    MeshIOCapabilities loadCapabilities(const QString &filename) const override
    {
        (void) filename;
        // Geometry only: neither reader recovers UVs, normals or materials.
        MeshIOCapabilities caps;
        caps.mask = Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX;
        return caps;
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask)
        const override
    {
        if (cb)
            (*cb)(5, "Reading with TrueForm...");

        const QString ext = extensionOf(filename);
        const std::string path = filename.toStdString();
        bool built = false;
        try {
            if (ext == QStringLiteral("stl")) {
                const auto buffer = tf::read_stl<int>(path);
                if (buffer.empty())
                    return kErrEmpty;
                built = copyToVcgMesh(buffer, mesh);
            } else if (ext == QStringLiteral("obj")) {
                const auto buffer = tf::read_obj<int, float>(path);
                if (buffer.empty())
                    return kErrEmpty;
                built = copyToVcgMesh(buffer, mesh);
            } else {
                return kErrUnsupported;
            }
        } catch (const std::exception &) {
            return kErrRead;
        } catch (...) {
            return kErrRead;
        }

        if (!built)
            return kErrEmpty;

        if (outLoadMask)
            *outLoadMask = Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX;
        if (cb)
            (*cb)(100, "Done.");
        return 0;
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb) const override
    {
        (void) options;
        if (cb)
            (*cb)(5, "Writing with TrueForm...");

        const QString ext = extensionOf(filename);
        const std::string path = filename.toStdString();
        try {
            const auto triangles = makeTrueFormTriangles(mesh);
            if (triangles.empty())
                return kErrEmpty;
            if (ext == QStringLiteral("stl")) {
                if (!tf::write_stl(triangles.polygons(), path))
                    return kErrWrite;
            } else if (ext == QStringLiteral("obj")) {
                if (!tf::write_obj(triangles.polygons(), path))
                    return kErrWrite;
            } else {
                return kErrUnsupported;
            }
        } catch (const std::exception &) {
            return kErrWrite;
        } catch (...) {
            return kErrWrite;
        }

        if (cb)
            (*cb)(100, "Done.");
        return 0;
    }

    QString filterString() const override
    {
        return QObject::tr("TrueForm Meshes (*.obj *.stl)");
    }

    QString saveFilterString() const override
    {
        return QObject::tr("Wavefront OBJ (*.obj);;STL (*.stl)");
    }

    QString errorString(int errCode) const override
    {
        switch (errCode) {
        case kErrRead:
            return QObject::tr("TrueForm could not parse the file.");
        case kErrEmpty:
            return QObject::tr("The file contained no usable geometry.");
        case kErrWrite:
            return QObject::tr("TrueForm could not write the file.");
        case kErrUnsupported:
            return QObject::tr("TrueForm handles only OBJ and STL files.");
        default:
            return QObject::tr("Unknown TrueForm I/O error.");
        }
    }
};

} // namespace

void registerTrueFormIOPlugin(MeshIOPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TrueFormIOPlugin>());
}

#pragma once

#include <vcg/complex/complex.h>
#include <wrap/io_trimesh/io_mask.h>

#include <algorithm>
#include <cmath>

class VCGVertex;
class VCGEdge;
class VCGFace;

struct VCGUsedTypes : public vcg::UsedTypes<
    vcg::Use<VCGVertex>::AsVertexType,
    vcg::Use<VCGEdge>::AsEdgeType,
    vcg::Use<VCGFace>::AsFaceType> {};

// Fixed (always allocated): Coord, Normal, Color, Quality, BitFlags
// OCF storable (allocated when data is loaded/computed, preserved in undo):
//   TexCoordfOcf, CurvatureDirfOcf
// OCF ancillary (allocated on demand by algorithms, discarded after use):
//   VFAdjOcf, MarkOcf
class VCGVertex : public vcg::Vertex<VCGUsedTypes,
    vcg::vertex::InfoOcf,
    vcg::vertex::Coord3f,
    vcg::vertex::Normal3f,
    vcg::vertex::Color4b,
    vcg::vertex::Qualityf,
    vcg::vertex::BitFlags,
    vcg::vertex::TexCoordfOcf,
    vcg::vertex::CurvatureDirfOcf,
    vcg::vertex::VFAdjOcf,
    vcg::vertex::MarkOcf> {};

class VCGEdge : public vcg::Edge<VCGUsedTypes,
    vcg::edge::VertexRef,
    vcg::edge::Color4b,
    vcg::edge::BitFlags> {};

// Fixed (always allocated): VertexRef, Normal, Color, Quality, BitFlags
// OCF storable (allocated when data is loaded/computed, preserved in undo):
//   WedgeTexCoordfOcf
// OCF ancillary (allocated on demand by algorithms, discarded after use):
//   FFAdjOcf, VFAdjOcf, MarkOcf
class VCGFace : public vcg::Face<VCGUsedTypes,
    vcg::face::InfoOcf,
    vcg::face::VertexRef,
    vcg::face::Normal3f,
    vcg::face::Color4b,
    vcg::face::Qualityf,
    vcg::face::BitFlags,
    vcg::face::WedgeTexCoordfOcf,
    vcg::face::FFAdjOcf,
    vcg::face::VFAdjOcf,
    vcg::face::MarkOcf> {};

class VCGMesh : public vcg::tri::TriMesh<
    vcg::vertex::vector_ocf<VCGVertex>,
    std::vector<VCGEdge>,
    vcg::face::vector_ocf<VCGFace>> {};

// Texture coordinate of a face corner, honouring the mesh's ioMask (per-wedge
// preferred, else per-vertex). Returns false if there are no usable texcoords or
// the value is non-finite. Shared by the UV renderer and the UV-space selection
// filter so both read parametrization coordinates the same way.
// Reading a vcg OCF texcoord (cWT/cT) is only legal when the optional component
// is actually ENABLED on the mesh. ioMask is merely an I/O hint and can disagree
// with the live mesh — e.g. after an undo/redo whose restored ioMask is paired
// with geometry that doesn't have the component — so every read below is gated on
// the container's runtime enabled flag (reached from the element via Base()).
// Without this guard a stale ioMask bit dereferences a null OCF backing store.
inline bool vcgFaceCornerUV(int ioMask, const VCGFace &f, int corner, float &u, float &v)
{
    if ((ioMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) && f.Base().IsWedgeTexCoordEnabled()) {
        const auto &wt = f.cWT(corner);
        u = wt.U();
        v = wt.V();
    } else if (ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) {
        const VCGVertex *vp = f.cV(corner);
        if (!vp || !vp->Base().IsTexCoordEnabled())
            return false;
        const auto &vt = vp->cT();
        u = vt.U();
        v = vt.V();
    } else {
        return false;
    }
    return std::isfinite(u) && std::isfinite(v);
}

inline int vcgFaceTextureGroup(int ioMask, const VCGFace &f)
{
    if ((ioMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) && f.Base().IsWedgeTexCoordEnabled())
        return std::max(0, int(f.cWT(0).N()));
    if ((ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) && f.cV(0) && f.cV(0)->Base().IsTexCoordEnabled())
        return std::max(0, int(f.cV(0)->cT().N()));
    return 0;
}

// ---------------------------------------------------------------------------
// RAII helpers for ancillary OCF fields.
// These fields are computed by algorithms and must be discarded after use —
// never stored in undo snapshots or deepCopy.
// ---------------------------------------------------------------------------

// Enables face-face adjacency for the scope of the object.
struct VCGMeshFFAdjScope {
    VCGMesh &m;
    explicit VCGMeshFFAdjScope(VCGMesh &m) : m(m) { m.face.EnableFFAdjacency(); }
    ~VCGMeshFFAdjScope() { m.face.DisableFFAdjacency(); }
    VCGMeshFFAdjScope(const VCGMeshFFAdjScope &) = delete;
    VCGMeshFFAdjScope &operator=(const VCGMeshFFAdjScope &) = delete;
};

// Enables vertex-face adjacency (both vert and face vectors) for the scope.
struct VCGMeshVFAdjScope {
    VCGMesh &m;
    explicit VCGMeshVFAdjScope(VCGMesh &m) : m(m) {
        m.vert.EnableVFAdjacency();
        m.face.EnableVFAdjacency();
    }
    ~VCGMeshVFAdjScope() {
        m.vert.DisableVFAdjacency();
        m.face.DisableVFAdjacency();
    }
    VCGMeshVFAdjScope(const VCGMeshVFAdjScope &) = delete;
    VCGMeshVFAdjScope &operator=(const VCGMeshVFAdjScope &) = delete;
};

// Enables face Mark for the scope (used by Tmark / spatial queries).
struct VCGMeshMarkScope {
    VCGMesh &m;
    explicit VCGMeshMarkScope(VCGMesh &m) : m(m) { m.face.EnableMark(); }
    ~VCGMeshMarkScope() { m.face.DisableMark(); }
    VCGMeshMarkScope(const VCGMeshMarkScope &) = delete;
    VCGMeshMarkScope &operator=(const VCGMeshMarkScope &) = delete;
};

// Enables vertex Mark for the scope (required by RemoveTVertex functions).
struct VCGMeshVertexMarkScope {
    VCGMesh &m;
    explicit VCGMeshVertexMarkScope(VCGMesh &m) : m(m) { m.vert.EnableMark(); }
    ~VCGMeshVertexMarkScope() { m.vert.DisableMark(); }
    VCGMeshVertexMarkScope(const VCGMeshVertexMarkScope &) = delete;
    VCGMeshVertexMarkScope &operator=(const VCGMeshVertexMarkScope &) = delete;
};

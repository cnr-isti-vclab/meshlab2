#pragma once

#include "vcgmesh.h"

#include <vcg/space/point3.h>

#include <cstdint>
#include <memory>

// Visibility helper: answers "is this point on the mesh hidden from the given eye,
// or from the given direction (an orthographic view)?" — i.e. true ray-traced
// occlusion, not backface culling. All coordinates are in the mesh's own (local) space.
//
// Backed by Intel Embree when it is compiled in (watertight, robust at grazing
// angles), otherwise by a vcglib GridStaticPtr ray caster. The acceleration
// structure is built once and cached by (mesh, geometryRevision), so a series of
// queries — e.g. successive rubber-band selections on the same mesh — reuse it
// instead of rebuilding every time.
class ViewpointOccluder
{
public:
    // Returns the occluder for this mesh revision, building (and caching) it on
    // first use. The returned instance stays valid as long as the caller holds
    // the shared_ptr; a later call with a different mesh/revision rebuilds.
    static std::shared_ptr<ViewpointOccluder> getOrBuild(VCGMesh &mesh,
                                                          std::uint64_t geometryRevision);

    ~ViewpointOccluder();
    ViewpointOccluder(const ViewpointOccluder &) = delete;
    ViewpointOccluder &operator=(const ViewpointOccluder &) = delete;

    // True if geometry lies between 'eye' and 'sample' (sample is hidden).
    // Thread-safe: may be called concurrently from worker threads once built.
    bool isOccluded(const vcg::Point3f &sample, const vcg::Point3f &eye) const;

    // The same for parallel rays: true if geometry lies between 'sample' and infinity in
    // the unit direction 'towardViewer' (an orthographic view has no eye). 'sample' must be
    // on or near the mesh. Implemented as the segment form with its far end placed past
    // the mesh bounding box, which is outside the mesh by construction.
    bool isOccludedAlong(const vcg::Point3f &sample, const vcg::Point3f &towardViewer) const;

    // Which backend is in use (for logging / diagnostics).
    bool usesEmbree() const;

private:
    ViewpointOccluder(VCGMesh &mesh);

    struct Impl;
    std::unique_ptr<Impl> d;
};

#pragma once

#include "meshfilterplugin.h"

// Surface reconstruction backed by Open3D. Implementation unit of
// filter_open3d - see open3dpointcloud.h.
bool isOpen3DReconstructionFilter(const QString &filterId);
MeshFilterRunResult runOpen3DReconstructionFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

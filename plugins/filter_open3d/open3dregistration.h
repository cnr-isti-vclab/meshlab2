#pragma once

#include "meshfilterplugin.h"

// Pairwise registration backed by Open3D: the ICP family and a feature-based
// initializer. Implementation unit of filter_open3d - see open3dpointcloud.h.
bool isOpen3DRegistrationFilter(const QString &filterId);
MeshFilterRunResult runOpen3DRegistrationFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

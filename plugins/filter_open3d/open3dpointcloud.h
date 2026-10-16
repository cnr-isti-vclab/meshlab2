#pragma once

#include "meshfilterplugin.h"

// Point-cloud preparation backed by Open3D: voxel sampling, outlier selection
// and normal estimation. Implementation unit of filter_open3d - one plugin per
// dependency, so these are free functions rather than a plugin of their own.
bool isOpen3DPointCloudFilter(const QString &filterId);
MeshFilterRunResult runOpen3DPointCloudFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

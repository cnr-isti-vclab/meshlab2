#pragma once

#include "meshfilterplugin.h"

// Centroidal Voronoi Tessellation remeshing backed by geogram.
// Implementation unit of filter_geogram - see geogrambooleans.h.
bool isGeogramRemeshingFilter(const QString &filterId);
MeshFilterRunResult runGeogramRemeshingFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

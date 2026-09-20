#pragma once

#include "meshfilterplugin.h"

// Atlas generation, chart packing and chart segmentation backed by geogram.
// Implementation unit of filter_geogram - see geogrambooleans.h.
bool isGeogramAtlasFilter(const QString &filterId);
MeshFilterRunResult runGeogramAtlasFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

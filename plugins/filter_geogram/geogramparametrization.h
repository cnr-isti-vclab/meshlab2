#pragma once

#include "meshfilterplugin.h"

// UV parametrization backed by geogram's flatteners (LSCM, ABF++).
// Implementation unit of filter_geogram - see geogrambooleans.h.
bool isGeogramParametrizationFilter(const QString &filterId);
MeshFilterRunResult runGeogramParametrizationFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

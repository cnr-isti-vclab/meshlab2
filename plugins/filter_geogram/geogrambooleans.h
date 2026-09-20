#pragma once

#include "meshfilterplugin.h"

// Boolean operations and self-intersection repair backed by geogram's exact
// predicates. Implementation unit of filter_geogram - one plugin per
// dependency (see docs/design/filter_organization.md, decision 1), so these
// are free functions rather than a plugin of their own.
bool isGeogramBooleanFilter(const QString &filterId);
MeshFilterRunResult runGeogramBooleanFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc);

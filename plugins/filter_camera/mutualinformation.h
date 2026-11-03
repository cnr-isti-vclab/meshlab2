#pragma once

#include "meshfilterplugin.h"

// Align Current Raster to Mesh by Mutual Information: refines the current raster's camera so
// that a rendering of the visible layers through it shares as much information with the photo
// as it can (Corsini et al. 2009). Stage 3c of docs/design/proposals/raster_alignment.md.
MeshFilterRunResult alignRasterByMutualInformation(const FilterParams &params, Document &doc);

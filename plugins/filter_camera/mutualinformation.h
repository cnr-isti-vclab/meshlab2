#pragma once

#include "meshfilterplugin.h"

// Align Current Raster to Mesh by Mutual Information: refines the current raster's camera so
// that a rendering of the visible layers through it shares as much information with the photo
// as it can (Corsini et al. 2009). Stage 3c of docs/design/proposals/raster_alignment.md.
MeshFilterRunResult alignRasterByMutualInformation(const FilterParams &params, Document &doc);

// Align Visible Rasters Globally by Mutual Information: refines the visible rasters' cameras
// together, each against the mesh painted with the photos of the rasters that overlap it, so
// that they all project the same detail onto the same place (Dellepiane and Scopigno 2013).
// Hidden rasters are anchors: they paint, and never move. Stage 3d of the proposal.
MeshFilterRunResult alignRastersGloballyByMutualInformation(const FilterParams &params, Document &doc);

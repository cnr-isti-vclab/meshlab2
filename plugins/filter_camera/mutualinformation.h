#pragma once

#include "meshfilterplugin.h"

#include <vcg/space/point2.h>
#include <vcg/space/point3.h>

#include <QSize>

#include <vector>

// The point pairs of the alignment filters, a JSON array of {"point": [x, y, z], "pixel": [u, v]}
// with pixels from the top-left corner of a viewport of `viewport`, y down: the points, and the
// pixels turned to vcg's, y up. An error message, or empty.
QString parseCorrespondences(const QString &json, const QSize &viewport, std::vector<vcg::Point3d> &points,
                             std::vector<vcg::Point2d> &pixels);

// Align Current Raster to Mesh by Mutual Information: refines the current raster's camera so
// that a rendering of the visible layers through it shares as much information with the photo
// as it can (Corsini et al. 2009). See docs/design/raster_alignment.md.
MeshFilterRunResult alignRasterByMutualInformation(const FilterParams &params, Document &doc);

// Align Visible Rasters Globally by Mutual Information: refines the visible rasters' cameras
// together, each against the mesh painted with the photos of the rasters that overlap it, so
// that they all project the same detail onto the same place (Dellepiane and Scopigno 2013).
// Hidden rasters are anchors: they paint, and never move.
MeshFilterRunResult alignRastersGloballyByMutualInformation(const FilterParams &params, Document &doc);

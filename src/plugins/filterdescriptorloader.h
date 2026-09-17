#pragma once

#include "meshfilterplugin.h"
#include <QString>
#include <vector>

class Document;

// Loads MeshFilterDescriptor objects from a JSON resource file and
// resolves symbolic bound tokens that depend on the current document state.
//
// JSON format:
//   {
//     "pluginId": "meshlab2.filter.basic",
//     "filters": [ { ... }, ... ]
//   }
//
// Each filter entry:
//   {
//     "id": "...",
//     "categories": [ "Root/Subcategory", ... ],   // ordered, first is primary;
//                                                  // validated against FilterCategories
//                                                  // (legacy "menuPath": "..." still read)
//     "name": "...",
//     "shortDescription": "...",
//     "longDescriptionMarkdown": "...",
//     "tags": [ "...", ... ],
//     "inputDomain":  "None" | "SingleMesh" | "WholeDocument",
//     "outputDomain": "Information" | "ModifyCurrentMesh" | "NewMeshes",
//     "inputRequirements": {
//       "requireVertices": bool, "requireEdges": bool, "requireFaces": bool,
//       "requireVertexColor": bool, "requireFaceColor": bool,
//       "requireTextureCoordinates": bool,
//       "requirePerVertexTexCoords": bool, "requirePerWedgeTexCoords": bool,
//       "requireTextures": bool,
//       "requireVertexQuality": bool, "requireFaceQuality": bool
//     },
//     "preRunCleanup": [
//       {
//         "kind": "removeUnreferencedVertices",
//         "whenBoolParam": "Autoclean",   // optional
//         "meshParameter": "sourceMesh"   // optional; default current mesh
//       }
//     ],
//     "postRunCleanup": [ ...same action object format... ],
//     "parameters": [
//       {
//         "id": "...", "label": "...", "help": "...", "group": "...",
//         "type": "bool" | "int" | "double" | "absperc" | "string" | "fileopen" | "filesave" | "textureref" | "textureoutputref" | "enum" | "color",
//         "default": <value>,   // may be "@token" string for dynamic values
//         "min": <value>,       // optional; may be "@token"
//         "max": <value>,       // optional; may be "@token"
//         "decimals": 3,        // optional, only for double
//         "enabledWhen": "flag" | "!flag",  // optional: id of a bool parameter that
//                               // gates this one; the editor greys out while the
//                               // condition is false. '!' inverts.
//         "meshRequirements": { ...same requirement keys as inputRequirements... },
//         "meshPrepare": ["FF", "BBox"], // only for type=="mesh"
//         "meshAllowsNone": true, // only for type=="mesh": the selection may be
//                               // left empty, and the filter then reads -1
//         "sourceMeshParameter": "sourceMesh", // only for textureref/textureoutputref
//         "allowAutomatic": true,               // only for textureref
//         "enumOptions": [      // only for enum type
//           { "id": "...", "label": "...", "help": "..." }, ...
//         ]
//       }, ...
//     ]
//   }
//
// Symbolic tokens (resolved by resolveSymbolicBounds against a Document):
//   @bboxDiag              - current mesh bounding-box diagonal
//   @bboxDiag01            - bboxDiag * 0.01
//   @bboxDiag001           - bboxDiag * 0.001
//   @bboxDiag0001          - bboxDiag * 0.0001
//   @bboxDiag0005          - bboxDiag * 0.005
//   @bboxDiag002           - bboxDiag * 0.02
//   @bboxDiag003           - bboxDiag * 0.03
//   @bboxDiag01            - bboxDiag * 0.01  (alias)
//   @bboxDiagTenth         - bboxDiag * 0.1
//   @bboxDiagHalf          - bboxDiag * 0.5
//   @bboxDiag5x            - bboxDiag * 5.0
//   @qualityVMin           - per-vertex quality minimum
//   @qualityVMax           - per-vertex quality maximum
//   @qualityFMin           - per-face quality minimum
//   @qualityFMax           - per-face quality maximum
//   @hasSelectedFaces      - bool: selected face count > 0
//   @hasSelectedVerts      - bool: selected vertex count > 0
//   @faceCount             - max(1, face count)
//   @faceCountHalf         - max(1, face count / 2)
//   @selOrFaceCountHalf    - max(1, (selFaces>0 ? selFaces : faceCount) / 2)
//   @hardwareThreads       - hardware thread count (min 1, fallback 8)
class FilterDescriptorLoader
{
public:
    // Load all filter descriptors from a Qt resource path.
    // Returns an empty vector and sets errorMessage on failure.
    static std::vector<MeshFilterDescriptor> load(
        const QString &resourcePath,
        QString &errorMessage);

    // Load a bare parameter list — a JSON object with a top-level "parameters"
    // array using exactly the schema documented above — from a Qt resource path.
    // Application preferences are declared this way, so they get the same editors,
    // grouping and help text as filter parameters.
    static std::vector<MeshFilterParameterDescriptor> loadParameters(
        const QString &resourcePath,
        QString &errorMessage);

    // Warns (once per descriptor file) about categories that are not in the
    // ontology. See src/plugins/filtercategories.h.
    static void validateCategories(
        const std::vector<MeshFilterDescriptor> &descriptors,
        const QString &resourcePath);

    // Warns about absperc parameters missing min/max: without a max, "100%" resolves to
    // 1.0 model units instead of the intended @bboxDiag, and the slider is meaningless.
    static void validateAbsPercBounds(
        const std::vector<MeshFilterDescriptor> &descriptors,
        const QString &resourcePath);

    // Resolve all "@token" symbolic values in descriptor bounds/defaults
    // against the current document state. Called from filters() implementations.
    static void resolveSymbolicBounds(
        std::vector<MeshFilterDescriptor> &descriptors,
        const Document &doc);
};

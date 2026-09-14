#pragma once

#include "renderingsettings.h"
#include "vcgmesh.h"
#include <QFile>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QRect>
#include <QSize>
#include <QtGlobal>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <QDir>

namespace RenderWidgetInternal {

inline QString normalizeTexturePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed())).toLower();
}

inline QShader loadShader(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("Failed to open shader: %s", qPrintable(path));
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

inline constexpr int kUbufSize = 352; // expanded: added vec4 lightDir at offset 336
inline constexpr int kUbufFloatCount = kUbufSize / sizeof(float);
inline constexpr int kUbufBBoxColorOffset = 176 / sizeof(float);
inline constexpr int kUbufPointColorOffset = 192 / sizeof(float);
inline constexpr int kUbufPointParamsOffset = 208 / sizeof(float);
inline constexpr int kUbufWireColorOffset = 224 / sizeof(float);
inline constexpr int kUbufWireParamsOffset = 240 / sizeof(float);
inline constexpr int kUbufFillColorOffset = 256 / sizeof(float);
inline constexpr int kUbufLightingParamsOffset = 272 / sizeof(float);
inline constexpr int kUbufEdgeColorOffset = 288 / sizeof(float);
inline constexpr int kUbufMaterialFlagsOffset = 304 / sizeof(float);  // was kUbufPbrMapUsageOffset
inline constexpr int kUbufMaterialParamsOffset = 320 / sizeof(float); // was kUbufPbrParamsOffset
inline constexpr int kUbufLightDirOffset = 336 / sizeof(float);        // vec4 lightDir (view-space, w unused)
inline constexpr int kFillVertexStrideFloats = 13;
inline constexpr int kPointsVertexStrideFloats = 11;
inline constexpr int kMaskMorphUbufSize = 16;
inline constexpr int kMaskDebugUbufSize = 16;
inline constexpr int kOutlineExtractUbufSize = 16;
inline constexpr int kOutlineUbufSize = 48;
inline constexpr int kRasterBackplateUbufSize = 32; // vec4 rect + vec4 params
inline constexpr int kRasterProjectedUbufSize = 80; // mat4 mvp + vec4 color
inline constexpr int kRasterProjectedVertexStrideFloats = 3;
inline constexpr int kRasterProjectedFrustumVertexCount = 16;
inline constexpr int kDecoratorUbufSize = 80; // mat4 mvp + vec4 color
inline constexpr int kDecoratorFatUbufSize = 96; // mat4 mvp + vec4 color + vec4(width, invW, invH, _)
inline constexpr int kToolLineUbufSize = 96;
inline constexpr int kDecoratorSlotVertexNormals = 0;
inline constexpr int kDecoratorSlotFaceNormals = 1;
inline constexpr int kDecoratorSlotBoundaryEdges = 2;
inline constexpr int kDecoratorSlotTextureSeams = 3;
inline constexpr int kDecoratorSlotNonManifoldEdges = 4;
inline constexpr int kDecoratorSlotNonManifoldVertices = 5;
inline constexpr int kDecoratorSlotCurvaturePD1 = 6;
inline constexpr int kDecoratorSlotCurvaturePD2 = 7;
inline constexpr int kDecoratorSlotCount = 8;
inline constexpr int kTrackballGizmoUbufSize = 96; // mat4 mvp + vec4(center.xyz, radius) + vec4(camera.xyz, backShade)
inline constexpr int kTrackballGizmoSteps = 96;
inline constexpr int kLightGizmoUbufSize = 96;  // mat4 mvp + vec4 lightDir + vec4 params
inline constexpr int kLightGizmoSteps = 64;
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kImageViewMinZoom = 0.05f;
inline constexpr float kImageViewMaxZoom = 5000.0f;

inline QVector4D fitImageRectNdc(const QSize &imageSize, const QSize &targetSize)
{
    if (imageSize.width() <= 0 || imageSize.height() <= 0
        || targetSize.width() <= 0 || targetSize.height() <= 0) {
        return QVector4D(0.0f, 0.0f, 1.0f, 1.0f);
    }

    const float imageAspect = float(imageSize.width()) / float(imageSize.height());
    const float targetAspect = float(targetSize.width()) / float(targetSize.height());
    float halfW = 1.0f;
    float halfH = 1.0f;
    if (imageAspect > targetAspect)
        halfH = targetAspect / imageAspect;
    else
        halfW = imageAspect / targetAspect;
    return QVector4D(0.0f, 0.0f, halfW, halfH);
}

inline QVector4D rasterViewRectNdc(
    const QSize &imageSize,
    const QSize &targetSize,
    float zoom,
    const QVector2D &pan)
{
    const QVector4D fitRect = fitImageRectNdc(imageSize, targetSize);
    const float halfW = fitRect.z() * qMax(1e-6f, zoom);
    const float halfH = fitRect.w() * qMax(1e-6f, zoom);
    return QVector4D(
        halfW * (1.0f - 2.0f * pan.x()),
        halfH * (2.0f * pan.y() - 1.0f),
        halfW,
        halfH);
}

inline QMatrix4x4 rasterViewClipMatrix(
    const QSize &imageSize,
    const QSize &targetSize,
    float zoom,
    const QVector2D &pan)
{
    const QVector4D rect = rasterViewRectNdc(imageSize, targetSize, zoom, pan);
    QMatrix4x4 transform;
    transform.setToIdentity();
    transform.translate(rect.x(), rect.y(), 0.0f);
    transform.scale(rect.z(), rect.w(), 1.0f);
    return transform;
}

struct MainStyleUbufKey {
    QColor bboxWireColor;
    QColor pointColor;
    float pointSize = 0.0f;
    QColor wireColor;
    float wireSize = 0.0f;
    QColor fillColor;
    bool pointLighting = false;
    bool wireLighting = false;
    bool fillLighting = false;
    FillMaterial fillMaterial = FillMaterial::Plain;
    PbrFillParams fillPbr;
    RsFillParams  fillRs;
    PlainFillParams fillPlain;
    QColor edgeColor;
    float edgeSize = 0.0f;

    bool operator==(const MainStyleUbufKey &other) const
    {
        return bboxWireColor == other.bboxWireColor
            && pointColor == other.pointColor
            && pointSize == other.pointSize
            && wireColor == other.wireColor
            && wireSize == other.wireSize
            && fillColor == other.fillColor
            && pointLighting == other.pointLighting
            && wireLighting == other.wireLighting
            && fillLighting == other.fillLighting
            && fillMaterial == other.fillMaterial
            && fillPbr == other.fillPbr
            && fillRs == other.fillRs
            && fillPlain == other.fillPlain
            && edgeColor == other.edgeColor
            && edgeSize == other.edgeSize;
    }
};

struct MainUbufMaterialOverrides {
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float roughnessFactor = 1.0f;
    QColor fillColorOverride = QColor(); // invalid = no override, use settings.fillColor
};

inline MainStyleUbufKey mainStyleUbufKeyFromSettings(
    const PerMeshRenderSettings &settings,
    bool includeLighting = true)
{
    MainStyleUbufKey key;
    key.bboxWireColor = settings.bboxWireColor;
    key.pointColor = settings.pointColor;
    key.pointSize = settings.pointSize;
    key.wireColor = settings.wireColor;
    key.wireSize = settings.wireSize;
    key.fillColor = settings.fillColor;
    key.pointLighting = includeLighting ? settings.pointLighting : false;
    key.wireLighting = includeLighting ? settings.wireLighting : false;
    key.fillLighting = includeLighting ? settings.fillLighting : false;
    key.fillMaterial = settings.fillMaterial;
    key.fillPbr = settings.fillPbr;
    key.fillRs = settings.fillRs;
    key.fillPlain = settings.fillPlain;
    key.edgeColor = settings.edgeColor;
    key.edgeSize = settings.edgeSize;
    return key;
}

inline void writeMainMatricesToUbuf(
    float *ubufData,
    const QMatrix4x4 &mvp,
    const QMatrix4x4 &modelView,
    const QMatrix3x3 &normalMat)
{
    std::memcpy(ubufData, mvp.constData(), 16 * sizeof(float));
    std::memcpy(ubufData + 16, modelView.constData(), 16 * sizeof(float));

    const float *n = normalMat.constData();
    ubufData[32] = n[0]; ubufData[33] = n[1]; ubufData[34] = n[2]; ubufData[35] = 0.0f;
    ubufData[36] = n[3]; ubufData[37] = n[4]; ubufData[38] = n[5]; ubufData[39] = 0.0f;
    ubufData[40] = n[6]; ubufData[41] = n[7]; ubufData[42] = n[8]; ubufData[43] = 0.0f;
}

// QRhi takes OpenGL-style viewports, measured from the bottom-left of the render target,
// while every rectangle in the widget is measured from the top-left. The two agree for a
// viewport covering the whole target, which is why nothing had to convert until the view
// could be split into tiles.
// Colour of the gaps between tiles, which is what frames them: the pass clears the whole
// target to it and each tile then paints its own background inside its own viewport.
//
// Derived from the backdrop rather than configured, so it stays visible whatever the
// gradient is set to -- a fixed grey disappears against a grey background, and darkening
// disappears against a dark one. Pushing away from the backdrop's own luminance always
// leaves a line you can see without it shouting.
inline QColor tileFrameColorFor(const QColor &bottom, const QColor &top)
{
    const auto mid = [](qreal a, qreal b) { return 0.5 * (a + b); };
    const qreal r = mid(bottom.redF(), top.redF());
    const qreal g = mid(bottom.greenF(), top.greenF());
    const qreal b = mid(bottom.blueF(), top.blueF());
    const qreal luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const qreal towards = (luminance > 0.45) ? 0.0 : 1.0; // black against light, white against dark
    constexpr qreal kAmount = 0.28;
    return QColor::fromRgbF(
        r + (towards - r) * kAmount,
        g + (towards - g) * kAmount,
        b + (towards - b) * kAmount);
}

inline QRhiViewport rhiViewportFor(const QRect &rect, const QSize &targetPixelSize)
{
    return QRhiViewport(
        float(rect.x()),
        float(targetPixelSize.height() - rect.y() - rect.height()),
        float(qMax(1, rect.width())),
        float(qMax(1, rect.height())));
}

inline void writeMainStyleToUbuf(
    float *ubufData,
    const PerMeshRenderSettings &settings,
    const QSize &pixelSize,
    bool enableLighting,
    const QVector3D &lightDir = QVector3D(0.0f, 0.0f, 1.0f))
{
    ubufData[kUbufBBoxColorOffset + 0] = settings.bboxWireColor.redF();
    ubufData[kUbufBBoxColorOffset + 1] = settings.bboxWireColor.greenF();
    ubufData[kUbufBBoxColorOffset + 2] = settings.bboxWireColor.blueF();
    ubufData[kUbufBBoxColorOffset + 3] = settings.bboxWireColor.alphaF();

    ubufData[kUbufPointColorOffset + 0] = settings.pointColor.redF();
    ubufData[kUbufPointColorOffset + 1] = settings.pointColor.greenF();
    ubufData[kUbufPointColorOffset + 2] = settings.pointColor.blueF();
    ubufData[kUbufPointColorOffset + 3] = settings.pointColor.alphaF();
    ubufData[kUbufPointParamsOffset + 0] = settings.pointSize;

    ubufData[kUbufWireColorOffset + 0] = settings.wireColor.redF();
    ubufData[kUbufWireColorOffset + 1] = settings.wireColor.greenF();
    ubufData[kUbufWireColorOffset + 2] = settings.wireColor.blueF();
    // Wire pass is intentionally translucent to compose independently over fill/effects.
    ubufData[kUbufWireColorOffset + 3] = settings.wireColor.alphaF() * 0.7f;
    ubufData[kUbufWireParamsOffset + 0] = settings.wireSize;
    ubufData[kUbufWireParamsOffset + 1] = qMax(1.0f, settings.edgeSize);
    ubufData[kUbufWireParamsOffset + 2] = 1.0f / float(qMax(1, pixelSize.width()));
    ubufData[kUbufWireParamsOffset + 3] = 1.0f / float(qMax(1, pixelSize.height()));

    ubufData[kUbufFillColorOffset + 0] = settings.fillColor.redF();
    ubufData[kUbufFillColorOffset + 1] = settings.fillColor.greenF();
    ubufData[kUbufFillColorOffset + 2] = settings.fillColor.blueF();
    ubufData[kUbufFillColorOffset + 3] = settings.fillColor.alphaF();

    // bbox lighting removed: slot 0 intentionally unused/reserved.
    ubufData[kUbufLightingParamsOffset + 0] = 0.0f;
    ubufData[kUbufLightingParamsOffset + 1] = (enableLighting && settings.pointLighting) ? 1.0f : 0.0f;
    ubufData[kUbufLightingParamsOffset + 2] = (enableLighting && settings.wireLighting) ? 1.0f : 0.0f;
    ubufData[kUbufLightingParamsOffset + 3] = (enableLighting && settings.fillLighting) ? 1.0f : 0.0f;

    ubufData[kUbufEdgeColorOffset + 0] = settings.edgeColor.redF();
    ubufData[kUbufEdgeColorOffset + 1] = settings.edgeColor.greenF();
    ubufData[kUbufEdgeColorOffset + 2] = settings.edgeColor.blueF();
    ubufData[kUbufEdgeColorOffset + 3] = settings.edgeColor.alphaF();

    const bool enablePbr = settings.fillMaterial == FillMaterial::Pbr;
    const bool enableRs  = settings.fillMaterial == FillMaterial::RadianceScaling;
    auto encodePbrSource = [enablePbr](FillPbrTextureSource source) -> float {
        if (!enablePbr)
            return 0.0f;
        return static_cast<float>(static_cast<int>(source));
    };
    // materialFlags: PBR -> source modes (x=normal: 0 none, 1 constant, 2 tangent texture, 3 object texture,
    //                                    y=ao, z=roughness, w=albedo)
    //                RS  → invert flag (x) + display mode (y) + flat shading (z)
    if (enableRs) {
        ubufData[kUbufMaterialFlagsOffset + 0] = settings.fillRs.invert ? 1.0f : 0.0f;
        ubufData[kUbufMaterialFlagsOffset + 1] = static_cast<float>(settings.fillRs.displayMode);
        ubufData[kUbufMaterialFlagsOffset + 2] = (settings.fillRs.shading == FillShading::Flat) ? 1.0f : 0.0f;
        ubufData[kUbufMaterialFlagsOffset + 3] = 0.0f;
    } else {
        float normalMode = encodePbrSource(settings.fillPbr.normalSource);
        if (enablePbr
            && settings.fillPbr.normalSource == FillPbrTextureSource::Texture
            && settings.fillPbr.normalMapSpace == FillPbrNormalMapSpace::Object) {
            normalMode = 3.0f;
        }
        ubufData[kUbufMaterialFlagsOffset + 0] = normalMode;
        ubufData[kUbufMaterialFlagsOffset + 1] = encodePbrSource(settings.fillPbr.occlusionSource);
        ubufData[kUbufMaterialFlagsOffset + 2] = encodePbrSource(settings.fillPbr.roughnessSource);
        // PBR: encode the chosen albedo source.  Plain: signal texture usage via
        // the same slot so the shader can sample albedoTex in plain mode too.
        ubufData[kUbufMaterialFlagsOffset + 3] = enablePbr
            ? encodePbrSource(settings.fillPbr.albedoSource)
            : (settings.fillPlain.colorSource == FillColorSource::Texture ? 2.0f : 0.0f);
    }

    // materialParams: x=param0 (RS enhancement OR PBR normal scale), y=aoStrength, z=roughness, w=material-id
    ubufData[kUbufMaterialParamsOffset + 0] = enableRs ? settings.fillRs.enhancement : settings.fillPbr.normalScale;
    ubufData[kUbufMaterialParamsOffset + 1] = settings.fillPbr.occlusionStrength;
    ubufData[kUbufMaterialParamsOffset + 2] = std::max(settings.fillPbr.roughnessFactor, 0.0f);
    ubufData[kUbufMaterialParamsOffset + 3] = enablePbr ? 1.0f : 0.0f;

    // lightDir: view-space light direction (w unused)
    ubufData[kUbufLightDirOffset + 0] = lightDir.x();
    ubufData[kUbufLightDirOffset + 1] = lightDir.y();
    ubufData[kUbufLightDirOffset + 2] = lightDir.z();
    ubufData[kUbufLightDirOffset + 3] = 0.0f;
}

inline void writeMainMaterialOverridesToUbuf(
    float *ubufData,
    const MainUbufMaterialOverrides &overrides)
{
    ubufData[kUbufMaterialParamsOffset + 0] = overrides.normalScale;
    ubufData[kUbufMaterialParamsOffset + 1] = overrides.occlusionStrength;
    ubufData[kUbufMaterialParamsOffset + 2] = overrides.roughnessFactor;
}

inline void writeMainUbuf(
    float *ubufData,
    const QMatrix4x4 &mvp,
    const QMatrix4x4 &modelView,
    const QMatrix3x3 &normalMat,
    const PerMeshRenderSettings &settings,
    const QSize &pixelSize,
    bool enableLighting,
    const QVector3D &lightDir = QVector3D(0.0f, 0.0f, 1.0f),
    MainUbufMaterialOverrides overrides = MainUbufMaterialOverrides{})
{
    writeMainMatricesToUbuf(ubufData, mvp, modelView, normalMat);
    writeMainStyleToUbuf(ubufData, settings, pixelSize, enableLighting, lightDir);
    // Override fill color with per-mesh color if set
    if (overrides.fillColorOverride.isValid()) {
        ubufData[kUbufFillColorOffset + 0] = overrides.fillColorOverride.redF();
        ubufData[kUbufFillColorOffset + 1] = overrides.fillColorOverride.greenF();
        ubufData[kUbufFillColorOffset + 2] = overrides.fillColorOverride.blueF();
        ubufData[kUbufFillColorOffset + 3] = overrides.fillColorOverride.alphaF();
    }
    writeMainMaterialOverridesToUbuf(ubufData, overrides);
}

template <typename Batch>
inline bool hasDrawableBatchGeometry(const Batch &batch)
{
    return batch.vertexBuffer && (batch.indexCount > 0 || batch.vertexCount > 0);
}

template <typename Batch>
inline void drawBatchGeometry(QRhiCommandBuffer *cb, const Batch &batch)
{
    const QRhiCommandBuffer::VertexInput vb(batch.vertexBuffer, 0);
    if (batch.indexCount > 0 && batch.indexBuffer) {
        cb->setVertexInput(
            0, 1, &vb, batch.indexBuffer, 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(batch.indexCount);
    } else {
        cb->setVertexInput(0, 1, &vb);
        cb->draw(batch.vertexCount);
    }
}

inline QVector3D toVec3(const VCGMesh::CoordType &p)
{
    return QVector3D(p[0], p[1], p[2]);
}

inline float decodePackedDepthRgb8(const uchar *px, bool bgraOrder)
{
    const float c0 = (bgraOrder ? px[2] : px[0]) / 255.0f;
    const float c1 = px[1] / 255.0f;
    const float c2 = (bgraOrder ? px[0] : px[2]) / 255.0f;
    return c0 + c1 / 255.0f + c2 / 65025.0f;
}

inline std::vector<float> buildTrackballGizmoVertices()
{
    std::vector<float> v;
    v.reserve(kTrackballGizmoSteps * 2 * 3 * 6);

    auto append = [&v](const QVector3D &p, const QVector3D &c) {
        v.push_back(p.x());
        v.push_back(p.y());
        v.push_back(p.z());
        v.push_back(c.x());
        v.push_back(c.y());
        v.push_back(c.z());
    };

    auto emitCircle = [&](int axis, const QVector3D &color) {
        // axis: 0=XY(z=0), 1=YZ(x=0), 2=XZ(y=0)
        for (int i = 0; i < kTrackballGizmoSteps; ++i) {
            const float t0 = float(i) * 2.0f * kPi / float(kTrackballGizmoSteps);
            const float t1 = float(i + 1) * 2.0f * kPi / float(kTrackballGizmoSteps);
            QVector3D p0, p1;
            if (axis == 0) {
                p0 = QVector3D(std::cos(t0), std::sin(t0), 0.0f);
                p1 = QVector3D(std::cos(t1), std::sin(t1), 0.0f);
            } else if (axis == 1) {
                p0 = QVector3D(0.0f, std::cos(t0), std::sin(t0));
                p1 = QVector3D(0.0f, std::cos(t1), std::sin(t1));
            } else {
                p0 = QVector3D(std::cos(t0), 0.0f, std::sin(t0));
                p1 = QVector3D(std::cos(t1), 0.0f, std::sin(t1));
            }
            append(p0, color);
            append(p1, color);
        }
    };

    emitCircle(0, QVector3D(0.40f, 0.40f, 0.85f)); // XY - blue-ish
    emitCircle(1, QVector3D(0.40f, 0.85f, 0.40f)); // YZ - green-ish
    emitCircle(2, QVector3D(0.85f, 0.40f, 0.40f)); // XZ - red-ish
    return v;
}

inline const std::vector<float> &trackballGizmoVertices()
{
    static const std::vector<float> kVerts = buildTrackballGizmoVertices();
    return kVerts;
}

// Light gizmo geometry: rim circle + radial arrow toward projected light position.
// Vertex format: [x, y, z, r, g, b]  (Lines topology, 2 verts per segment)
//   z == 0 : rim circle, (x,y) on the unit circle
//   z == 1 : arrow, x = perpendicular offset, y = scale along L2 (0 = center, 1 = tip)
//            The shader multiplies y by L2 (= lightDir.xy, not normalised), so the
//            tip lands at the projected light position inside the rim circle.
inline std::vector<float> buildLightGizmoVertices()
{
    std::vector<float> v;
    const int N = kLightGizmoSteps;
    v.reserve((N + 4) * 2 * 6);

    auto append = [&v](const QVector3D &p, const QVector3D &c) {
        v.push_back(p.x()); v.push_back(p.y()); v.push_back(p.z());
        v.push_back(c.x()); v.push_back(c.y()); v.push_back(c.z());
    };

    // Rim circle (z = 0)
    const QVector3D rimColor(1.0f, 0.85f, 0.1f);
    for (int i = 0; i < N; ++i) {
        const float t0 = float(i)     * 2.0f * kPi / float(N);
        const float t1 = float(i + 1) * 2.0f * kPi / float(N);
        append(QVector3D(std::cos(t0), std::sin(t0), 0.0f), rimColor);
        append(QVector3D(std::cos(t1), std::sin(t1), 0.0f), rimColor);
    }

    // Arrow shaft: from center (y=0) to tip (y=1) along L2  (z = 1)
    const QVector3D arrowColor(1.0f, 0.5f, 0.0f);
    append(QVector3D(0.0f,  0.0f, 1.0f), arrowColor);   // centre of circle
    append(QVector3D(0.0f,  1.0f, 1.0f), arrowColor);   // projected light position

    // Arrowhead wings: perp offsets at y=0.75 toward the tip
    append(QVector3D(0.0f, 1.0f, 1.0f), arrowColor);
    append(QVector3D(-0.2f, 0.75f, 1.0f), arrowColor);
    append(QVector3D(0.0f, 1.0f, 1.0f), arrowColor);
    append(QVector3D( 0.2f, 0.75f, 1.0f), arrowColor);

    return v;
}

inline const std::vector<float> &lightGizmoVertices()
{
    static const std::vector<float> kVerts = buildLightGizmoVertices();
    return kVerts;
}

} // namespace RenderWidgetInternal

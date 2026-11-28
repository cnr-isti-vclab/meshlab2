#include "plugins/io_gltf/gltfimportplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"
#include "textureassociationutils.h"

#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QObject>
#include <QQuaternion>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

#define TINYGLTF_IMPLEMENTATION
// STB_IMAGE_IMPLEMENTATION deliberately absent: MeshLab2Core's stbimageimpl.cpp compiles
// stb_image for the whole project (readImageFile falls back to it for the formats Qt
// declines), and defining it twice collides at link. tinygltf still gets its
// declarations from the header and its definitions from core, which this plugin links.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#if defined(MESHLAB2_GLTF_HAS_DRACO)
#include <draco/attributes/geometry_attribute.h>
#include <draco/compression/encode.h>
#include <draco/core/encoder_buffer.h>
#include <draco/core/status.h>
#include <draco/mesh/mesh.h>
#endif

namespace {
constexpr int kErrOpen = -1;
constexpr int kErrParse = -2;
constexpr int kErrNoMesh = -3;
constexpr int kErrInvalidData = -4;
constexpr int kErrSaveUnsupported = -100;
constexpr int kErrSaveEmpty = -101;
constexpr int kErrSaveWrite = -102;

QString primitiveModeName(int mode)
{
    switch (mode) {
    case TINYGLTF_MODE_POINTS: return QStringLiteral("POINTS");
    case TINYGLTF_MODE_LINE: return QStringLiteral("LINES");
    case TINYGLTF_MODE_LINE_LOOP: return QStringLiteral("LINE_LOOP");
    case TINYGLTF_MODE_LINE_STRIP: return QStringLiteral("LINE_STRIP");
    case TINYGLTF_MODE_TRIANGLES: return QStringLiteral("TRIANGLES");
    case TINYGLTF_MODE_TRIANGLE_STRIP: return QStringLiteral("TRIANGLE_STRIP");
    case TINYGLTF_MODE_TRIANGLE_FAN: return QStringLiteral("TRIANGLE_FAN");
    default: break;
    }
    return QStringLiteral("MODE_%1").arg(mode);
}

void reportProgress(vcg::CallBackPos *cb, int pos, const QString &msg, bool replaceLast = false)
{
    if (!cb)
        return;
    const QByteArray raw = replaceLast
        ? (msg + QStringLiteral("\r")).toLocal8Bit()
        : msg.toLocal8Bit();
    cb(pos, raw.constData());
}

struct AccessorView {
    const unsigned char *data = nullptr;
    size_t stride = 0;
    size_t count = 0;
    int componentType = 0;
    int componentCount = 0;
    bool normalized = false;
    bool valid = false;
};

size_t componentSize(int componentType)
{
    switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return 1;
    case TINYGLTF_COMPONENT_TYPE_SHORT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return 2;
    case TINYGLTF_COMPONENT_TYPE_INT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return 4;
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
        return 8;
    default:
        return 0;
    }
}

double readComponentAsDouble(const unsigned char *ptr, int componentType, bool normalized)
{
    switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE: {
        const auto v = *reinterpret_cast<const int8_t *>(ptr);
        return normalized ? std::max(-1.0, double(v) / 127.0) : double(v);
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
        const auto v = *reinterpret_cast<const uint8_t *>(ptr);
        return normalized ? double(v) / 255.0 : double(v);
    }
    case TINYGLTF_COMPONENT_TYPE_SHORT: {
        const auto v = *reinterpret_cast<const int16_t *>(ptr);
        return normalized ? std::max(-1.0, double(v) / 32767.0) : double(v);
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
        const auto v = *reinterpret_cast<const uint16_t *>(ptr);
        return normalized ? double(v) / 65535.0 : double(v);
    }
    case TINYGLTF_COMPONENT_TYPE_INT: {
        const auto v = *reinterpret_cast<const int32_t *>(ptr);
        return normalized ? std::max(-1.0, double(v) / 2147483647.0) : double(v);
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
        const auto v = *reinterpret_cast<const uint32_t *>(ptr);
        return normalized ? double(v) / 4294967295.0 : double(v);
    }
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return double(*reinterpret_cast<const float *>(ptr));
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
        return *reinterpret_cast<const double *>(ptr);
    default:
        return 0.0;
    }
}

bool readIndex(const AccessorView &view, size_t index, uint32_t &outIndex)
{
    if (!view.valid || index >= view.count)
        return false;
    if (view.componentCount != 1)
        return false;

    const unsigned char *ptr = view.data + index * view.stride;
    switch (view.componentType) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        outIndex = *reinterpret_cast<const uint8_t *>(ptr);
        return true;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        outIndex = *reinterpret_cast<const uint16_t *>(ptr);
        return true;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        outIndex = *reinterpret_cast<const uint32_t *>(ptr);
        return true;
    default:
        return false;
    }
}

bool makeAccessorView(const tinygltf::Model &model, int accessorIndex, AccessorView &view)
{
    view = {};
    if (accessorIndex < 0 || accessorIndex >= int(model.accessors.size()))
        return false;

    const tinygltf::Accessor &accessor = model.accessors[accessorIndex];
    if (accessor.bufferView < 0 || accessor.bufferView >= int(model.bufferViews.size()))
        return false;

    const tinygltf::BufferView &bufferView = model.bufferViews[accessor.bufferView];
    if (bufferView.buffer < 0 || bufferView.buffer >= int(model.buffers.size()))
        return false;

    const tinygltf::Buffer &buffer = model.buffers[bufferView.buffer];
    const size_t compSize = componentSize(accessor.componentType);
    const int compCount = tinygltf::GetNumComponentsInType(accessor.type);
    if (compSize == 0 || compCount <= 0)
        return false;

    const size_t minStride = compSize * size_t(compCount);
    const size_t stride = accessor.ByteStride(bufferView) > 0
        ? size_t(accessor.ByteStride(bufferView))
        : minStride;
    if (stride < minStride)
        return false;

    const size_t baseOffset = size_t(bufferView.byteOffset) + size_t(accessor.byteOffset);
    if (baseOffset > buffer.data.size())
        return false;

    const size_t count = size_t(accessor.count);
    if (count > 0) {
        const size_t lastElementOffset = baseOffset + (count - 1) * stride;
        if (lastElementOffset + minStride > buffer.data.size())
            return false;
    }

    view.data = buffer.data.data() + baseOffset;
    view.stride = stride;
    view.count = count;
    view.componentType = accessor.componentType;
    view.componentCount = compCount;
    view.normalized = accessor.normalized;
    view.valid = true;
    return true;
}

QVector3D readVec3(const AccessorView &view, size_t index)
{
    if (!view.valid || index >= view.count || view.componentCount < 3)
        return {};
    const unsigned char *ptr = view.data + index * view.stride;
    const size_t step = componentSize(view.componentType);
    const float x = float(readComponentAsDouble(ptr + 0 * step, view.componentType, view.normalized));
    const float y = float(readComponentAsDouble(ptr + 1 * step, view.componentType, view.normalized));
    const float z = float(readComponentAsDouble(ptr + 2 * step, view.componentType, view.normalized));
    return { x, y, z };
}

QVector2D readVec2(const AccessorView &view, size_t index)
{
    if (!view.valid || index >= view.count || view.componentCount < 2)
        return {};
    const unsigned char *ptr = view.data + index * view.stride;
    const size_t step = componentSize(view.componentType);
    const float x = float(readComponentAsDouble(ptr + 0 * step, view.componentType, view.normalized));
    const float y = float(readComponentAsDouble(ptr + 1 * step, view.componentType, view.normalized));
    return { x, y };
}

QVector3D mulMat3Vec3(const QMatrix3x3 &m, const QVector3D &v)
{
    return QVector3D(
        m(0, 0) * v.x() + m(0, 1) * v.y() + m(0, 2) * v.z(),
        m(1, 0) * v.x() + m(1, 1) * v.y() + m(1, 2) * v.z(),
        m(2, 0) * v.x() + m(2, 1) * v.y() + m(2, 2) * v.z());
}

vcg::Color4b readColor4b(const AccessorView &view, size_t index, const vcg::Color4b &fallback)
{
    if (!view.valid || index >= view.count || view.componentCount < 3)
        return fallback;

    const unsigned char *ptr = view.data + index * view.stride;
    const size_t step = componentSize(view.componentType);

    const auto comp = [&](int i) -> float {
        const float v = float(readComponentAsDouble(ptr + size_t(i) * step, view.componentType, view.normalized));
        return view.normalized ? v : (v / 255.0f);
    };

    const float r = std::clamp(comp(0), 0.0f, 1.0f);
    const float g = std::clamp(comp(1), 0.0f, 1.0f);
    const float b = std::clamp(comp(2), 0.0f, 1.0f);
    const float a = (view.componentCount >= 4) ? std::clamp(comp(3), 0.0f, 1.0f) : 1.0f;

    return vcg::Color4b(
        uint8_t(std::round(r * 255.0f)),
        uint8_t(std::round(g * 255.0f)),
        uint8_t(std::round(b * 255.0f)),
        uint8_t(std::round(a * 255.0f)));
}

QMatrix4x4 nodeLocalMatrix(const tinygltf::Node &node)
{
    if (node.matrix.size() == 16) {
        float m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = float(node.matrix[size_t(i)]);
        return QMatrix4x4(m);
    }

    QMatrix4x4 m;
    m.setToIdentity();
    if (node.translation.size() == 3) {
        m.translate(
            float(node.translation[0]),
            float(node.translation[1]),
            float(node.translation[2]));
    }
    if (node.rotation.size() == 4) {
        const QQuaternion q(
            float(node.rotation[3]),
            float(node.rotation[0]),
            float(node.rotation[1]),
            float(node.rotation[2]));
        m.rotate(q.normalized());
    }
    if (node.scale.size() == 3) {
        m.scale(
            float(node.scale[0]),
            float(node.scale[1]),
            float(node.scale[2]));
    }
    return m;
}

QString normalizedExtension(const QString &filename)
{
    return QFileInfo(filename).suffix().trimmed().toLower();
}

bool isSupportedExtension(const QString &ext)
{
    return ext == QLatin1String("gltf") || ext == QLatin1String("glb");
}

size_t appendAndAlign4(std::vector<unsigned char> &dst, const void *src, size_t byteCount)
{
    const size_t offset = dst.size();
    if (byteCount > 0 && src != nullptr) {
        const auto *p = reinterpret_cast<const unsigned char *>(src);
        dst.insert(dst.end(), p, p + byteCount);
    }
    while ((dst.size() & size_t(3)) != 0)
        dst.push_back(0);
    return offset;
}

struct PrimitivePayload {
    int mode = TINYGLTF_MODE_TRIANGLES;
    int textureSlot = -1;
    bool hasNormals = false;
    bool hasColors = false;
    bool hasTexcoords = false;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> colors;
    std::vector<float> texcoords;
    std::vector<uint32_t> indices;
    // Output index of each distinct corner already written: the source vertex plus the
    // per-corner values that can split it (UV bits, override color). Without it every
    // face corner became its own glTF vertex and the file read back as a triangle soup.
    std::map<std::tuple<const VCGVertex *, uint32_t, uint32_t, uint32_t>, uint32_t> written;
};

constexpr int kDracoPositionQuantizationBits = 14;
constexpr int kDracoNormalQuantizationBits = 10;
constexpr int kDracoTexcoordQuantizationBits = 12;
constexpr int kDracoColorQuantizationBits = 8;

bool modelUsesDracoCompression(const tinygltf::Model &model)
{
    for (const tinygltf::Mesh &mesh : model.meshes) {
        for (const tinygltf::Primitive &prim : mesh.primitives) {
            if (prim.extensions.find("KHR_draco_mesh_compression") != prim.extensions.end())
                return true;
        }
    }
    return false;
}

#if defined(MESHLAB2_GLTF_HAS_DRACO)
struct DracoEncodedPrimitive {
    std::vector<unsigned char> bytes;
    std::unordered_map<std::string, int> attributeUniqueIds;
    QString error;
    bool valid = false;
};

bool addDracoFloatAttribute(
    draco::Mesh &mesh,
    draco::GeometryAttribute::Type attributeType,
    int componentCount,
    const std::vector<float> &values,
    uint32_t vertexCount,
    int &outUniqueId)
{
    if (componentCount <= 0)
        return false;
    if (values.size() != size_t(vertexCount) * size_t(componentCount))
        return false;

    draco::GeometryAttribute attr;
    attr.Init(
        attributeType,
        nullptr,
        componentCount,
        draco::DT_FLOAT32,
        false,
        int64_t(sizeof(float) * size_t(componentCount)),
        0);

    const int attrId = mesh.AddAttribute(attr, true, vertexCount);
    if (attrId < 0)
        return false;
    draco::PointAttribute *pointAttr = mesh.attribute(attrId);
    if (!pointAttr)
        return false;

    for (uint32_t i = 0; i < vertexCount; ++i) {
        pointAttr->SetAttributeValue(
            pointAttr->mapped_index(draco::PointIndex(i)),
            values.data() + size_t(i) * size_t(componentCount));
    }

    outUniqueId = pointAttr->unique_id();
    return true;
}

bool encodePrimitiveWithDraco(
    const PrimitivePayload &prim,
    const MeshIOSaveOptions &options,
    DracoEncodedPrimitive &out)
{
    out = {};
    if (prim.mode != TINYGLTF_MODE_TRIANGLES)
        return false;
    if (prim.positions.empty() || (prim.positions.size() % 3) != 0)
        return false;
    if (prim.indices.empty() || (prim.indices.size() % 3) != 0)
        return false;

    const uint32_t vertexCount = uint32_t(prim.positions.size() / 3);
    const uint32_t triangleCount = uint32_t(prim.indices.size() / 3);
    if (vertexCount == 0 || triangleCount == 0)
        return false;

    // The point count must be set before the attributes are added: the encoder sizes its value
    // buffers from it, and with none set every attribute encoder read and wrote through empty
    // (null) buffers while the faces named points 0..vertexCount-1, a crash at every level.
    draco::Mesh mesh;
    mesh.set_num_points(vertexCount);
    mesh.SetNumFaces(triangleCount);
    for (uint32_t ti = 0; ti < triangleCount; ++ti) {
        const uint32_t i0 = prim.indices[size_t(ti) * 3 + 0];
        const uint32_t i1 = prim.indices[size_t(ti) * 3 + 1];
        const uint32_t i2 = prim.indices[size_t(ti) * 3 + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            out.error = QObject::tr("draco encode failed: primitive index out of bounds.");
            return false;
        }
        draco::Mesh::Face face;
        face[0] = draco::PointIndex(i0);
        face[1] = draco::PointIndex(i1);
        face[2] = draco::PointIndex(i2);
        mesh.SetFace(draco::FaceIndex(ti), face);
    }

    int positionUniqueId = -1;
    if (!addDracoFloatAttribute(
            mesh,
            draco::GeometryAttribute::POSITION,
            3,
            prim.positions,
            vertexCount,
            positionUniqueId)) {
        out.error = QObject::tr("draco encode failed: POSITION attribute setup.");
        return false;
    }
    out.attributeUniqueIds["POSITION"] = positionUniqueId;

    if (prim.hasNormals && prim.normals.size() == prim.positions.size()) {
        int normalUniqueId = -1;
        if (!addDracoFloatAttribute(
                mesh,
                draco::GeometryAttribute::NORMAL,
                3,
                prim.normals,
                vertexCount,
                normalUniqueId)) {
            out.error = QObject::tr("draco encode failed: NORMAL attribute setup.");
            return false;
        }
        out.attributeUniqueIds["NORMAL"] = normalUniqueId;
    }

    if (prim.hasColors && (prim.colors.size() / 4) == (prim.positions.size() / 3)) {
        int colorUniqueId = -1;
        if (!addDracoFloatAttribute(
                mesh,
                draco::GeometryAttribute::COLOR,
                4,
                prim.colors,
                vertexCount,
                colorUniqueId)) {
            out.error = QObject::tr("draco encode failed: COLOR_0 attribute setup.");
            return false;
        }
        out.attributeUniqueIds["COLOR_0"] = colorUniqueId;
    }

    if (prim.hasTexcoords && (prim.texcoords.size() / 2) == (prim.positions.size() / 3)) {
        int texcoordUniqueId = -1;
        if (!addDracoFloatAttribute(
                mesh,
                draco::GeometryAttribute::TEX_COORD,
                2,
                prim.texcoords,
                vertexCount,
                texcoordUniqueId)) {
            out.error = QObject::tr("draco encode failed: TEXCOORD_0 attribute setup.");
            return false;
        }
        out.attributeUniqueIds["TEXCOORD_0"] = texcoordUniqueId;
    }

    draco::Encoder encoder;
    const int compressionLevel = std::clamp(options.dracoCompressionLevel, 0, 10);
    const int speed = 10 - compressionLevel;
    encoder.SetSpeedOptions(speed, speed);
    encoder.SetAttributeQuantization(
        draco::GeometryAttribute::POSITION, kDracoPositionQuantizationBits);
    if (out.attributeUniqueIds.find("NORMAL") != out.attributeUniqueIds.end())
        encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, kDracoNormalQuantizationBits);
    if (out.attributeUniqueIds.find("TEXCOORD_0") != out.attributeUniqueIds.end())
        encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, kDracoTexcoordQuantizationBits);
    if (out.attributeUniqueIds.find("COLOR_0") != out.attributeUniqueIds.end())
        encoder.SetAttributeQuantization(draco::GeometryAttribute::COLOR, kDracoColorQuantizationBits);

    draco::EncoderBuffer buffer;
    const draco::Status status = encoder.EncodeMeshToBuffer(mesh, &buffer);
    if (!status.ok()) {
        out.error = QObject::tr("draco encode failed: %1")
                        .arg(QString::fromStdString(status.error_msg()));
        return false;
    }

    const auto *encoded =
        reinterpret_cast<const unsigned char *>(buffer.data());
    out.bytes.assign(encoded, encoded + buffer.size());
    out.valid = !out.bytes.empty();
    if (!out.valid)
        out.error = QObject::tr("draco encode failed: empty output buffer.");
    return out.valid;
}
#endif

uint32_t appendPrimitiveVertex(
    PrimitivePayload &payload,
    const VCGVertex *v,
    const vcg::Color4b &overrideColor,
    bool useOverrideColor,
    bool writeNormal,
    bool writeColor,
    bool writeTexcoord,
    float texU,
    float texV)
{
    uint32_t uBits = 0, vBits = 0;
    if (writeTexcoord) {
        std::memcpy(&uBits, &texU, sizeof(uBits));
        std::memcpy(&vBits, &texV, sizeof(vBits));
    }
    uint32_t colorBits = 0;
    if (writeColor && useOverrideColor)
        std::memcpy(&colorBits, &overrideColor[0], sizeof(colorBits));
    const uint32_t idx = uint32_t(payload.positions.size() / 3);
    const auto [it, inserted] = payload.written.try_emplace({v, uBits, vBits, colorBits}, idx);
    if (!inserted)
        return it->second;

    const auto p = v->cP();
    payload.positions.push_back(float(p[0]));
    payload.positions.push_back(float(p[1]));
    payload.positions.push_back(float(p[2]));

    if (writeNormal) {
        const auto n = v->cN();
        payload.normals.push_back(float(n[0]));
        payload.normals.push_back(float(n[1]));
        payload.normals.push_back(float(n[2]));
    }

    if (writeColor) {
        const vcg::Color4b c = useOverrideColor ? overrideColor : v->cC();
        payload.colors.push_back(float(c[0]) / 255.0f);
        payload.colors.push_back(float(c[1]) / 255.0f);
        payload.colors.push_back(float(c[2]) / 255.0f);
        payload.colors.push_back(float(c[3]) / 255.0f);
    }

    if (writeTexcoord) {
        payload.texcoords.push_back(texU);
        payload.texcoords.push_back(texV);
    }

    return idx;
}

int addBufferView(
    tinygltf::Model &model,
    std::vector<unsigned char> &buffer,
    const void *src,
    size_t byteCount,
    int target)
{
    tinygltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = appendAndAlign4(buffer, src, byteCount);
    view.byteLength = byteCount;
    if (target != 0)
        view.target = target;
    model.bufferViews.push_back(std::move(view));
    return int(model.bufferViews.size() - 1);
}

int addAccessor(
    tinygltf::Model &model,
    int bufferViewIndex,
    int componentType,
    size_t count,
    int type,
    bool normalized = false)
{
    tinygltf::Accessor accessor;
    accessor.bufferView = bufferViewIndex;
    accessor.byteOffset = 0;
    accessor.componentType = componentType;
    accessor.count = count;
    accessor.type = type;
    accessor.normalized = normalized;
    model.accessors.push_back(std::move(accessor));
    return int(model.accessors.size() - 1);
}

void setAccessorMinMaxVec3(tinygltf::Accessor &accessor, const std::vector<float> &positions)
{
    if (positions.size() < 3)
        return;
    float minX = positions[0], minY = positions[1], minZ = positions[2];
    float maxX = positions[0], maxY = positions[1], maxZ = positions[2];
    for (size_t i = 3; i + 2 < positions.size(); i += 3) {
        minX = std::min(minX, positions[i + 0]);
        minY = std::min(minY, positions[i + 1]);
        minZ = std::min(minZ, positions[i + 2]);
        maxX = std::max(maxX, positions[i + 0]);
        maxY = std::max(maxY, positions[i + 1]);
        maxZ = std::max(maxZ, positions[i + 2]);
    }
    accessor.minValues = { double(minX), double(minY), double(minZ) };
    accessor.maxValues = { double(maxX), double(maxY), double(maxZ) };
}

// One channel of a pixel: a named one, or the mean of the color channels for RGB.
int channelValue(QRgb p, MeshIOTextureChannel channel)
{
    switch (channel) {
    case MeshIOTextureChannel::R: return qRed(p);
    case MeshIOTextureChannel::G: return qGreen(p);
    case MeshIOTextureChannel::B: return qBlue(p);
    case MeshIOTextureChannel::A: return qAlpha(p);
    case MeshIOTextureChannel::RGB: break;
    }
    return (qRed(p) + qGreen(p) + qBlue(p)) / 3;
}

// An image whose channel `to` holds channel `from` of a (and, when b is given, whose other
// target is filled from b the same way), the rest white: how a value glTF reads from a fixed
// channel is written when the material read it from another. b is scaled to a's size.
QImage packChannels(const QImage &a, MeshIOTextureChannel from, const QImage &b, MeshIOTextureChannel bFrom,
    MeshIOTextureChannel to, MeshIOTextureChannel bTo = MeshIOTextureChannel::RGB)
{
    const QImage first = a.isNull() ? b : a;
    if (first.isNull())
        return {};
    QImage out(first.size(), QImage::Format_ARGB32);
    out.fill(Qt::white);
    const auto fill = [&](const QImage &source, MeshIOTextureChannel src, MeshIOTextureChannel dst) {
        if (source.isNull() || dst == MeshIOTextureChannel::RGB)
            return;
        const QImage img = source.size() == out.size() ? source.convertToFormat(QImage::Format_ARGB32)
            : source.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < out.height(); ++y) {
            const QRgb *in = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < out.width(); ++x) {
                const int v = channelValue(in[x], src);
                const QRgb p = line[x];
                line[x] = qRgba(dst == MeshIOTextureChannel::R ? v : qRed(p), dst == MeshIOTextureChannel::G ? v : qGreen(p),
                    dst == MeshIOTextureChannel::B ? v : qBlue(p), dst == MeshIOTextureChannel::A ? v : qAlpha(p));
            }
        }
    };
    fill(a, from, to);
    fill(b, bFrom, bTo);
    return out;
}

// glTF's metallic-roughness image: roughness in green, metallic in blue, either white (the
// factor alone) when the material has no map for it.
QImage packMetallicRoughness(const QImage &roughness, MeshIOTextureChannel roughnessChannel, const QImage &metallic,
    MeshIOTextureChannel metallicChannel)
{
    return packChannels(roughness, roughnessChannel, metallic, metallicChannel, MeshIOTextureChannel::G,
        MeshIOTextureChannel::B);
}

QString makeCopiedTextureUri(
    const QString &targetFilePath,
    const QString &sourceTexturePath,
    vcg::CallBackPos *cb,
    int textureSlot)
{
    if (sourceTexturePath.isEmpty())
        return QString();

    const QFileInfo sourceInfo(sourceTexturePath);
    const QString sourceAbs = sourceInfo.absoluteFilePath();
    if (!QFileInfo::exists(sourceAbs)) {
        reportProgress(
            cb,
            0,
            QObject::tr("glTF export warning: texture #%1 not found: %2")
                .arg(textureSlot)
                .arg(sourceTexturePath));
        return QString();
    }

    const QDir outDir = QFileInfo(targetFilePath).absoluteDir();
    QString fileName = sourceInfo.fileName();
    if (fileName.isEmpty())
        fileName = QStringLiteral("texture_%1.png").arg(textureSlot);

    QString candidateName = fileName;
    QString destPath = outDir.filePath(candidateName);
    int suffixCounter = 1;
    while (QFileInfo::exists(destPath) && QFileInfo(destPath).absoluteFilePath() != sourceAbs) {
        const QFileInfo candidateInfo(fileName);
        const QString base = candidateInfo.completeBaseName().isEmpty()
            ? QStringLiteral("texture_%1").arg(textureSlot)
            : candidateInfo.completeBaseName();
        const QString ext = candidateInfo.suffix();
        candidateName = ext.isEmpty()
            ? QStringLiteral("%1_%2").arg(base).arg(suffixCounter++)
            : QStringLiteral("%1_%2.%3").arg(base).arg(suffixCounter++).arg(ext);
        destPath = outDir.filePath(candidateName);
    }

    if (QFileInfo(destPath).absoluteFilePath() != sourceAbs) {
        if (!QFile::copy(sourceAbs, destPath)) {
            reportProgress(
                cb,
                0,
                QObject::tr("glTF export warning: failed copying texture #%1 to %2")
                    .arg(textureSlot)
                    .arg(destPath));
            return QString();
        }
    }

    return candidateName;
}

bool fillTinyGltfImage(
    tinygltf::Image &outImage,
    QImage image,
    int textureSlot,
    vcg::CallBackPos *cb,
    const QString &source = QString())
{
    if (image.isNull()) {
        reportProgress(
            cb,
            0,
            QObject::tr("glTF export warning: failed loading texture #%1%2")
                .arg(textureSlot)
                .arg(source.isEmpty() ? QString() : QObject::tr(" from %1").arg(source)));
        return false;
    }

    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return false;

    outImage.name = QStringLiteral("texture_%1").arg(textureSlot).toStdString();
    outImage.mimeType = "image/png";
    outImage.width = image.width();
    outImage.height = image.height();
    outImage.component = 4;
    outImage.bits = 8;
    outImage.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

    const size_t packedStride = size_t(image.width()) * 4;
    outImage.image.resize(packedStride * size_t(image.height()));
    const unsigned char *src = image.constBits();
    const int srcStride = image.bytesPerLine();
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(
            outImage.image.data() + size_t(y) * packedStride,
            src + size_t(y) * size_t(srcStride),
            packedStride);
    }
    return true;
}

bool fillTinyGltfImageFromFile(
    tinygltf::Image &outImage,
    const QString &texturePath,
    int textureSlot,
    vcg::CallBackPos *cb)
{
    QImage image;
    QString imageError;
    TextureAssociationUtils::readImageFile(texturePath, image, imageError);
    return fillTinyGltfImage(outImage, image, textureSlot, cb, texturePath);
}

QString saveTextureImage(
    const QString &targetFilePath,
    const QImage &image,
    const QString &preferredName,
    int textureSlot,
    vcg::CallBackPos *cb)
{
    const QDir outDir = QFileInfo(targetFilePath).absoluteDir();
    QString base = QFileInfo(preferredName).completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("texture_%1").arg(textureSlot);
    QString name = base + QStringLiteral(".png");
    for (int suffix = 1; QFileInfo::exists(outDir.filePath(name)); ++suffix)
        name = QStringLiteral("%1_%2.png").arg(base).arg(suffix);
    if (image.save(outDir.filePath(name), "PNG"))
        return name;
    reportProgress(cb, 0,
        QObject::tr("glTF export warning: failed writing texture #%1").arg(textureSlot));
    return QString();
}

class GLTFImportPlugin final : public MeshIOPlugin
{
public:
    QString pluginId() const override
    {
        return QStringLiteral("io_gltf");
    }

    QString name() const override
    {
        return QObject::tr("glTF Import/Export (tinygltf)");
    }

    QStringList supportedExtensions() const override
    {
        return { QStringLiteral("gltf"), QStringLiteral("glb") };
    }

    bool canLoad(const QString &filename) const override
    {
        return isSupportedExtension(normalizedExtension(filename));
    }

    MeshIOCapabilities loadCapabilities(const QString &) const override
    {
        using M = vcg::tri::io::Mask;
        return { M::IOM_VERTCOORD | M::IOM_VERTCOLOR | M::IOM_VERTNORMAL
                | M::IOM_EDGEINDEX | M::IOM_FACEINDEX | M::IOM_WEDGTEXCOORD
                | M::IOM_WEDGTEXMULTI,
            true, true };
    }

    MeshIOCapabilities saveCapabilities(const QString &filename) const override
    {
        return { saveMaskCapability(filename), true, true };
    }

    bool canSave(const QString &filename) const override
    {
        return isSupportedExtension(normalizedExtension(filename));
    }

    // glTF is Y-up by definition; MeshLab keeps its frame unless asked.
    std::vector<MeshFilterParameterDescriptor> loadParameters(const QString &) const override
    {
        return { UpAxisOption::descriptor() };
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask) const override
    {
        return load(filename, mesh, cb, outLoadMask, nullptr);
    }

    int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet,
        MeshIOLoadContext &context) const override
    {
        const int err = load(filename, mesh, cb, outLoadMask, outMaterialSet);
        // The glTF specification measures every distance in meters, with +Y up.
        if (err == 0) {
            context.unit = LengthUnit::Meter;
            UpAxisOption::apply(mesh, true, context);
        }
        return err;
    }

    int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet) const override
    {
        if (outLoadMask)
            *outLoadMask = 0;
        if (outMaterialSet)
            outMaterialSet->clear();

        reportProgress(cb, 0, QObject::tr("Reading glTF file: %1").arg(QFileInfo(filename).fileName()), true);

        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string warn;
        std::string err;

        const QString ext = QFileInfo(filename).suffix().toLower();
        const bool ok = (ext == QLatin1String("glb"))
            ? loader.LoadBinaryFromFile(&model, &warn, &err, filename.toStdString())
            : loader.LoadASCIIFromFile(&model, &warn, &err, filename.toStdString());
        if (!warn.empty()) {
            const QStringList warnLines =
                QString::fromStdString(warn).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
            for (const QString &line : warnLines)
                reportProgress(cb, 0, QObject::tr("glTF warning: %1").arg(line.trimmed()));
        }
        if (!ok) {
            const QString errorText = QString::fromStdString(err).trimmed();
            if (!errorText.isEmpty())
                reportProgress(cb, 0, QObject::tr("glTF load error: %1").arg(errorText));
            else
                reportProgress(cb, 0, QObject::tr("glTF load error: unknown loader failure."));
            return QFileInfo::exists(filename) ? kErrParse : kErrOpen;
        }
        if (model.meshes.empty()) {
            reportProgress(cb, 0, QObject::tr("glTF error: document has no meshes."));
            return kErrNoMesh;
        }

        int primitiveCount = 0;
        int trianglePrimitiveCount = 0;
        int pointPrimitiveCount = 0;
        int linePrimitiveCount = 0;
        int otherPrimitiveCount = 0;
        int primitivesWithPosition = 0;
        int primitivesWithNormal = 0;
        int primitivesWithTexcoord = 0;
        int primitivesWithColor = 0;
        int primitivesWithTangent = 0;
        std::map<int, int> primitiveModes;
        for (const tinygltf::Mesh &srcMesh : model.meshes) {
            for (const tinygltf::Primitive &prim : srcMesh.primitives) {
                ++primitiveCount;
                ++primitiveModes[prim.mode];
                switch (prim.mode) {
                case TINYGLTF_MODE_TRIANGLES:
                    ++trianglePrimitiveCount;
                    break;
                case TINYGLTF_MODE_POINTS:
                    ++pointPrimitiveCount;
                    break;
                case TINYGLTF_MODE_LINE:
                case TINYGLTF_MODE_LINE_LOOP:
                case TINYGLTF_MODE_LINE_STRIP:
                    ++linePrimitiveCount;
                    break;
                default:
                    ++otherPrimitiveCount;
                    break;
                }
                const auto hasAttr = [&](const char *name) {
                    return prim.attributes.find(name) != prim.attributes.end();
                };
                if (hasAttr("POSITION"))
                    ++primitivesWithPosition;
                if (hasAttr("NORMAL"))
                    ++primitivesWithNormal;
                if (hasAttr("TEXCOORD_0"))
                    ++primitivesWithTexcoord;
                if (hasAttr("COLOR_0"))
                    ++primitivesWithColor;
                if (hasAttr("TANGENT"))
                    ++primitivesWithTangent;
            }
        }

        reportProgress(
            cb,
            0,
            QObject::tr(
                "glTF source info: scenes=%1 nodes=%2 meshes=%3 primitives=%4 (tri=%5 points=%6 lines=%7 other=%8) materials=%9 textures=%10 images=%11 accessors=%12 bufferViews=%13")
                .arg(model.scenes.size())
                .arg(model.nodes.size())
                .arg(model.meshes.size())
                .arg(primitiveCount)
                .arg(trianglePrimitiveCount)
                .arg(pointPrimitiveCount)
                .arg(linePrimitiveCount)
                .arg(otherPrimitiveCount)
                .arg(model.materials.size())
                .arg(model.textures.size())
                .arg(model.images.size())
                .arg(model.accessors.size())
                .arg(model.bufferViews.size()));
        reportProgress(
            cb,
            0,
            QObject::tr(
                "glTF source attributes (primitive count): POSITION=%1 NORMAL=%2 TEXCOORD_0=%3 COLOR_0=%4 TANGENT=%5")
                .arg(primitivesWithPosition)
                .arg(primitivesWithNormal)
                .arg(primitivesWithTexcoord)
                .arg(primitivesWithColor)
                .arg(primitivesWithTangent));
        if (!primitiveModes.empty()) {
            QStringList modeItems;
            modeItems.reserve(int(primitiveModes.size()));
            for (const auto &kv : primitiveModes) {
                modeItems.push_back(
                    QStringLiteral("%1:%2").arg(primitiveModeName(kv.first)).arg(kv.second));
            }
            reportProgress(
                cb,
                0,
                QObject::tr("glTF primitive modes: %1")
                    .arg(modeItems.join(QStringLiteral(", "))));
        }

        if (!model.extensionsRequired.empty()) {
            QStringList extensions;
            extensions.reserve(int(model.extensionsRequired.size()));
            for (const std::string &extName : model.extensionsRequired)
                extensions.push_back(QString::fromStdString(extName));
            reportProgress(
                cb,
                0,
                QObject::tr("glTF required extensions: %1")
                    .arg(extensions.join(QStringLiteral(", "))));
        }
        if (!model.extensionsUsed.empty()) {
            QStringList extensions;
            extensions.reserve(int(model.extensionsUsed.size()));
            for (const std::string &extName : model.extensionsUsed)
                extensions.push_back(QString::fromStdString(extName));
            reportProgress(
                cb,
                0,
                QObject::tr("glTF used extensions: %1")
                    .arg(extensions.join(QStringLiteral(", "))));
        }
        const QDir inputDir = QFileInfo(filename).absoluteDir();

        for (size_t ti = 0; ti < model.textures.size(); ++ti) {
            const tinygltf::Texture &texture = model.textures[ti];
            const int imageIndex = texture.source;
            QString imageInfo = QObject::tr("no image");
            QString textureNameFallback;
            if (imageIndex >= 0 && imageIndex < int(model.images.size())) {
                const tinygltf::Image &img = model.images[size_t(imageIndex)];
                QString uri = QUrl::fromPercentEncoding(QString::fromStdString(img.uri).toUtf8());
                QString sourceKind = QObject::tr("embedded");
                textureNameFallback = QString::fromStdString(img.name).trimmed();
                if (uri.trimmed().isEmpty()) {
                    if (!img.image.empty())
                        uri = QObject::tr("embedded");
                    else if (img.bufferView >= 0)
                        uri = QObject::tr("bufferView #%1").arg(img.bufferView);
                    else
                        uri = QObject::tr("unspecified");
                }
                imageInfo = QObject::tr("img=%1 %2x%3 comp=%4 bits=%5 src=%6")
                                .arg(imageIndex)
                                .arg(img.width)
                                .arg(img.height)
                                .arg(img.component)
                                .arg(img.bits)
                                .arg(uri);
                imageInfo += QObject::tr(" kind=%1").arg(sourceKind);
            }
            QString texName = QString::fromStdString(texture.name).trimmed();
            if (texName.isEmpty())
                texName = textureNameFallback;
            if (texName.isEmpty() && imageIndex >= 0)
                texName = QObject::tr("Image %1").arg(imageIndex);
            if (texName.isEmpty())
                texName = QObject::tr("Texture %1").arg(ti);
            reportProgress(
                cb,
                0,
                QObject::tr("glTF texture %1: '%2' (%3)")
                    .arg(ti)
                    .arg(texName)
                    .arg(imageInfo));
        }
        for (size_t mi = 0; mi < model.materials.size(); ++mi) {
            const tinygltf::Material &mat = model.materials[mi];
            const int baseTex = mat.pbrMetallicRoughness.baseColorTexture.index;
            const int normalTex = mat.normalTexture.index;
            const int occlusionTex = mat.occlusionTexture.index;
            const int roughTex = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
            QString matName = QString::fromStdString(mat.name).trimmed();
            if (matName.isEmpty())
                matName = QObject::tr("Material %1").arg(mi);
            reportProgress(
                cb,
                0,
                QObject::tr(
                    "glTF material %1: '%2' baseTex=%3 normalTex=%4 occlusionTex=%5 roughnessTex=%6 roughnessFactor=%7")
                    .arg(mi)
                    .arg(matName)
                    .arg(baseTex)
                    .arg(normalTex)
                    .arg(occlusionTex)
                    .arg(roughTex)
                    .arg(mat.pbrMetallicRoughness.roughnessFactor, 0, 'f', 3));
        }

#if !defined(MESHLAB2_GLTF_HAS_DRACO)
        if (modelUsesDracoCompression(model)) {
            reportProgress(
                cb,
                0,
                QObject::tr("glTF uses KHR_draco_mesh_compression but this MeshLab build has no Draco support."));
        }
#endif

        reportProgress(cb, 5, QObject::tr("Parsing glTF scene graph..."), true);

        mesh.Clear();
        mesh.textures.clear();

        int loadMask = 0;
        bool hasVertexNormals = false;
        bool hasEdges = false;
        bool hasVertexColors = false;
        bool hasWedgeTexCoords = false;
        int skippedUnsupportedPrimitiveCount = 0;
        int skippedMissingPositionCount = 0;
        int skippedInvalidPositionAccessorCount = 0;
        int textureDecodeFailureCount = 0;
        int textureMissingFileCount = 0;

        // -- Texture resolution ------------------------------------------------

        // Images embedded in the file, decoded, by glTF image index; placed in the material
        // set's asset list once the slots are known.
        std::unordered_map<int, MeshIOTextureAsset> embeddedImages;
        std::unordered_map<int, QString> imageToResolvedUri;
        std::vector<std::string> textureFiles;
        MeshIOMaterialSet materialSet;
        materialSet.entries.reserve(model.materials.size());

        auto resolveImageUri = [&](int imageIndex) -> QString {
            if (imageIndex < 0 || imageIndex >= int(model.images.size()))
                return QString();
            const auto cached = imageToResolvedUri.find(imageIndex);
            if (cached != imageToResolvedUri.end())
                return cached->second;

            const tinygltf::Image &img = model.images[size_t(imageIndex)];
            QString uri = QString::fromStdString(img.uri);
            uri = QUrl::fromPercentEncoding(uri.toUtf8());

            const bool isDataUri = uri.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive);
            if (uri.isEmpty() || isDataUri) {
                if (img.image.empty() || img.width <= 0 || img.height <= 0) {
                    ++textureDecodeFailureCount;
                    reportProgress(
                        cb,
                        0,
                        QObject::tr("glTF warning: embedded texture #%1 has no valid pixel data.")
                            .arg(imageIndex));
                    return QString();
                }

                QImage qimg;
                if (img.bits == 8 && img.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    if (img.component == 4) {
                        qimg = QImage(img.image.data(), img.width, img.height, QImage::Format_RGBA8888).copy();
                    } else if (img.component == 3) {
                        qimg = QImage(img.image.data(), img.width, img.height, QImage::Format_RGB888).copy();
                    } else if (img.component == 1) {
                        qimg = QImage(img.image.data(), img.width, img.height, QImage::Format_Grayscale8).copy();
                    }
                }
                if (qimg.isNull()) {
                    ++textureDecodeFailureCount;
                    reportProgress(
                        cb,
                        0,
                        QObject::tr("glTF warning: unsupported embedded texture format for image #%1 (components=%2, bits=%3, pixelType=%4).")
                            .arg(imageIndex)
                            .arg(img.component)
                            .arg(img.bits)
                            .arg(img.pixel_type));
                    return QString();
                }

                const QString baseName = QFileInfo(filename).completeBaseName();
                QString imageName = QString::fromStdString(img.name).trimmed();
                if (imageName.isEmpty())
                    imageName = QStringLiteral("%1_img_%2.png").arg(baseName).arg(imageIndex);
                MeshIOTextureAsset asset;
                asset.name = QFileInfo(imageName).fileName();
                asset.image = std::move(qimg);
                uri = asset.name;
                embeddedImages[imageIndex] = std::move(asset);
            } else {
                QString resolvedTexturePath = uri;
                if (QFileInfo(uri).isRelative())
                    resolvedTexturePath = inputDir.filePath(uri);
                if (!QFileInfo::exists(resolvedTexturePath)) {
                    ++textureMissingFileCount;
                    reportProgress(
                        cb,
                        0,
                        QObject::tr("glTF warning: texture file not found '%1' (resolved: '%2').")
                            .arg(uri)
                            .arg(resolvedTexturePath));
                    return QString();
                }
                uri = resolvedTexturePath;
            }

            imageToResolvedUri[imageIndex] = uri;
            return uri;
        };

        auto imageOfTexture = [&](int texIndex) -> int {
            return texIndex >= 0 && texIndex < int(model.textures.size()) ? model.textures[size_t(texIndex)].source : -1;
        };

        constexpr int kMaxTextureEntriesInLog = 20;
        if (model.textures.empty()) {
            reportProgress(cb, 10, QObject::tr("glTF texture files: none"));
        } else {
            QStringList textureFileEntries;
            textureFileEntries.reserve(std::min<size_t>(model.textures.size(), kMaxTextureEntriesInLog));
            for (size_t ti = 0; ti < model.textures.size(); ++ti) {
                const tinygltf::Texture &texture = model.textures[ti];
                const int imageIndex = texture.source;
                const QString resolvedUri = resolveImageUri(imageIndex);

                QString displayName = QString::fromStdString(texture.name).trimmed();
                if (displayName.isEmpty() && imageIndex >= 0 && imageIndex < int(model.images.size())) {
                    const tinygltf::Image &img = model.images[size_t(imageIndex)];
                    displayName = QString::fromStdString(img.name).trimmed();
                }
                if (displayName.isEmpty() && !resolvedUri.trimmed().isEmpty())
                    displayName = QFileInfo(resolvedUri).fileName().trimmed();
                if (displayName.isEmpty())
                    displayName = QObject::tr("Texture %1").arg(ti);

                const bool embedded = embeddedImages.count(imageIndex) > 0;
                const QString status = resolvedUri.trimmed().isEmpty()
                    ? QObject::tr("missing")
                    : (embedded ? QObject::tr("embedded") : QObject::tr("found"));
                const QString source = resolvedUri.trimmed().isEmpty()
                    ? QObject::tr("unresolved")
                    : (embedded ? QObject::tr("in memory")
                                : QDir::toNativeSeparators(resolvedUri));

                if (int(textureFileEntries.size()) < kMaxTextureEntriesInLog) {
                    textureFileEntries.push_back(
                        QObject::tr("  [%1] %2 -> %3 (%4)")
                            .arg(ti)
                            .arg(displayName)
                            .arg(source)
                            .arg(status));
                }
            }

            QStringList details;
            details.reserve(int(textureFileEntries.size()) + 2);
            details.push_back(QObject::tr("glTF texture files (%1):").arg(model.textures.size()));
            details.append(textureFileEntries);
            if (model.textures.size() > textureFileEntries.size()) {
                details.push_back(
                    QObject::tr("  ... +%1 more").arg(model.textures.size() - textureFileEntries.size()));
            }
            reportProgress(cb, 10, details.join(QLatin1Char('\n')));
        }

        // One slot per material whose base color is a texture that could be read; a face's
        // texture index is its material's slot. A material with no base color texture has no
        // slot: its faces take its base color factor as their color, and their slot is -1
        // (docs/design/proposals/usd_support.md §6). Each base color asset sits at its slot's
        // index, as Document::loadMesh pairs mesh.textures[i] with textureAssets[i]; the other
        // channels' embedded images are appended after, once each.
        std::vector<int> materialSlot(model.materials.size(), -1);
        std::vector<MeshIOTextureAsset> baseAssets;
        for (size_t mi = 0; mi < model.materials.size(); ++mi) {
            const tinygltf::Material &mat = model.materials[mi];
            const int baseImage = imageOfTexture(mat.pbrMetallicRoughness.baseColorTexture.index);
            const QString baseUri = resolveImageUri(baseImage);
            if (baseUri.isEmpty())
                continue;
            MeshIOMaterialSlot slot;
            slot.name = QString::fromStdString(mat.name).trimmed();
            if (slot.name.isEmpty())
                slot.name = QObject::tr("Material %1").arg(mi + 1);
            const auto embedded = embeddedImages.find(baseImage);
            MeshIOTextureAsset asset;
            if (embedded != embeddedImages.end()) {
                asset = embedded->second;
            } else {
                asset.name = QFileInfo(baseUri).fileName();
                asset.sourcePath = baseUri;
                slot.baseColorTexture.filePath = baseUri;
            }
            slot.baseColorTexture.fileName = asset.name;
            slot.baseColorTexture.assetIndex = int(baseAssets.size());
            materialSlot[mi] = int(baseAssets.size());
            textureFiles.push_back(baseUri.toStdString());
            baseAssets.push_back(std::move(asset));
            materialSet.entries.push_back(std::move(slot));
        }
        materialSet.textureAssets = baseAssets;
        std::unordered_map<int, int> channelAsset;
        auto channelRef = [&](int texIndex, MeshIOTextureChannel channel) -> MeshIOMaterialTextureRef {
            MeshIOMaterialTextureRef ref;
            const int imageIndex = imageOfTexture(texIndex);
            const QString uri = resolveImageUri(imageIndex);
            if (uri.isEmpty())
                return ref;
            ref.fileName = QFileInfo(uri).fileName();
            ref.channel = channel;
            const auto embedded = embeddedImages.find(imageIndex);
            if (embedded == embeddedImages.end()) {
                ref.filePath = uri;
                return ref;
            }
            const auto placed = channelAsset.find(imageIndex);
            if (placed != channelAsset.end()) {
                ref.assetIndex = placed->second;
            } else {
                ref.assetIndex = int(materialSet.textureAssets.size());
                channelAsset[imageIndex] = ref.assetIndex;
                materialSet.textureAssets.push_back(embedded->second);
            }
            return ref;
        };
        const auto finite = [](double v, double lo) { return std::isfinite(v) && v >= lo; };
        for (size_t mi = 0; mi < model.materials.size(); ++mi) {
            if (materialSlot[mi] < 0)
                continue;
            const tinygltf::Material &mat = model.materials[mi];
            const tinygltf::PbrMetallicRoughness &pbr = mat.pbrMetallicRoughness;
            MeshIOMaterialSlot &slot = materialSet.entries[size_t(materialSlot[mi])];
            // glTF packs roughness in green and metallic in blue of one image, occlusion in red.
            slot.normalTexture = channelRef(mat.normalTexture.index, MeshIOTextureChannel::RGB);
            slot.occlusionTexture = channelRef(mat.occlusionTexture.index, MeshIOTextureChannel::R);
            slot.roughnessTexture = channelRef(pbr.metallicRoughnessTexture.index, MeshIOTextureChannel::G);
            slot.metallicTexture = channelRef(pbr.metallicRoughnessTexture.index, MeshIOTextureChannel::B);
            slot.emissiveTexture = channelRef(mat.emissiveTexture.index, MeshIOTextureChannel::RGB);
            if (finite(mat.normalTexture.scale, 0.0))
                slot.normalScale = float(mat.normalTexture.scale);
            if (finite(mat.occlusionTexture.strength, 0.0))
                slot.occlusionStrength = float(mat.occlusionTexture.strength);
            if (finite(pbr.roughnessFactor, 0.0))
                slot.roughnessFactor = float(pbr.roughnessFactor);
            // tinygltf fills in the specification's defaults (metallic 1, base color white).
            if (finite(pbr.metallicFactor, 0.0))
                slot.metallicFactor = float(pbr.metallicFactor);
            for (size_t c = 0; c < 4 && c < pbr.baseColorFactor.size(); ++c)
                slot.baseColorFactor[c] = float(pbr.baseColorFactor[c]);
            for (size_t c = 0; c < 3 && c < mat.emissiveFactor.size(); ++c)
                slot.emissiveFactor[c] = float(mat.emissiveFactor[c]);
            slot.alphaMode = mat.alphaMode == "MASK" ? MeshIOAlphaMode::Mask
                : mat.alphaMode == "BLEND"           ? MeshIOAlphaMode::Blend
                                                     : MeshIOAlphaMode::Opaque;
            if (finite(mat.alphaCutoff, 0.0))
                slot.alphaCutoff = float(mat.alphaCutoff);
        }

        // The color of a primitive's vertices when it has no COLOR_0: its material's base color
        // factor when the material has no slot; a slot keeps its factor itself.
        auto primitiveBaseColor = [&](int materialIndex) -> vcg::Color4b {
            if (materialIndex < 0 || materialIndex >= int(model.materials.size()) || materialSlot[size_t(materialIndex)] >= 0)
                return vcg::Color4b::White;
            const auto &factor = model.materials[size_t(materialIndex)].pbrMetallicRoughness.baseColorFactor;
            if (factor.size() < 3)
                return vcg::Color4b::White;
            const auto toU8 = [](double v) {
                return uint8_t(std::round(std::clamp(v, 0.0, 1.0) * 255.0));
            };
            const uint8_t r = toU8(factor[0]);
            const uint8_t g = toU8(factor[1]);
            const uint8_t b = toU8(factor[2]);
            const uint8_t a = (factor.size() >= 4) ? toU8(factor[3]) : 255;
            return vcg::Color4b(r, g, b, a);
        };

        std::function<void(int, const QMatrix4x4 &)> processNode;
        processNode = [&](int nodeIndex, const QMatrix4x4 &parentWorld) {
            if (nodeIndex < 0 || nodeIndex >= int(model.nodes.size()))
                return;
            const tinygltf::Node &node = model.nodes[size_t(nodeIndex)];
            const QMatrix4x4 world = parentWorld * nodeLocalMatrix(node);

            if (node.mesh >= 0 && node.mesh < int(model.meshes.size())) {
                const tinygltf::Mesh &srcMesh = model.meshes[size_t(node.mesh)];
                const int primitiveCount = int(srcMesh.primitives.size());
                for (int pi = 0; pi < primitiveCount; ++pi) {
                    const tinygltf::Primitive &prim = srcMesh.primitives[size_t(pi)];
                    const int mode = prim.mode;
                    // Lines are read as polyline edges: the writer saves a layer's edges as a LINES
                    // primitive, which this reader used to skip, so a polyline it wrote came back as
                    // "no supported geometry".
                    const bool lines = mode == TINYGLTF_MODE_LINE || mode == TINYGLTF_MODE_LINE_STRIP
                        || mode == TINYGLTF_MODE_LINE_LOOP;
                    if (mode != TINYGLTF_MODE_TRIANGLES && mode != TINYGLTF_MODE_POINTS && !lines) {
                        ++skippedUnsupportedPrimitiveCount;
                        continue;
                    }

                    const auto posIt = prim.attributes.find("POSITION");
                    if (posIt == prim.attributes.end()) {
                        ++skippedMissingPositionCount;
                        continue;
                    }

                    AccessorView posView, nrmView, uvView, colView, idxView;
                    if (!makeAccessorView(model, posIt->second, posView) || posView.componentCount < 3) {
                        ++skippedInvalidPositionAccessorCount;
                        continue;
                    }
                    const auto nIt = prim.attributes.find("NORMAL");
                    if (nIt != prim.attributes.end() && makeAccessorView(model, nIt->second, nrmView) && nrmView.componentCount >= 3)
                        hasVertexNormals = true;
                    const auto uvIt = prim.attributes.find("TEXCOORD_0");
                    if (uvIt != prim.attributes.end() && makeAccessorView(model, uvIt->second, uvView) && uvView.componentCount >= 2)
                        hasWedgeTexCoords = true;
                    const auto cIt = prim.attributes.find("COLOR_0");
                    if (cIt != prim.attributes.end() && makeAccessorView(model, cIt->second, colView) && colView.componentCount >= 3)
                        hasVertexColors = true;
                    const bool hasIndices = (prim.indices >= 0) && makeAccessorView(model, prim.indices, idxView);

                    const int texSlot = prim.material >= 0 && prim.material < int(materialSlot.size())
                        ? materialSlot[size_t(prim.material)] : -1;
                    const vcg::Color4b baseColor = primitiveBaseColor(prim.material);
                    const QMatrix3x3 normalMat = world.normalMatrix();

                    auto indexAt = [&](size_t i, uint32_t &dst) -> bool {
                        if (hasIndices)
                            return readIndex(idxView, i, dst);
                        if (i >= posView.count)
                            return false;
                        dst = uint32_t(i);
                        return true;
                    };

                    // --- Batch vertex+face allocation ---
                    // First pass: count unique source vertices so we can
                    // allocate them all at once (avoiding O(n²) resize per
                    // individual AddVertex call).
                    const size_t totalIdxCount = hasIndices ? idxView.count : posView.count;
                    std::vector<int> srcToLocal(posView.count, -1);
                    int localCount = 0;
                    for (size_t i = 0; i < totalIdxCount; ++i) {
                        uint32_t src = 0;
                        if (!indexAt(i, src) || src >= posView.count)
                            continue;
                        if (srcToLocal[src] < 0)
                            srcToLocal[src] = localCount++;
                    }

                    // Batch-allocate all new vertices at once.
                    const int baseVert = mesh.VN();
                    if (localCount > 0) {
                        vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, localCount);
                        for (uint32_t si = 0; si < posView.count; ++si) {
                            const int li = srcToLocal[si];
                            if (li < 0)
                                continue;
                            const int di = baseVert + li;
                            const QVector3D p = readVec3(posView, si);
                            const QVector4D wp = world * QVector4D(p, 1.0f);
                            mesh.vert[di].P() = VCGMesh::CoordType(wp.x(), wp.y(), wp.z());

                            if (nrmView.valid && si < nrmView.count) {
                                QVector3D n = mulMat3Vec3(normalMat, readVec3(nrmView, si));
                                if (!qFuzzyIsNull(n.lengthSquared()))
                                    n.normalize();
                                mesh.vert[di].N() = VCGMesh::CoordType(n.x(), n.y(), n.z());
                            }

                            if (colView.valid && si < colView.count) {
                                mesh.vert[di].C() = readColor4b(colView, si, baseColor);
                            } else {
                                mesh.vert[di].C() = baseColor;
                            }
                        }
                    }

                    if (lines) {
                        // LINES pairs the indices; LINE_STRIP joins each to the next, and LINE_LOOP
                        // also the last to the first.
                        std::vector<std::pair<int, int>> segments;
                        const size_t step = mode == TINYGLTF_MODE_LINE ? 2 : 1;
                        const size_t last = mode == TINYGLTF_MODE_LINE_LOOP ? totalIdxCount : totalIdxCount - 1;
                        for (size_t i = 0; totalIdxCount >= 2 && i < last; i += step) {
                            uint32_t a = 0, b = 0;
                            if (!indexAt(i, a) || !indexAt((i + 1) % totalIdxCount, b) || a >= posView.count || b >= posView.count)
                                continue;
                            if (srcToLocal[a] >= 0 && srcToLocal[b] >= 0 && a != b)
                                segments.emplace_back(baseVert + srcToLocal[a], baseVert + srcToLocal[b]);
                        }
                        if (!segments.empty()) {
                            auto ei = vcg::tri::Allocator<VCGMesh>::AddEdges(mesh, segments.size());
                            for (const auto &segment : segments) {
                                ei->V(0) = &mesh.vert[size_t(segment.first)];
                                ei->V(1) = &mesh.vert[size_t(segment.second)];
                                ++ei;
                            }
                            hasEdges = true;
                        }
                    }

                    // Batch-allocate faces (triangles only).
                    if (mode == TINYGLTF_MODE_TRIANGLES) {
                        const size_t triCount = totalIdxCount / 3;
                        if (triCount > 0) {
                            const int baseFace = mesh.FN();
                            vcg::tri::Allocator<VCGMesh>::AddFaces(mesh, triCount);
                            for (size_t ti = 0; ti < triCount; ++ti) {
                                uint32_t idx[3] = { 0, 0, 0 };
                                if (!indexAt(ti * 3 + 0, idx[0])
                                 || !indexAt(ti * 3 + 1, idx[1])
                                 || !indexAt(ti * 3 + 2, idx[2]))
                                    continue;
                                const int li[3] = {
                                    srcToLocal[idx[0]],
                                    srcToLocal[idx[1]],
                                    srcToLocal[idx[2]]
                                };
                                if (li[0] < 0 || li[1] < 0 || li[2] < 0)
                                    continue;
                                auto &f = mesh.face[baseFace + ti];
                                f.V(0) = &mesh.vert[baseVert + li[0]];
                                f.V(1) = &mesh.vert[baseVert + li[1]];
                                f.V(2) = &mesh.vert[baseVert + li[2]];
                                if (uvView.valid) {
                                    hasWedgeTexCoords = true;
                                    const int wedgeSlot = texSlot;
                                    for (int c = 0; c < 3; ++c) {
                                        const QVector2D uv =
                                            (idx[c] < uvView.count)
                                            ? readVec2(uvView, idx[c])
                                            : QVector2D();
                                        f.WT(c).U() = uv.x();
                                        f.WT(c).V() = 1.0f - uv.y();
                                        f.WT(c).N() = wedgeSlot;
                                    }
                                }
                            }
                        }
                    }

                    if (cb != nullptr && primitiveCount > 0) {
                        const int primitiveProgress = int((pi + 1) * 100 / primitiveCount);
                        reportProgress(cb, primitiveProgress, QStringLiteral("Loading glTF primitives"), true);
                    }
                }
            }

            for (const int child : node.children)
                processNode(child, world);
        };

        bool processedAnyRoot = false;
        QMatrix4x4 identity;
        identity.setToIdentity();

        if (model.defaultScene >= 0 && model.defaultScene < int(model.scenes.size())) {
            const auto &scene = model.scenes[size_t(model.defaultScene)];
            for (const int nodeIndex : scene.nodes) {
                processedAnyRoot = true;
                processNode(nodeIndex, identity);
            }
        } else if (!model.scenes.empty()) {
            for (const auto &scene : model.scenes) {
                for (const int nodeIndex : scene.nodes) {
                    processedAnyRoot = true;
                    processNode(nodeIndex, identity);
                }
            }
        }

        if (!processedAnyRoot) {
            for (size_t ni = 0; ni < model.nodes.size(); ++ni)
                processNode(int(ni), identity);
        }

        if (mesh.VN() == 0) {
            reportProgress(cb, 0, QObject::tr("glTF error: no supported geometry could be imported."));
            return kErrInvalidData;
        }

        mesh.textures = textureFiles;

        if (hasEdges)
            loadMask |= vcg::tri::io::Mask::IOM_EDGEINDEX;
        if (hasVertexNormals)
            loadMask |= vcg::tri::io::Mask::IOM_VERTNORMAL;
        if (hasVertexColors)
            loadMask |= vcg::tri::io::Mask::IOM_VERTCOLOR;
        if (hasWedgeTexCoords)
            loadMask |= vcg::tri::io::Mask::IOM_WEDGTEXCOORD;
        if (hasWedgeTexCoords && textureFiles.size() > 1)
            loadMask |= vcg::tri::io::Mask::IOM_WEDGTEXMULTI;

        if (outLoadMask)
            *outLoadMask = loadMask;
        if (outMaterialSet)
            *outMaterialSet = materialSet;

        if (skippedUnsupportedPrimitiveCount > 0) {
            reportProgress(
                cb,
                100,
                QObject::tr("glTF warning: skipped %1 unsupported primitive(s) (only TRIANGLES and POINTS are imported).")
                    .arg(skippedUnsupportedPrimitiveCount));
        }
        if (skippedMissingPositionCount > 0) {
            reportProgress(
                cb,
                100,
                QObject::tr("glTF warning: skipped %1 primitive(s) missing POSITION attribute.")
                    .arg(skippedMissingPositionCount));
        }
        if (skippedInvalidPositionAccessorCount > 0) {
            reportProgress(
                cb,
                100,
                QObject::tr("glTF warning: skipped %1 primitive(s) with invalid POSITION accessor.")
                    .arg(skippedInvalidPositionAccessorCount));
        }
        if (textureDecodeFailureCount > 0 || textureMissingFileCount > 0) {
            reportProgress(
                cb,
                100,
                QObject::tr("glTF texture report: %1 decode failure(s), %2 missing file(s).")
                    .arg(textureDecodeFailureCount)
                    .arg(textureMissingFileCount));
        }

        reportProgress(cb, 100, QStringLiteral("Loading glTF done"), true);

        return 0;
    }

    QString filterString() const override
    {
        return QObject::tr("glTF Files (*.gltf *.glb)");
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb) const override
    {
        return save(filename, mesh, options, cb, nullptr);
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb,
        const MeshIOTextureContext *textureContext) const override
    {
        const QString ext = normalizedExtension(filename);
        if (!isSupportedExtension(ext))
            return kErrSaveUnsupported;
        if (mesh.VN() == 0)
            return kErrSaveEmpty;

        const int capabilityMask = saveMaskCapability(filename);
        int saveMask = options.mask != 0 ? options.mask : capabilityMask;
        saveMask &= capabilityMask;
        saveMask |= vcg::tri::io::Mask::IOM_VERTCOORD;
        if (mesh.FN() > 0)
            saveMask |= vcg::tri::io::Mask::IOM_FACEINDEX;
        if (mesh.EN() > 0)
            saveMask |= vcg::tri::io::Mask::IOM_EDGEINDEX;

        const bool writeNormals = (saveMask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
        const bool writeVertexColors = (saveMask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0;
        const bool writeFaceColors = !writeVertexColors && ((saveMask & vcg::tri::io::Mask::IOM_FACECOLOR) != 0);
        const bool writeWedgeTexcoords = (saveMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0;
        const bool writeVertexTexcoords =
            !writeWedgeTexcoords && ((saveMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0);
        const bool embedTextures = options.embedTextures;
        const bool dracoRequested = options.dracoCompression;

        reportProgress(
            cb,
            5,
            QObject::tr("Preparing glTF export: %1").arg(QFileInfo(filename).fileName()),
            true);

        std::vector<PrimitivePayload> primitives;
        std::unordered_map<int, size_t> triByTextureSlot;
        std::unordered_map<int, int> usedTextureSlots;

        auto primitiveForTriangleTexture = [&](int textureSlot) -> PrimitivePayload & {
            const auto it = triByTextureSlot.find(textureSlot);
            if (it != triByTextureSlot.end())
                return primitives[it->second];

            PrimitivePayload payload;
            payload.mode = TINYGLTF_MODE_TRIANGLES;
            payload.textureSlot = textureSlot;
            payload.hasNormals = writeNormals;
            payload.hasColors = writeVertexColors || writeFaceColors;
            payload.hasTexcoords = writeWedgeTexcoords || writeVertexTexcoords;

            primitives.push_back(std::move(payload));
            const size_t idx = primitives.size() - 1;
            triByTextureSlot[textureSlot] = idx;
            if (textureSlot >= 0)
                usedTextureSlots[textureSlot] = -1;
            return primitives[idx];
        };

        const bool hasFaces = mesh.FN() > 0 && ((saveMask & vcg::tri::io::Mask::IOM_FACEINDEX) != 0);
        const bool hasEdges = mesh.EN() > 0 && ((saveMask & vcg::tri::io::Mask::IOM_EDGEINDEX) != 0);

        if (hasFaces) {
            for (const VCGFace &face : mesh.face) {
                if (face.IsD())
                    continue;

                int textureSlot = -1;
                if (writeWedgeTexcoords) {
                    textureSlot = face.cWT(0).N();
                    if (textureSlot < 0)
                        textureSlot = -1;
                } else if (writeVertexTexcoords && !mesh.textures.empty()) {
                    // Per-vertex texcoords have no per-face texture index in VCGMesh.
                    // Bind texture slot 0 when UVs are exported.
                    textureSlot = 0;
                }

                PrimitivePayload &prim = primitiveForTriangleTexture(textureSlot);
                for (int c = 0; c < 3; ++c) {
                    const VCGVertex *v = face.cV(c);
                    if (!v)
                        continue;

                    float u = 0.0f;
                    float vTex = 0.0f;
                    if (writeWedgeTexcoords) {
                        u = face.cWT(c).U();
                        // Internal UVs are stored with opposite V wrt glTF import.
                        vTex = 1.0f - face.cWT(c).V();
                    } else if (writeVertexTexcoords) {
                        u = v->cT().u();
                        vTex = 1.0f - v->cT().v();
                    }

                    const uint32_t idx = appendPrimitiveVertex(
                        prim,
                        v,
                        face.cC(),
                        writeFaceColors,
                        writeNormals,
                        writeVertexColors || writeFaceColors,
                        writeWedgeTexcoords || writeVertexTexcoords,
                        u,
                        vTex);
                    prim.indices.push_back(idx);
                }
            }
        }

        if (hasEdges) {
            PrimitivePayload edgePrim;
            edgePrim.mode = TINYGLTF_MODE_LINE;
            edgePrim.hasNormals = writeNormals;
            edgePrim.hasColors = writeVertexColors;
            edgePrim.hasTexcoords = writeVertexTexcoords;

            for (const VCGEdge &edge : mesh.edge) {
                if (edge.IsD())
                    continue;
                const VCGVertex *v0 = edge.cV(0);
                const VCGVertex *v1 = edge.cV(1);
                if (!v0 || !v1)
                    continue;

                const float u0 = writeVertexTexcoords ? v0->cT().u() : 0.0f;
                const float v0t = writeVertexTexcoords ? (1.0f - v0->cT().v()) : 0.0f;
                const float u1 = writeVertexTexcoords ? v1->cT().u() : 0.0f;
                const float v1t = writeVertexTexcoords ? (1.0f - v1->cT().v()) : 0.0f;

                const uint32_t i0 = appendPrimitiveVertex(
                    edgePrim,
                    v0,
                    vcg::Color4b::White,
                    false,
                    writeNormals,
                    writeVertexColors,
                    writeVertexTexcoords,
                    u0,
                    v0t);
                const uint32_t i1 = appendPrimitiveVertex(
                    edgePrim,
                    v1,
                    vcg::Color4b::White,
                    false,
                    writeNormals,
                    writeVertexColors,
                    writeVertexTexcoords,
                    u1,
                    v1t);
                edgePrim.indices.push_back(i0);
                edgePrim.indices.push_back(i1);
            }

            if (!edgePrim.indices.empty())
                primitives.push_back(std::move(edgePrim));
        }

        if (!hasFaces && !hasEdges) {
            PrimitivePayload pointPrim;
            pointPrim.mode = TINYGLTF_MODE_POINTS;
            pointPrim.hasNormals = writeNormals;
            pointPrim.hasColors = writeVertexColors;
            pointPrim.hasTexcoords = writeVertexTexcoords;

            for (const VCGVertex &vertex : mesh.vert) {
                if (vertex.IsD())
                    continue;
                const float u = writeVertexTexcoords ? vertex.cT().u() : 0.0f;
                const float vTex = writeVertexTexcoords ? (1.0f - vertex.cT().v()) : 0.0f;
                const uint32_t idx = appendPrimitiveVertex(
                    pointPrim,
                    &vertex,
                    vcg::Color4b::White,
                    false,
                    writeNormals,
                    writeVertexColors,
                    writeVertexTexcoords,
                    u,
                    vTex);
                pointPrim.indices.push_back(idx);
            }

            if (!pointPrim.indices.empty())
                primitives.push_back(std::move(pointPrim));
        }

        if (primitives.empty())
            return kErrSaveEmpty;

        tinygltf::Model model;
        model.asset.version = "2.0";
        model.asset.generator = "MeshLab";

        // One glTF material per material slot the faces use, carrying all of it: base color,
        // normal, occlusion, metallic-roughness and emissive textures with their factors, and
        // the alpha mode. Each image is written once, however many slots or channels use it.
        std::unordered_map<int, int> textureSlotToMaterial;
        // Assets with no file are missing from mesh.textures, so it alone undercounts them.
        const int textureSlotCount = std::max(int(mesh.textures.size()),
            textureContext && textureContext->textureAssets
                ? int(textureContext->textureAssets->size()) : 0);
        QStringList savedTexturePaths(textureContext && textureContext->textureAssets
            ? qsizetype(textureContext->textureAssets->size()) : 0);
        const MeshIOMaterialSlot *const noSlot = nullptr;
        auto slotOf = [&](int slot) -> const MeshIOMaterialSlot * {
            return textureContext && textureContext->materialSet && slot >= 0
                    && slot < int(textureContext->materialSet->entries.size())
                ? &textureContext->materialSet->entries[size_t(slot)] : noSlot;
        };
        // The asset a reference names, and the file that holds its image (empty when none does).
        auto assetOf = [&](const MeshIOMaterialTextureRef &ref, int fallbackIndex, int *assetIndex)
            -> std::pair<const MeshIOTextureAsset *, QString> {
            int index = ref.assetIndex >= 0 ? ref.assetIndex : fallbackIndex;
            const MeshIOTextureAsset *asset = textureContext && textureContext->textureAssets && index >= 0
                    && index < int(textureContext->textureAssets->size())
                ? &textureContext->textureAssets->at(size_t(index)) : nullptr;
            if (assetIndex)
                *assetIndex = asset ? index : -1;
            QString path = asset ? asset->sourcePath.trimmed() : ref.filePath.trimmed();
            if (path.isEmpty() && fallbackIndex >= 0 && fallbackIndex < int(mesh.textures.size()))
                path = QString::fromStdString(mesh.textures[size_t(fallbackIndex)]);
            return { asset, QFileInfo(path).isFile() ? path : QString() };
        };
        auto imageOf = [&](const MeshIOMaterialTextureRef &ref) -> QImage {
            const auto [asset, path] = assetOf(ref, -1, nullptr);
            return !path.isEmpty() ? QImage(path) : (asset ? asset->image : QImage());
        };
        // Writes an image as a glTF image and texture, from its file when it has one (a JPEG stays
        // a JPEG), else from pixels; returns the texture index, -1 when it could not be written.
        std::map<QString, int> writtenTextures;
        auto writeTexture = [&](const QString &key, const QString &file, const QImage &pixels,
                                const QString &name, int number, int assetIndex) -> int {
            const auto done = writtenTextures.find(key);
            if (done != writtenTextures.end())
                return done->second;
            tinygltf::Image image;
            QString savedPath;
            if (embedTextures) {
                const bool loaded = file.isEmpty() ? fillTinyGltfImage(image, pixels, number, cb)
                                                   : fillTinyGltfImageFromFile(image, file, number, cb);
                if (!loaded)
                    return writtenTextures[key] = -1;
                savedPath = QFileInfo(filename).absoluteFilePath();
            } else {
                const QString uri = file.isEmpty() ? saveTextureImage(filename, pixels, name, number, cb)
                                                   : makeCopiedTextureUri(filename, file, cb, number);
                if (uri.isEmpty())
                    return writtenTextures[key] = -1;
                image.uri = uri.toStdString();
                image.name = QFileInfo(uri).fileName().toStdString();
                savedPath = QFileInfo(filename).absoluteDir().absoluteFilePath(uri);
            }
            if (assetIndex >= 0 && assetIndex < savedTexturePaths.size())
                savedTexturePaths[assetIndex] = savedPath;
            model.images.push_back(std::move(image));
            tinygltf::Texture texture;
            texture.source = int(model.images.size() - 1);
            model.textures.push_back(std::move(texture));
            return writtenTextures[key] = int(model.textures.size() - 1);
        };
        // A reference written as it is, its image read by glTF in the channel glTF fixes for the
        // role (wanted); an image whose channel is another is repacked into that one.
        auto writeRef = [&](const MeshIOMaterialTextureRef &ref, int fallbackIndex, MeshIOTextureChannel wanted,
                            int number) -> int {
            if (!ref.isValid() && fallbackIndex < 0)
                return -1;
            int assetIndex = -1;
            const auto [asset, file] = assetOf(ref, fallbackIndex, &assetIndex);
            if (!asset && file.isEmpty())
                return -1;
            const QString key = !file.isEmpty() ? QFileInfo(file).absoluteFilePath()
                                                : QStringLiteral("asset:%1").arg(assetIndex);
            if (ref.channel == wanted || ref.channel == MeshIOTextureChannel::RGB)
                return writeTexture(key, file, asset ? asset->image : QImage(),
                    asset ? asset->name : QFileInfo(file).fileName(), number, assetIndex);
            const QImage packed = packChannels(imageOf(ref), ref.channel, QImage(), MeshIOTextureChannel::RGB, wanted);
            return writeTexture(key + QStringLiteral("#") + QString::number(int(wanted)), QString(), packed,
                QStringLiteral("channel_%1.png").arg(number), number, -1);
        };
        // glTF reads roughness from the green and metallic from the blue of one image: the
        // slot's image when both name it in those channels (or there is no metallic map and the
        // factor leaves metallic at 0), else one packed from the two.
        auto writeMetallicRoughness = [&](const MeshIOMaterialSlot &slot, int number) -> int {
            const MeshIOMaterialTextureRef &r = slot.roughnessTexture, &m = slot.metallicTexture;
            if (!r.isValid() && !m.isValid())
                return -1;
            const bool sameImage = r.isValid() && m.isValid() && r.assetIndex == m.assetIndex
                && r.filePath == m.filePath && r.fileName == m.fileName;
            if ((sameImage && r.channel == MeshIOTextureChannel::G && m.channel == MeshIOTextureChannel::B)
                || (!m.isValid() && slot.metallicFactor == 0.0f && r.channel == MeshIOTextureChannel::G))
                return writeRef(r, -1, MeshIOTextureChannel::G, number);
            const QImage packed = packMetallicRoughness(r.isValid() ? imageOf(r) : QImage(), r.channel,
                m.isValid() ? imageOf(m) : QImage(), m.channel);
            return writeTexture(QStringLiteral("metallicRoughness:%1").arg(number), QString(), packed,
                QStringLiteral("metallic_roughness_%1.png").arg(number), number, -1);
        };
        if (textureSlotCount > 0 && !usedTextureSlots.empty()) {
            for (const auto &entry : usedTextureSlots) {
                const int slotIndex = entry.first;
                if (slotIndex < 0 || slotIndex >= textureSlotCount)
                    continue;
                const MeshIOMaterialSlot *slot = slotOf(slotIndex);
                // A slot listed only in mesh.textures is a base color texture of the same index.
                const int baseTexture = writeRef(slot ? slot->baseColorTexture : MeshIOMaterialTextureRef{},
                    slot && slot->baseColorTexture.isValid() ? -1 : slotIndex, MeshIOTextureChannel::RGB, slotIndex);
                if (baseTexture < 0)
                    continue;

                tinygltf::Material material;
                material.name = (slot && !slot->name.trimmed().isEmpty() ? slot->name.trimmed()
                                                                          : QStringLiteral("mat_%1").arg(slotIndex))
                                    .toStdString();
                tinygltf::PbrMetallicRoughness &pbr = material.pbrMetallicRoughness;
                pbr.baseColorTexture.index = baseTexture;
                pbr.baseColorTexture.texCoord = 0;
                // Written always: tinygltf leaves out a value equal to the specification's default,
                // and a slot with no metallic of its own must not read as fully metallic.
                const MeshIOMaterialSlot plain;
                const MeshIOMaterialSlot &m = slot ? *slot : plain;
                pbr.baseColorFactor = { m.baseColorFactor[0], m.baseColorFactor[1], m.baseColorFactor[2], m.baseColorFactor[3] };
                pbr.metallicFactor = m.metallicFactor;
                pbr.roughnessFactor = m.roughnessFactor;
                if (slot) {
                    pbr.metallicRoughnessTexture.index = writeMetallicRoughness(*slot, slotIndex);
                    material.normalTexture.index = writeRef(slot->normalTexture, -1, MeshIOTextureChannel::RGB, slotIndex);
                    material.normalTexture.scale = slot->normalScale;
                    material.occlusionTexture.index =
                        writeRef(slot->occlusionTexture, -1, MeshIOTextureChannel::R, slotIndex);
                    material.occlusionTexture.strength = slot->occlusionStrength;
                    material.emissiveTexture.index =
                        writeRef(slot->emissiveTexture, -1, MeshIOTextureChannel::RGB, slotIndex);
                    material.emissiveFactor = { slot->emissiveFactor[0], slot->emissiveFactor[1], slot->emissiveFactor[2] };
                    material.alphaMode = slot->alphaMode == MeshIOAlphaMode::Mask ? "MASK"
                        : slot->alphaMode == MeshIOAlphaMode::Blend              ? "BLEND"
                                                                                 : "OPAQUE";
                    material.alphaCutoff = slot->alphaCutoff;
                }
                material.doubleSided = true;
                model.materials.push_back(std::move(material));
                textureSlotToMaterial[slotIndex] = int(model.materials.size() - 1);
            }
        }

        std::vector<unsigned char> binaryBuffer;
        tinygltf::Mesh outMesh;
        outMesh.name = QFileInfo(filename).completeBaseName().toStdString();
        bool usedDracoCompression = false;
        bool warnedDracoModeUnsupported = false;
        bool warnedDracoUnavailable = false;
#if defined(MESHLAB2_GLTF_HAS_DRACO)
        (void) warnedDracoUnavailable;
#endif

        for (PrimitivePayload &prim : primitives) {
            if (prim.positions.empty() || prim.indices.empty())
                continue;

            tinygltf::Primitive gltfPrim;
            gltfPrim.mode = prim.mode;

            bool wroteWithDraco = false;
            if (dracoRequested && prim.mode != TINYGLTF_MODE_TRIANGLES && !warnedDracoModeUnsupported) {
                reportProgress(
                    cb,
                    0,
                    QObject::tr("glTF export warning: Draco compression is currently applied only to triangle primitives."));
                warnedDracoModeUnsupported = true;
            }

#if defined(MESHLAB2_GLTF_HAS_DRACO)
            if (dracoRequested && prim.mode == TINYGLTF_MODE_TRIANGLES) {
                DracoEncodedPrimitive dracoPrim;
                if (encodePrimitiveWithDraco(prim, options, dracoPrim) && dracoPrim.valid) {
                    const int dracoBv = addBufferView(
                        model,
                        binaryBuffer,
                        dracoPrim.bytes.data(),
                        dracoPrim.bytes.size(),
                        0);

                    const int posAcc =
                        addAccessor(model, -1, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.positions.size() / 3, TINYGLTF_TYPE_VEC3);
                    setAccessorMinMaxVec3(model.accessors[size_t(posAcc)], prim.positions);
                    gltfPrim.attributes["POSITION"] = posAcc;

                    if (dracoPrim.attributeUniqueIds.find("NORMAL") != dracoPrim.attributeUniqueIds.end()) {
                        const int nAcc =
                            addAccessor(model, -1, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.normals.size() / 3, TINYGLTF_TYPE_VEC3);
                        gltfPrim.attributes["NORMAL"] = nAcc;
                    }

                    if (dracoPrim.attributeUniqueIds.find("COLOR_0") != dracoPrim.attributeUniqueIds.end()) {
                        const int cAcc =
                            addAccessor(model, -1, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.colors.size() / 4, TINYGLTF_TYPE_VEC4);
                        gltfPrim.attributes["COLOR_0"] = cAcc;
                    }

                    if (dracoPrim.attributeUniqueIds.find("TEXCOORD_0") != dracoPrim.attributeUniqueIds.end()) {
                        const int tAcc =
                            addAccessor(model, -1, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.texcoords.size() / 2, TINYGLTF_TYPE_VEC2);
                        gltfPrim.attributes["TEXCOORD_0"] = tAcc;
                    }

                    const int iAcc = addAccessor(
                        model,
                        -1,
                        TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                        prim.indices.size(),
                        TINYGLTF_TYPE_SCALAR);
                    gltfPrim.indices = iAcc;

                    tinygltf::Value::Object dracoExt;
                    dracoExt["bufferView"] = tinygltf::Value(dracoBv);
                    tinygltf::Value::Object dracoAttrMap;
                    for (const auto &entry : dracoPrim.attributeUniqueIds)
                        dracoAttrMap[entry.first] = tinygltf::Value(entry.second);
                    dracoExt["attributes"] = tinygltf::Value(dracoAttrMap);
                    gltfPrim.extensions["KHR_draco_mesh_compression"] = tinygltf::Value(dracoExt);

                    usedDracoCompression = true;
                    wroteWithDraco = true;
                } else if (!dracoPrim.error.isEmpty()) {
                    reportProgress(
                        cb,
                        0,
                        QObject::tr("glTF export warning: %1 Falling back to uncompressed geometry.")
                            .arg(dracoPrim.error));
                }
            }
#else
            if (dracoRequested && !warnedDracoUnavailable) {
                reportProgress(
                    cb,
                    0,
                    QObject::tr("glTF export warning: Draco compression requested but this MeshLab build has no Draco support."));
                warnedDracoUnavailable = true;
            }
#endif

            if (!wroteWithDraco) {
                const int posBv = addBufferView(
                    model,
                    binaryBuffer,
                    prim.positions.data(),
                    prim.positions.size() * sizeof(float),
                    TINYGLTF_TARGET_ARRAY_BUFFER);
                const int posAcc =
                    addAccessor(model, posBv, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.positions.size() / 3, TINYGLTF_TYPE_VEC3);
                setAccessorMinMaxVec3(model.accessors[size_t(posAcc)], prim.positions);
                gltfPrim.attributes["POSITION"] = posAcc;

                if (prim.hasNormals && prim.normals.size() == prim.positions.size()) {
                    const int nBv = addBufferView(
                        model,
                        binaryBuffer,
                        prim.normals.data(),
                        prim.normals.size() * sizeof(float),
                        TINYGLTF_TARGET_ARRAY_BUFFER);
                    const int nAcc =
                        addAccessor(model, nBv, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.normals.size() / 3, TINYGLTF_TYPE_VEC3);
                    gltfPrim.attributes["NORMAL"] = nAcc;
                }

                if (prim.hasColors && (prim.colors.size() / 4) == (prim.positions.size() / 3)) {
                    const int cBv = addBufferView(
                        model,
                        binaryBuffer,
                        prim.colors.data(),
                        prim.colors.size() * sizeof(float),
                        TINYGLTF_TARGET_ARRAY_BUFFER);
                    const int cAcc =
                        addAccessor(model, cBv, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.colors.size() / 4, TINYGLTF_TYPE_VEC4);
                    gltfPrim.attributes["COLOR_0"] = cAcc;
                }

                if (prim.hasTexcoords && (prim.texcoords.size() / 2) == (prim.positions.size() / 3)) {
                    const int tBv = addBufferView(
                        model,
                        binaryBuffer,
                        prim.texcoords.data(),
                        prim.texcoords.size() * sizeof(float),
                        TINYGLTF_TARGET_ARRAY_BUFFER);
                    const int tAcc =
                        addAccessor(model, tBv, TINYGLTF_COMPONENT_TYPE_FLOAT, prim.texcoords.size() / 2, TINYGLTF_TYPE_VEC2);
                    gltfPrim.attributes["TEXCOORD_0"] = tAcc;
                }

                const int iBv = addBufferView(
                    model,
                    binaryBuffer,
                    prim.indices.data(),
                    prim.indices.size() * sizeof(uint32_t),
                    TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                const int iAcc =
                    addAccessor(model, iBv, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, prim.indices.size(), TINYGLTF_TYPE_SCALAR);
                gltfPrim.indices = iAcc;
            }

            if (prim.textureSlot >= 0) {
                const auto it = textureSlotToMaterial.find(prim.textureSlot);
                if (it != textureSlotToMaterial.end())
                    gltfPrim.material = it->second;
            }

            outMesh.primitives.push_back(std::move(gltfPrim));
        }

        if (usedDracoCompression) {
            if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                    std::string("KHR_draco_mesh_compression")) == model.extensionsUsed.end()) {
                model.extensionsUsed.push_back("KHR_draco_mesh_compression");
            }
            if (std::find(model.extensionsRequired.begin(), model.extensionsRequired.end(),
                    std::string("KHR_draco_mesh_compression")) == model.extensionsRequired.end()) {
                model.extensionsRequired.push_back("KHR_draco_mesh_compression");
            }
        }

        if (outMesh.primitives.empty())
            return kErrSaveEmpty;

        model.meshes.push_back(std::move(outMesh));

        tinygltf::Node node;
        node.mesh = 0;
        node.name = QFileInfo(filename).completeBaseName().toStdString();
        model.nodes.push_back(std::move(node));

        tinygltf::Scene scene;
        scene.nodes.push_back(0);
        scene.name = "Scene";
        model.scenes.push_back(std::move(scene));
        model.defaultScene = 0;

        tinygltf::Buffer gltfBuffer;
        gltfBuffer.name = "buffer0";
        gltfBuffer.data = std::move(binaryBuffer);
        model.buffers.push_back(std::move(gltfBuffer));

        tinygltf::TinyGLTF writer;
        if (!embedTextures)
            writer.SetImageWriter(nullptr, nullptr);
        const bool writeBinary = (ext == QLatin1String("glb"));
        const bool ok = writer.WriteGltfSceneToFile(
            &model,
            filename.toStdString(),
            embedTextures,
            true,   // embedBuffers (single-file .gltf, implicit for .glb)
            true,   // prettyPrint
            writeBinary);
        if (!ok)
            return kErrSaveWrite;
        if (textureContext && textureContext->savedTexturePaths)
            *textureContext->savedTexturePaths = savedTexturePaths;

        reportProgress(cb, 100, QObject::tr("Saved glTF: %1").arg(QFileInfo(filename).fileName()), true);
        return 0;
    }

    QString saveFilterString() const override
    {
        return QObject::tr("glTF Files (*.gltf *.glb)");
    }

    int saveMaskCapability(const QString &filename) const override
    {
        if (!canSave(filename))
            return 0;
        int capability = 0;
        capability |= vcg::tri::io::Mask::IOM_VERTCOORD;
        capability |= vcg::tri::io::Mask::IOM_FACEINDEX;
        capability |= vcg::tri::io::Mask::IOM_EDGEINDEX;
        capability |= vcg::tri::io::Mask::IOM_VERTNORMAL;
        capability |= vcg::tri::io::Mask::IOM_VERTCOLOR;
        capability |= vcg::tri::io::Mask::IOM_FACECOLOR;
        capability |= vcg::tri::io::Mask::IOM_VERTTEXCOORD;
        capability |= vcg::tri::io::Mask::IOM_WEDGTEXCOORD;
        return capability;
    }

    QString errorString(int errCode) const override
    {
        switch (errCode) {
        case kErrOpen:
            return QObject::tr("Cannot open glTF/glb file. Check file path and external resources.");
        case kErrParse:
            return QObject::tr("Failed to parse glTF document. Check log for detailed tinygltf errors.");
        case kErrNoMesh:
            return QObject::tr("No mesh data found in glTF document.");
        case kErrInvalidData:
            return QObject::tr("glTF geometry data is empty or unsupported.");
        case kErrSaveUnsupported:
            return QObject::tr("Unsupported glTF extension for export.");
        case kErrSaveEmpty:
            return QObject::tr("No exportable geometry found.");
        case kErrSaveWrite:
            return QObject::tr("Failed to write glTF/glb file.");
        default:
            return QObject::tr("Unknown glTF import/export error.");
        }
    }
};
}

void registerGltfImportPlugin(MeshIOPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<GLTFImportPlugin>());
}

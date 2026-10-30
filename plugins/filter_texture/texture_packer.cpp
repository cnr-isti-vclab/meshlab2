/****************************************************************************
* MeshLab                                                           o o     *
* Copyright(C) 2005 Visual Computing Lab - ISTI CNR                         *
* GPL-2.0-or-later                                                          *
****************************************************************************/

#include "texture_packer.hpp"

#include <QObject>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <limits>

std::vector<QImage> TexturePacker::simplePacking(
    const std::vector<QImage> &sourceImages,
    int outputCount,
    int gutter,
    VCGMesh &mesh,
    QString *error)
{
    if (error)
        error->clear();
    TexturePacker packer(sourceImages, outputCount, gutter, mesh, error);
    if (!packer.validate() || !packer.findPlacements())
        return {};
    packer.updateMeshUVs();
    return packer.rasterize();
}

TexturePacker::TexturePacker(
    const std::vector<QImage> &sourceImages,
    int outputCount,
    int gutter,
    VCGMesh &mesh,
    QString *error)
    : m_gutter(gutter), m_mesh(mesh), m_error(error)
{
    m_sources.reserve(sourceImages.size());
    for (const QImage &image : sourceImages)
        m_sources.push_back({ image });

    if (outputCount <= 0)
        return;
    m_containers.resize(size_t(outputCount));
    for (int source = 0; source < int(m_sources.size()); ++source) {
        const int container = source % outputCount;
        m_sources[size_t(source)].container = container;
        m_containers[size_t(container)].sources.push_back(source);
    }
}

bool TexturePacker::fail(const QString &message)
{
    if (m_error)
        *m_error = message;
    return false;
}

bool TexturePacker::validate()
{
    if (m_sources.empty())
        return fail(QObject::tr("No source texture images were provided."));
    if (m_containers.empty() || m_containers.size() > m_sources.size())
        return fail(QObject::tr("The output texture count must be between 1 and %1.")
                        .arg(m_sources.size()));
    if (m_gutter < 0 || m_gutter > std::numeric_limits<int>::max() / 2)
        return fail(QObject::tr("The texture gutter is outside the supported range."));
    if (!m_mesh.face.IsWedgeTexCoordEnabled())
        return fail(QObject::tr("The mesh does not have per-wedge texture coordinates."));

    constexpr float tolerance = 1e-6f;
    for (int source = 0; source < int(m_sources.size()); ++source) {
        const QImage &image = m_sources[size_t(source)].image.get();
        if (image.isNull() || image.width() <= 0 || image.height() <= 0)
            return fail(QObject::tr("Source texture %1 has no valid image data.").arg(source));
        if (image.width() > std::numeric_limits<int>::max() - 2 * m_gutter
            || image.height() > std::numeric_limits<int>::max() - 2 * m_gutter) {
            return fail(QObject::tr("Source texture %1 is too large to add the requested gutter.")
                            .arg(source));
        }
    }

    for (int faceIndex = 0; faceIndex < m_mesh.FN(); ++faceIndex) {
        const VCGFace &face = m_mesh.face[size_t(faceIndex)];
        // A face with no texture (vcglib's index -1) has nothing to move: it is left alone.
        if (face.IsD() || face.cWT(0).N() < 0)
            continue;
        const int source = face.cWT(0).N();
        if (source >= int(m_sources.size()))
            return fail(QObject::tr("Face %1 references invalid texture %2.")
                            .arg(faceIndex).arg(source));
        for (int corner = 0; corner < 3; ++corner) {
            const auto &uv = face.cWT(corner);
            if (uv.N() != source)
                return fail(QObject::tr("Face %1 references more than one texture.").arg(faceIndex));
            if (!std::isfinite(uv.U()) || !std::isfinite(uv.V())
                || uv.U() < -tolerance || uv.U() > 1.0f + tolerance
                || uv.V() < -tolerance || uv.V() > 1.0f + tolerance) {
                return fail(QObject::tr(
                    "Face %1 has UV coordinates outside [0,1]; repeated textures cannot be packed safely.")
                                .arg(faceIndex));
            }
        }
    }
    return true;
}

bool TexturePacker::findPlacements()
{
    const int maxInt = std::numeric_limits<int>::max();
    for (Container &container : m_containers) {
        std::vector<vcg::Point2i> sizes;
        sizes.reserve(container.sources.size());
        long double area = 0.0;
        for (const int source : container.sources) {
            const QImage &image = m_sources[size_t(source)].image.get();
            const int width = image.width() + 2 * m_gutter;
            const int height = image.height() + 2 * m_gutter;
            sizes.emplace_back(width, height);
            area += static_cast<long double>(width) * height;
        }

        const long double sideValue = std::ceil(std::sqrt(area));
        if (sideValue > maxInt)
            return fail(QObject::tr("Packed texture dimensions exceed the supported integer range."));
        vcg::Point2i trialSize(std::max(1, int(sideValue)), std::max(1, int(sideValue)));
        std::vector<vcg::Point2i> offsets;
        vcg::Point2i bounds;
        while (!vcg::RectPacker<float>::PackInt(sizes, trialSize, offsets, bounds)) {
            const long double grown = std::ceil(trialSize.X() * 1.1L);
            const int next = grown >= maxInt
                ? maxInt
                : std::max(trialSize.X() + 1, int(grown));
            if (next <= trialSize.X())
                return fail(QObject::tr("Unable to find a finite texture packing."));
            trialSize = vcg::Point2i(next, next);
        }

        container.size = bounds;
        for (int i = 0; i < int(offsets.size()); ++i)
            m_sources[size_t(container.sources[size_t(i)])].offset = offsets[size_t(i)];
    }
    return true;
}

void TexturePacker::updateMeshUVs()
{
    for (VCGFace &face : m_mesh.face) {
        if (face.IsD() || face.cWT(0).N() < 0)
            continue;
        const int sourceIndex = face.cWT(0).N();
        const Source &source = m_sources[size_t(sourceIndex)];
        const Container &container = m_containers[size_t(source.container)];
        const QImage &image = source.image.get();
        const float x = float(source.offset.X() + m_gutter);
        const float yFromBottom =
            float(container.size.Y() - source.offset.Y() - m_gutter - image.height());

        for (int corner = 0; corner < 3; ++corner) {
            auto &uv = face.WT(corner);
            uv.N() = source.container;
            uv.U() = (std::clamp(uv.U(), 0.0f, 1.0f) * image.width() + x)
                / container.size.X();
            uv.V() = (std::clamp(uv.V(), 0.0f, 1.0f) * image.height() + yFromBottom)
                / container.size.Y();
        }
    }
}

std::vector<QImage> TexturePacker::rasterize()
{
    std::vector<QImage> output;
    output.reserve(m_containers.size());
    for (const Container &container : m_containers) {
        QImage image(
            container.size.X(), container.size.Y(), QImage::Format_RGBA8888);
        if (image.isNull()) {
            fail(QObject::tr("Unable to allocate a packed texture image."));
            return {};
        }
        image.fill(Qt::transparent);
        QPainter painter(&image);
        if (!painter.isActive()) {
            fail(QObject::tr("Unable to paint a packed texture image."));
            return {};
        }
        painter.setCompositionMode(QPainter::CompositionMode_Source);

        for (const int sourceIndex : container.sources) {
            const Source &source = m_sources[size_t(sourceIndex)];
            const QImage &src = source.image.get();
            const int x = source.offset.X();
            const int y = source.offset.Y();
            const int w = src.width();
            const int h = src.height();
            const int g = m_gutter;
            painter.drawImage(QPoint(x + g, y + g), src);
            if (g == 0)
                continue;

            painter.drawImage(QRect(x, y + g, g, h), src, QRect(0, 0, 1, h));
            painter.drawImage(QRect(x + g + w, y + g, g, h), src, QRect(w - 1, 0, 1, h));
            painter.drawImage(QRect(x + g, y, w, g), src, QRect(0, 0, w, 1));
            painter.drawImage(QRect(x + g, y + g + h, w, g), src, QRect(0, h - 1, w, 1));
            painter.drawImage(QRect(x, y, g, g), src, QRect(0, 0, 1, 1));
            painter.drawImage(QRect(x + g + w, y, g, g), src, QRect(w - 1, 0, 1, 1));
            painter.drawImage(QRect(x, y + g + h, g, g), src, QRect(0, h - 1, 1, 1));
            painter.drawImage(
                QRect(x + g + w, y + g + h, g, g), src, QRect(w - 1, h - 1, 1, 1));
        }
        painter.end();
        output.push_back(std::move(image));
    }
    return output;
}

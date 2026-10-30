/****************************************************************************
* MeshLab                                                           o o     *
* Copyright(C) 2005 Visual Computing Lab - ISTI CNR                         *
* GPL-2.0-or-later                                                          *
****************************************************************************/

#pragma once

#include "vcgmesh.h"
#include <vcg/space/rect_packer.h>
#include <QImage>
#include <QString>
#include <functional>
#include <vector>

// Packs complete source images into fewer container images and updates the
// mesh's wedge UVs and texture indices accordingly. A face with no texture
// (index -1) is left as it is.
class TexturePacker
{
public:
    static std::vector<QImage> simplePacking(
        const std::vector<QImage> &sourceImages,
        int outputCount,
        int gutter,
        VCGMesh &mesh,
        QString *error = nullptr);

private:
    struct Source {
        std::reference_wrapper<const QImage> image;
        int container = -1;
        vcg::Point2i offset;
    };
    struct Container {
        vcg::Point2i size;
        std::vector<int> sources;
    };

    TexturePacker(
        const std::vector<QImage> &sourceImages,
        int outputCount,
        int gutter,
        VCGMesh &mesh,
        QString *error);

    bool validate();
    bool findPlacements();
    void updateMeshUVs();
    std::vector<QImage> rasterize();
    bool fail(const QString &message);

    int m_gutter = 0;
    VCGMesh &m_mesh;
    QString *m_error = nullptr;
    std::vector<Source> m_sources;
    std::vector<Container> m_containers;
};

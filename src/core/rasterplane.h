#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <cstdint>

enum class RasterPlaneSemantic {
    None = 0x0000,
    RGBA = 0x0001,
    MaskUInt8 = 0x0002,
    MaskFloat = 0x0004,
    DepthFloat = 0x0008,
    Extra00Float = 0x0100,
    Extra01Float = 0x0200,
    Extra02Float = 0x0400,
    Extra03Float = 0x0800,
    Extra00RGBA = 0x1000,
    Extra01RGBA = 0x2000,
    Extra02RGBA = 0x4000,
    Extra03RGBA = 0x8000
};

struct RasterPlane {
    RasterPlaneSemantic semantic = RasterPlaneSemantic::RGBA;
    QString name;
    QString sourcePath;
    QSize size;
    QImage image;
    // The file at sourcePath holds the image already in the frame of the raster's pinhole
    // camera: written from memory, so not to be undistorted again by the raster's lens. A
    // plane file is otherwise the photo as shot.
    bool sourceUndistorted = false;

    bool hasImage() const { return !image.isNull(); }
    bool hasSourcePath() const { return !sourcePath.trimmed().isEmpty(); }
};

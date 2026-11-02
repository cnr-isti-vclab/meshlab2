#pragma once

#include <vcg/math/lens_distortion.h>
#include <vcg/math/shot.h>
#include <QImage>
#include <QMatrix4x4>
#include <QSize>
#include <QVector2D>
#include <QVector3D>

class QXmlStreamAttributes;
class QXmlStreamWriter;

// The lens of a raster's camera, kept beside its CameraShot, which stays a pinhole camera: it
// relates the photo as shot (the file) to the image the pinhole camera describes (the image in
// memory). Identity for a pinhole photo. See docs/design/proposals/camera_lens_model.md.
using CameraLens = vcg::LensDistortion<double>;

// CameraShot wraps vcg::Shot<float> — a pinhole camera model with intrinsics
// (focal length, pixel size, viewport) and extrinsics (position, rotation), with
// no lens distortion.  It supports ray-casting operations: project(), unproject(),
// depth(), viewMatrix(), projectionMatrix().
//
// Relationship with ViewTrackball:
//   CameraShot stores an eye position and a view direction.  ViewTrackball
//   stores those PLUS an orbit center (a 3D point the camera orbits around).
//   Trackball → Shot is one-way: use cameraEyePosition() + cameraViewDirection()
//   to derive extrinsics; the center is lost.  Shot → Trackball is
//   underdetermined — you must supply either a center point or a distance
//   along the view direction.  No single "right" conversion exists.
class CameraShot
{
public:
    using VcgShot = vcg::Shot<float>;

    // The two projections MeshLab renders. vcg::Camera also names ISOMETRIC and CAVALIERI,
    // which no code here ever drew; a project file carrying one reads as Perspective, with a
    // warning from the loader.
    enum class CameraType {
        Perspective,
        Orthographic
    };

    CameraShot();

    static CameraShot fromVcgShot(const VcgShot &shot);
    static CameraShot defaultPerspectiveForImageSize(
        const QSize &imageSize,
        float focalMm = 50.0f,
        float sensorWidthMm = 36.0f);

    VcgShot toVcgShot() const;

    bool isValid() const;

    CameraType cameraType() const;
    void setCameraType(CameraType type);

    QSize viewportPx() const;
    void setViewportPx(const QSize &size);
    // The same camera for its image at another resolution (vcg::Camera::RescaleViewportPx).
    // Both sizes must be non-empty; whether the aspect ratio still matches is the caller's to check.
    void rescaleViewportPx(const QSize &size);

    QVector2D centerPx() const;
    void setCenterPx(const QVector2D &center);

    QVector2D pixelSizeMm() const;
    void setPixelSizeMm(const QVector2D &pixelSize);

    float focalMm() const;
    void setFocalMm(float focalMm);

    QVector3D viewPoint() const;
    void setViewPoint(const QVector3D &point);

    QVector3D referenceAxis(int axisIndex) const;
    QVector3D viewDirection() const;

    QVector2D project(const QVector3D &worldPoint) const;
    QVector3D unproject(const QVector2D &pixelPoint, float depth) const;
    float depth(const QVector3D &worldPoint) const;
    bool isOrthographic() const { return cameraType() == CameraType::Orthographic; }
    // The ray through a pixel, in world space; `direction` is a unit vector. Perspective: it
    // starts at the viewpoint. Orthographic: it runs along the view direction and starts on
    // the plane through the viewpoint. False for an invalid shot.
    bool viewRayThrough(const QVector2D &pixelPoint, QVector3D &origin, QVector3D &direction) const;
    // Unit vector from a world point towards the camera: towards the viewpoint in
    // perspective, against the view direction (the same everywhere) in orthographic. What a
    // raster filter means by "the direction the camera sees this point from".
    QVector3D directionToCamera(const QVector3D &worldPoint) const;
    QMatrix4x4 viewMatrix() const;
    // Depth range in distances from the viewpoint. Orthographic accepts a near plane at or
    // behind the viewpoint; perspective needs it in front.
    QMatrix4x4 projectionMatrix(float nearPlane, float farPlane) const;

    // Transform the shot's extrinsic parameters
    void applyRigidTransformation(const QMatrix4x4 &m);
    // Unused: it also scaled the intrinsics, and the camera filters moved a camera by a different
    // transform than the meshes with it. Use applySimilarity, which moves only where it stands.
    // void rescalingWorld(float scale);
    void applySimilarity(const QMatrix4x4 &m);

    // The image this pinhole camera describes, resampled from a photo taken through `lens`,
    // of the same size: pixels the photo does not cover are transparent. The photo may be at
    // another resolution than viewportPx(), as the lens is normalized. The photo itself when
    // the lens is the identity, or the camera orthographic, which a real lens cannot be.
    QImage undistortImage(const QImage &photo, const CameraLens &lens) const;

    // The VCGCamera element of a MeshLab project (.mlp) and of a camera file. The lens is in
    // LensModel and LensParams, which old MeshLab ignores; its LensDistortion, the radial
    // model vcg::Camera no longer has, is always written as zeros and a non-zero one read is
    // dropped with a warning.
    static bool readVcgCameraXml(
        const QXmlStreamAttributes &attrs, CameraShot &shot, CameraLens &lens, QString *errorMessage);
    void writeVcgCameraXml(QXmlStreamWriter &xml, const CameraLens &lens) const;

private:
    VcgShot m_shot;
};

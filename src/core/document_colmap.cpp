// Reading a COLMAP sparse model -- cameras, images and points3D, as text or binary -- into the
// raster entries a MeshLab project gives, so that Document::loadMeshLabProject opens it as it
// opens an .mlp. See docs/design/proposals/camera_lens_model.md (Stage 2).

#include "document_internal.h"

#include <QDataStream>

#include <algorithm>
#include <cmath>
#include <map>

namespace DocumentInternal {

namespace {

// The models whose lens is OpenCV's Brown-Conrady one, with their parameters in COLMAP's
// order, mapped onto FULL_OPENCV's fx fy cx cy k1 k2 p1 p2 k3 k4 k5 k6. "f" is fx = fy. The
// ids are COLMAP's, which the binary files use.
struct ColmapCameraModel {
    int id;
    const char *name;
    std::vector<const char *> params;
};
const std::vector<ColmapCameraModel> &colmapCameraModels()
{
    static const std::vector<ColmapCameraModel> models = {
        { 0, "SIMPLE_PINHOLE", { "f", "cx", "cy" } },
        { 1, "PINHOLE", { "fx", "fy", "cx", "cy" } },
        { 2, "SIMPLE_RADIAL", { "f", "cx", "cy", "k1" } },
        { 3, "RADIAL", { "f", "cx", "cy", "k1", "k2" } },
        { 4, "OPENCV", { "fx", "fy", "cx", "cy", "k1", "k2", "p1", "p2" } },
        { 6, "FULL_OPENCV", { "fx", "fy", "cx", "cy", "k1", "k2", "p1", "p2", "k3", "k4", "k5", "k6" } },
    };
    return models;
}

struct ColmapCamera {
    int width = 0;
    int height = 0;
    double fx = 0, fy = 0, cx = 0, cy = 0;
    CameraLens lens;
};

struct ColmapImage {
    double q[4] = {}; // qw qx qy qz, world to camera
    double t[3] = {};
    int cameraId = 0;
    QString name;
    std::vector<std::pair<vcg::Point2d, std::int64_t>> observations; // pixel, point3D id or -1
};

struct ColmapPoint {
    vcg::Point3d position;
    vcg::Color4b color;
};

// A camera from its model's parameters; false, with the message, for a model the lens cannot
// represent (the fisheye ones, FOV, THIN_PRISM_FISHEYE).
bool colmapCamera(const ColmapCameraModel *model, const QString &modelName, int width, int height,
                  const std::vector<double> &values, ColmapCamera &camera, QString &error)
{
    if (!model) {
        error = QObject::tr("COLMAP camera model %1 is not supported").arg(modelName);
        return false;
    }
    if (values.size() < model->params.size()) {
        error = QObject::tr("COLMAP camera of model %1 has %2 parameters, not %3")
                    .arg(modelName).arg(values.size()).arg(model->params.size());
        return false;
    }
    std::map<std::string, double> p;
    for (size_t i = 0; i < model->params.size(); ++i)
        p[model->params[i]] = values[i];
    if (p.count("f"))
        p["fx"] = p["fy"] = p["f"];
    camera.width = width;
    camera.height = height;
    camera.fx = p["fx"];
    camera.fy = p["fy"];
    camera.cx = p["cx"];
    camera.cy = p["cy"];
    camera.lens.k1 = p["k1"];
    camera.lens.k2 = p["k2"];
    camera.lens.p1 = p["p1"];
    camera.lens.p2 = p["p2"];
    camera.lens.k3 = p["k3"];
    camera.lens.k4 = p["k4"];
    camera.lens.k5 = p["k5"];
    camera.lens.k6 = p["k6"];
    return true;
}

// The non-comment lines of a text file. Empty lines are kept: in images.txt an image with no
// observations has an empty second line.
bool readColmapText(const QString &path, QStringList &lines, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QObject::tr("Cannot open '%1'").arg(path);
        return false;
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.startsWith(QLatin1Char('#')))
            lines << line;
    }
    return true;
}

bool readColmapCamerasText(const QString &path, std::map<int, ColmapCamera> &cameras, QString &error)
{
    QStringList lines;
    if (!readColmapText(path, lines, error))
        return false;
    for (const QString &line : lines) {
        const QStringList v = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (v.isEmpty())
            continue;
        if (v.size() < 4) {
            error = QObject::tr("Invalid camera line in '%1': %2").arg(path, line);
            return false;
        }
        const auto &models = colmapCameraModels();
        const auto model = std::find_if(models.begin(), models.end(),
                                        [&](const ColmapCameraModel &m) { return v[1] == QLatin1String(m.name); });
        std::vector<double> values;
        for (int i = 4; i < v.size(); ++i)
            values.push_back(v[i].toDouble());
        ColmapCamera camera;
        if (!colmapCamera(model == models.end() ? nullptr : &*model, v[1], v[2].toInt(), v[3].toInt(),
                          values, camera, error))
            return false;
        cameras[v[0].toInt()] = camera;
    }
    return true;
}

bool readColmapImagesText(const QString &path, std::vector<ColmapImage> &images, QString &error)
{
    QStringList lines;
    if (!readColmapText(path, lines, error))
        return false;
    for (int i = 0; i + 1 < lines.size(); i += 2) {
        const QStringList v = lines[i].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (v.isEmpty())
            break; // the blank line a file may end with
        if (v.size() < 10) {
            error = QObject::tr("Invalid image line in '%1': %2").arg(path, lines[i]);
            return false;
        }
        ColmapImage image;
        for (int k = 0; k < 4; ++k)
            image.q[k] = v[1 + k].toDouble();
        for (int k = 0; k < 3; ++k)
            image.t[k] = v[5 + k].toDouble();
        image.cameraId = v[8].toInt();
        image.name = v.mid(9).join(QLatin1Char(' '));
        const QStringList o = lines[i + 1].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (int k = 0; k + 2 < o.size(); k += 3)
            image.observations.push_back({ vcg::Point2d(o[k].toDouble(), o[k + 1].toDouble()), o[k + 2].toLongLong() });
        images.push_back(std::move(image));
    }
    return true;
}

bool readColmapPointsText(const QString &path, std::map<std::int64_t, ColmapPoint> &points, QString &error)
{
    QStringList lines;
    if (!readColmapText(path, lines, error))
        return false;
    for (const QString &line : lines) {
        const QStringList v = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (v.isEmpty())
            continue;
        if (v.size() < 7) {
            error = QObject::tr("Invalid point line in '%1': %2").arg(path, line);
            return false;
        }
        points[v[0].toLongLong()] = { vcg::Point3d(v[1].toDouble(), v[2].toDouble(), v[3].toDouble()),
                                      vcg::Color4b(v[4].toInt(), v[5].toInt(), v[6].toInt(), 255) };
    }
    return true;
}

// The binary files are little-endian, with 64-bit counts and doubles.
bool openColmapBinary(const QString &path, QFile &file, QDataStream &in, QString &error)
{
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot open '%1'").arg(path);
        return false;
    }
    in.setDevice(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setFloatingPointPrecision(QDataStream::DoublePrecision);
    return true;
}

bool readColmapCamerasBinary(const QString &path, std::map<int, ColmapCamera> &cameras, QString &error)
{
    QFile file;
    QDataStream in;
    if (!openColmapBinary(path, file, in, error))
        return false;
    quint64 count = 0;
    in >> count;
    for (quint64 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        qint32 id = 0, modelId = 0;
        quint64 width = 0, height = 0;
        in >> id >> modelId >> width >> height;
        const auto &models = colmapCameraModels();
        const auto model = std::find_if(models.begin(), models.end(),
                                        [&](const ColmapCameraModel &m) { return m.id == modelId; });
        // A model this reader does not know has a parameter count it does not know either, so
        // nothing after it could be read
        if (model == models.end()) {
            error = QObject::tr("COLMAP camera model %1 is not supported").arg(modelId);
            return false;
        }
        std::vector<double> values(model->params.size());
        for (double &value : values)
            in >> value;
        ColmapCamera camera;
        if (!colmapCamera(&*model, QLatin1String(model->name), int(width), int(height), values, camera, error))
            return false;
        cameras[id] = camera;
    }
    if (in.status() != QDataStream::Ok) {
        error = QObject::tr("'%1' is truncated").arg(path);
        return false;
    }
    return true;
}

bool readColmapImagesBinary(const QString &path, std::vector<ColmapImage> &images, QString &error)
{
    QFile file;
    QDataStream in;
    if (!openColmapBinary(path, file, in, error))
        return false;
    quint64 count = 0;
    in >> count;
    for (quint64 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        ColmapImage image;
        qint32 id = 0, cameraId = 0;
        in >> id;
        for (double &q : image.q)
            in >> q;
        for (double &t : image.t)
            in >> t;
        in >> cameraId;
        image.cameraId = cameraId;
        QByteArray name;
        for (qint8 c = 0; in.status() == QDataStream::Ok;) {
            in >> c;
            if (c == 0)
                break;
            name.append(char(c));
        }
        image.name = QString::fromUtf8(name);
        quint64 observations = 0;
        in >> observations;
        for (quint64 k = 0; k < observations && in.status() == QDataStream::Ok; ++k) {
            double x = 0, y = 0;
            qint64 point = 0;
            in >> x >> y >> point;
            image.observations.push_back({ vcg::Point2d(x, y), point });
        }
        images.push_back(std::move(image));
    }
    if (in.status() != QDataStream::Ok) {
        error = QObject::tr("'%1' is truncated").arg(path);
        return false;
    }
    return true;
}

bool readColmapPointsBinary(const QString &path, std::map<std::int64_t, ColmapPoint> &points, QString &error)
{
    QFile file;
    QDataStream in;
    if (!openColmapBinary(path, file, in, error))
        return false;
    quint64 count = 0;
    in >> count;
    for (quint64 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        quint64 id = 0, trackLength = 0;
        double x = 0, y = 0, z = 0, reprojectionError = 0;
        quint8 r = 0, g = 0, b = 0;
        in >> id >> x >> y >> z >> r >> g >> b >> reprojectionError >> trackLength;
        in.skipRawData(int(trackLength * 8)); // (image id, point2D index), two int32 each
        points[std::int64_t(id)] = { vcg::Point3d(x, y, z), vcg::Color4b(r, g, b, 255) };
    }
    if (in.status() != QDataStream::Ok) {
        error = QObject::tr("'%1' is truncated").arg(path);
        return false;
    }
    return true;
}

// The pinhole camera of an image. COLMAP's pose is world to camera, with OpenCV's axes (x right,
// y down, looking down +z); vcg's camera looks down -z with y up, so its rotation is
// diag(1, -1, -1) R and its viewpoint the camera centre -R^T t. COLMAP puts pixel centres at
// k + 0.5 as vcg does, but counts rows from the top: CenterPx is (cx, height - cy). Only the
// ratio of FocalMm to PixelSizeMm matters; 0.01 mm is an arbitrary pixel size.
CameraShot colmapShot(const ColmapImage &image, const ColmapCamera &camera)
{
    const double w = image.q[0], x = image.q[1], y = image.q[2], z = image.q[3];
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    const double qw = w / n, qx = x / n, qy = y / n, qz = z / n;
    const double r[3][3] = {
        { 1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy) },
        { 2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx) },
        { 2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy) },
    };
    vcg::Point3d centre(0, 0, 0);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            centre[i] -= r[j][i] * image.t[j];

    CameraShot::VcgShot shot;
    vcg::Matrix44f rot;
    rot.SetIdentity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rot[i][j] = float(i == 0 ? r[i][j] : -r[i][j]);
    shot.Extrinsics.SetRot(rot);
    shot.Extrinsics.SetTra(vcg::Point3f::Construct(centre));

    const double pixelSizeMm = 0.01;
    vcg::Camera<float> &cam = shot.Intrinsics;
    cam.cameraType = vcg::Camera<float>::PERSPECTIVE;
    cam.FocalMm = float(camera.fx * pixelSizeMm);
    cam.PixelSizeMm = vcg::Point2f(float(pixelSizeMm), float(pixelSizeMm * camera.fx / camera.fy));
    cam.ViewportPx = vcg::Point2i(camera.width, camera.height);
    cam.CenterPx = vcg::Point2f(float(camera.cx), float(camera.height - camera.cy));
    return CameraShot::fromVcgShot(shot);
}

// COLMAP keeps the image names relative to the folder it was given, by convention images/
// beside sparse/. From the model's folder up two levels (the model is in sparse/0, sparse/ or
// the top folder itself), the first folder holding the first image, in images/ or in itself.
QDir colmapImageFolder(const QString &modelFolder, const QString &firstImage)
{
    QDir dir(modelFolder);
    for (int up = 0; up < 3; ++up) {
        for (const QString &candidate : { dir.filePath(QStringLiteral("images")), dir.path() }) {
            if (QFileInfo::exists(QDir(candidate).filePath(firstImage)))
                return QDir(candidate);
        }
        if (!dir.cdUp())
            break;
    }
    return QDir(QFileInfo(modelFolder).dir().filePath(QStringLiteral("images")));
}

} // namespace

bool isColmapModelFile(const QString &path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    return name == QStringLiteral("cameras.txt") || name == QStringLiteral("cameras.bin");
}

bool parseColmapModel(const QString &camerasPath, ColmapModel &model, QString &error)
{
    const QFileInfo info(camerasPath);
    const bool binary = info.suffix().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0;
    const QDir folder = info.dir();
    const QString suffix = binary ? QStringLiteral(".bin") : QStringLiteral(".txt");
    const QString imagesPath = folder.filePath(QStringLiteral("images") + suffix);
    const QString pointsPath = folder.filePath(QStringLiteral("points3D") + suffix);

    std::map<int, ColmapCamera> cameras;
    std::vector<ColmapImage> images;
    std::map<std::int64_t, ColmapPoint> points;
    if (!(binary ? readColmapCamerasBinary(camerasPath, cameras, error)
                 : readColmapCamerasText(camerasPath, cameras, error))
        || !(binary ? readColmapImagesBinary(imagesPath, images, error)
                    : readColmapImagesText(imagesPath, images, error)))
        return false;
    // A model may come without its points; the cameras are what it is opened for
    if (QFileInfo::exists(pointsPath)
        && !(binary ? readColmapPointsBinary(pointsPath, points, error)
                    : readColmapPointsText(pointsPath, points, error)))
        return false;

    std::map<std::int64_t, int> pointIndex;
    for (const auto &[id, point] : points) {
        pointIndex[id] = int(model.points.size());
        model.points.push_back(point.position);
        model.pointColors.push_back(point.color);
    }

    // COLMAP lists images by id, which says nothing about how they were taken; their names
    // usually do, and give the rasters an order a reader can find one in
    std::sort(images.begin(), images.end(),
              [](const ColmapImage &a, const ColmapImage &b) { return a.name < b.name; });
    const QDir imageFolder = colmapImageFolder(folder.absolutePath(), images.empty() ? QString() : images.front().name);
    for (const ColmapImage &image : images) {
        const auto camera = cameras.find(image.cameraId);
        if (camera == cameras.end()) {
            error = QObject::tr("Image '%1' refers to camera %2, which '%3' does not have")
                        .arg(image.name).arg(image.cameraId).arg(camerasPath);
            return false;
        }
        MeshLabProjectRasterEntry raster;
        raster.label = image.name;
        raster.shot = colmapShot(image, camera->second);
        raster.lens = camera->second.lens;
        MeshLabProjectPlaneEntry plane;
        plane.name = image.name;
        plane.sourcePath = imageFolder.filePath(image.name);
        raster.planes.push_back(plane);
        for (const auto &[pixel, pointId] : image.observations) {
            const auto point = pointIndex.find(pointId);
            if (point != pointIndex.end())
                model.observations.push_back({ int(model.rasters.size()), pixel, point->second });
        }
        model.rasters.push_back(std::move(raster));
    }
    return true;
}

std::vector<double> colmapReprojectionErrors(const ColmapModel &model)
{
    std::vector<double> errors;
    errors.reserve(model.observations.size());
    for (const ColmapModel::Observation &o : model.observations) {
        const MeshLabProjectRasterEntry &raster = model.rasters[size_t(o.raster)];
        const CameraShot &shot = raster.shot;
        const vcg::Point3d &p = model.points[size_t(o.point)];
        const QVector2D projected = shot.project(QVector3D(float(p[0]), float(p[1]), float(p[2])));
        // Back to COLMAP's pixels, y down, then through the lens to the photo as shot
        const double fx = shot.focalMm() / shot.pixelSizeMm().x();
        const double fy = shot.focalMm() / shot.pixelSizeMm().y();
        const double cx = shot.centerPx().x();
        const double cy = shot.viewportPx().height() - shot.centerPx().y();
        const double v = shot.viewportPx().height() - projected.y();
        const vcg::Point2d d = raster.lens.Distort(vcg::Point2d((projected.x() - cx) / fx, (v - cy) / fy));
        errors.push_back(std::hypot(d[0] * fx + cx - o.pixel[0], d[1] * fy + cy - o.pixel[1]));
    }
    return errors;
}

} // namespace DocumentInternal

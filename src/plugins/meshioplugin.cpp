#include "meshioplugin.h"

#include <vcg/complex/algorithms/update/position.h>

#include <QObject>

namespace UpAxisOption {

MeshFilterParameterDescriptor descriptor()
{
    MeshFilterParameterDescriptor rotate;
    rotate.id = QString::fromLatin1(kId);
    rotate.label = QObject::tr("Rotate Y-up to Z-up");
    rotate.helpMarkdown = QObject::tr(
        "The file says its up direction is +Y. MeshLab keeps a file's own frame, so by default "
        "its coordinates are read as they are, and in MeshLab's default view, whose up is +Y too, "
        "it stands as the file means. On, the coordinates are rotated by +90 degrees about X, so "
        "that +Y becomes +Z, as tools whose up is +Z expect. Saving writes the rotated "
        "coordinates. Nothing is done to a file that is not Y-up.");
    rotate.type = MeshFilterParameterType::Bool;
    rotate.defaultValue = false;
    return rotate;
}

bool requested(const MeshIOLoadContext &context)
{
    return context.options.value(QString::fromLatin1(kId), false).toBool();
}

void apply(VCGMesh &mesh, bool fileIsYUp, MeshIOLoadContext &context)
{
    if (!fileIsYUp)
        return;
    if (!requested(context)) {
        context.messages << QObject::tr("the file is Y-up; its coordinates are kept as they are "
                                        "(the Rotate Y-up to Z-up load option turns it Z-up).");
        return;
    }
    vcg::Matrix44f up;
    up.SetRotateDeg(90.0f, vcg::Point3f(1, 0, 0));
    vcg::tri::UpdatePosition<VCGMesh>::Matrix(mesh, up, true);
    context.messages << QObject::tr("the file is Y-up and was rotated by +90 degrees about X to Z-up, "
                                    "as the Rotate Y-up to Z-up load option asks: its coordinates "
                                    "are not the file's.");
}

}

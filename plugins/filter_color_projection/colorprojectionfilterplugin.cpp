#include "colorprojectionfilterplugin.h"

#include "camerashot.h"
#include "document.h"
#include "filterparam.h"
#include "meshfilterpluginmanager.h"
#include "textureassociationutils.h"
#include "vcgmesh.h"

#include "floatbuffer.h"
#include "src/core/pushpull.h"
#include "softdepthbuffer.h"

#include <vcg/complex/algorithms/point_sampling.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QDir>
#include <QImage>
#include <QMatrix4x4>
#include <QRgb>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr QLatin1StringView kFilterSingleProj(
    "transfer_color_from_current_raster_to_vertex");
constexpr QLatin1StringView kFilterMultiProj(
    "transfer_color_from_visible_rasters_to_vertex");
constexpr QLatin1StringView kFilterMultiTexture(
    "transfer_color_from_visible_rasters_to_texture");

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult r;
    r.success = false;
    r.documentModified = false;
    r.errorMessage = message;
    return r;
}

MeshFilterRunResult success(const QStringList &info = {})
{
    MeshFilterRunResult r;
    r.success = true;
    r.documentModified = true;
    r.infoMessages = info;
    return r;
}



// ---------------------------------------------------------------------------
// near/far, texels
// ---------------------------------------------------------------------------

static void calculateNearFar(const Document &doc, const VCGMesh &mesh,
    const QMatrix4x4 &tf, std::vector<float> &nr, std::vector<float> &fr)
{
    int n = doc.rasterCount();
    nr.assign(size_t(n),  1000000.0f);
    fr.assign(size_t(n), -1000000.0f);
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD()) continue;
        QVector3D wv = transformPoint(tf, v.cP());
        for (int ri = 0; ri < n; ++ri) {
            const auto &re = doc.raster(ri);
            if (!re.shot.isValid()) continue;
            QVector2D pp = re.shot.project(wv);
            QSize vp = re.shot.viewportPx();
            if (pp.x() > 0.0f && pp.y() > 0.0f && pp.x() < float(vp.width()) && pp.y() < float(vp.height())) {
                float d = re.shot.depth(wv);
                if (d < nr[size_t(ri)]) nr[size_t(ri)] = d;
                if (d > fr[size_t(ri)]) fr[size_t(ri)] = d;
            }
        }
    }
    for (int ri = 0; ri < n; ++ri)
        if (nr[size_t(ri)] == 1000000.0f || fr[size_t(ri)] == -1000000.0f)
            nr[size_t(ri)] = fr[size_t(ri)] = 0.0f;
}

struct TexelDesc { vcg::Point2i tc; QVector3D mp, mn; };
struct TexelAccum { float w = 0, r = 0, g = 0, b = 0; };

class TexFillerSampler {
public:
    QImage &img; QMatrix4x4 tf, nm;
    std::vector<TexelDesc> *td = nullptr;
    std::vector<TexelAccum> *ta = nullptr;
    const VCGFace *cf = nullptr; int fn = 0, fc = 0, st = 0, off = 100;
    TexFillerSampler(QImage &i, const QMatrix4x4 &t, const QMatrix4x4 &n)
        : img(i), tf(t), nm(n) {}
    void AddTextureSample(const VCGFace &f, const VCGMesh::CoordType &p,
                          const vcg::Point2i &tp, float) {
        vcg::Point3f mp = f.cP(0) * p[0] + f.cP(1) * p[1] + f.cP(2) * p[2];
        vcg::Point3f mn = (f.cV(0)->N() * p[0] + f.cV(1)->N() * p[1] + f.cV(2)->N() * p[2]).Normalize();
        TexelDesc d;
        d.tc = tp;
        d.mp = tf.map(QVector3D(mp[0], mp[1], mp[2]));
        d.mn = nm.mapVector(QVector3D(mn[0], mn[1], mn[2])).normalized();
        td->push_back(d);
        ta->push_back({});
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

QString ColorProjectionFilterPlugin::pluginId() const
{ return QStringLiteral("meshlab2.filter.color_projection"); }

QString ColorProjectionFilterPlugin::name() const
{ return QStringLiteral("Raster Projection Filters"); }

MeshFilterRunResult ColorProjectionFilterPlugin::runFilter(
    const QString &fid, const FilterParams &p, Document &doc) const
{
    using namespace vcg::tri::io;

    int mi = doc.currentMeshIndex();
    if (mi < 0 || mi >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    Document::MeshEntry &ent = doc.mesh(mi);
    VCGMesh &m = ent.mesh;

    // --- single proj ---
    if (fid == QString::fromLatin1(kFilterSingleProj)) {
        float eta = float(p.getDouble(QStringLiteral("deptheta"), 0.5));
        bool onSel = p.getBool(QStringLiteral("onselection"), false);
        bool preserveOccluded = p.getBool(QStringLiteral("preserveoccluded"), false);
        QColor blank = p.getColor(QStringLiteral("blankColor"), QColor(0, 0, 0, 255));

        int ri = doc.currentRasterIndex();
        if (ri < 0 || ri >= doc.rasterCount())
            return fail(QObject::tr("No current raster selected."));
        Document::RasterEntry &re = doc.raster(ri);
        const CameraShot &shot = re.shot;
        if (!shot.isValid()) return fail(QObject::tr("Invalid camera."));
        if (re.planes.empty()) return fail(QObject::tr("No planes."));
        RasterPlane *rp = re.currentPlane();
        if (!rp) return fail(QObject::tr("No plane."));
        Document::ensureRasterPlaneImage(*rp);
        if (rp->image.isNull()) return fail(QObject::tr("No image."));

        const QImage &rimg = rp->image;
        int iw = shot.viewportPx().width(), ih = shot.viewportPx().height();
        QVector3D cz = shot.referenceAxis(2);

        auto dbuf = buildDepthBuffer(shot, m, ent.transform);


        for (VCGVertex &v : m.vert) {
            if (v.IsD() || (onSel && !v.IsS())) continue;
            QVector3D wv = transformPoint(ent.transform, v.cP());
            QVector2D pp = shot.project(wv);
            if (pp.x() <= 0 || pp.y() <= 0 || pp.x() >= float(iw) || pp.y() >= float(ih)) {
                v.C() = vcg::Color4b(blank.red(), blank.green(), blank.blue(), blank.alpha());
                continue;
            }
            QVector3D pray = (shot.viewPoint() - wv).normalized();
            if (QVector3D::dotProduct(pray, -cz) > 0.0f) {
                v.C() = vcg::Color4b(blank.red(), blank.green(), blank.blue(), blank.alpha());
                continue;
            }
            float d = shot.depth(wv);
            float pd = dbuf->getval(int(pp.x()), int(pp.y()));
            if (d <= pd + eta) {
                QRgb c = rimg.pixel(int(pp.x()), ih - int(pp.y()));
                v.C() = vcg::Color4b(qRed(c), qGreen(c), qBlue(c), 255);
            } else if (!preserveOccluded) {
                v.C() = vcg::Color4b(blank.red(), blank.green(), blank.blue(), blank.alpha());
            }
        }
        ent.ioMask |= Mask::IOM_VERTCOLOR;
        doc.markMeshGeometryChanged(mi, QObject::tr("Projected raster color onto '%1'.").arg(ent.name));
        return success();
    }

    // --- multi proj ---
    if (fid == QString::fromLatin1(kFilterMultiProj)) {
        float eta = float(p.getDouble(QStringLiteral("deptheta"), 0.5));
        bool onSel = p.getBool(QStringLiteral("onselection"), false);
        bool ua = p.getBool(QStringLiteral("useangle"), true);
        bool ud = p.getBool(QStringLiteral("usedistance"), true);
        bool ub = p.getBool(QStringLiteral("useborders"), true);
        bool us = p.getBool(QStringLiteral("usesilhouettes"), true);
        bool ualpha = p.getBool(QStringLiteral("usealpha"), false);
        bool preserveOccluded = p.getBool(QStringLiteral("preserveoccluded"), false);
        QColor blank = p.getColor(QStringLiteral("blankColor"), QColor(0,0,0,0));

        if (doc.rasterCount() == 0) return fail(QObject::tr("No rasters."));
        if (ua) vcg::tri::UpdateNormal<VCGMesh>::PerVertex(m);

        std::vector<float> nr, fr;
        calculateNearFar(doc, m, ent.transform, nr, fr);
        float allMx = -1000000.0f, allMn = 1000000.0f;
        for (int ri = 0; ri < doc.rasterCount(); ++ri) {
            if (fr[size_t(ri)] > allMx) allMx = fr[size_t(ri)];
            if (nr[size_t(ri)] < allMn) allMn = nr[size_t(ri)];
        }

        int vn = int(m.vert.size());
        std::vector<double> wts(size_t(vn), 0), ar(size_t(vn), 0), ag(size_t(vn), 0), ab(size_t(vn), 0);
        QMatrix4x4 nmat = ent.transform.inverted().transposed();

        int ci = 0;
        for (int ri = 0; ri < doc.rasterCount(); ++ri) {
            Document::RasterEntry &re = doc.raster(ri);
            if (!re.visible || !re.shot.isValid() || re.planes.empty()) { ++ci; continue; }
            const CameraShot &shot = re.shot;
            RasterPlane *rp = re.currentPlane();
            if (!rp) { ++ci; continue; }
            Document::ensureRasterPlaneImage(*rp);
            if (!rp->hasImage()) { ++ci; continue; }
            const QImage &rimg = rp->image;
            int iw = shot.viewportPx().width(), ih = shot.viewportPx().height();
            QVector3D cz = shot.referenceAxis(2);

            auto dbuf = buildDepthBuffer(shot, m, ent.transform);
            FloatBuffer *sb = nullptr;
            float msd = float(iw + ih);
            if (us) {
                sb = new FloatBuffer(); sb->init(iw, ih);
                sb->applysobel(*dbuf); sb->initborder(*dbuf);
                float d = sb->distancefield(); if (d > 0) msd = d;
            }

            int bi = 0;
            for (const VCGVertex &v : m.vert) {
                if (!v.IsD() && (!onSel || v.IsS())) {
                    QVector3D wv = transformPoint(ent.transform, v.cP());
                    QVector2D pp = shot.project(wv);
                    if (pp.x() >= 0 && pp.y() >= 0 && pp.x() < float(iw) && pp.y() < float(ih)) {
                        QVector3D pray = (shot.viewPoint() - wv).normalized();
                        if (QVector3D::dotProduct(pray, -cz) <= 0) {
                            float d = shot.depth(wv);
                            float pd = dbuf->getval(int(pp.x()), int(pp.y()));
                            if (d <= pd + eta) {
                                QRgb c = rimg.pixel(int(pp.x()), ih - int(pp.y()));
                                double pw = 1.0;
                                if (ua) {
                                    const vcg::Point3f &mn = v.cN();
                                    QVector3D wn = nmat.mapVector(QVector3D(mn[0], mn[1], mn[2])).normalized();
                                    pw *= double(std::min(1.0f, std::abs(QVector3D::dotProduct(wn, pray))));
                                }
                                if (ud && allMx > allMn) {
                                    float dw = 1.0f - (d - allMn*0.99f) / (allMx*1.01f - allMn*0.99f);
                                    pw *= double(dw*dw);
                                }
                                if (ub) {
                                    pw *= std::min(1.0 - std::abs(pp.x() - iw*0.5) / (iw*0.5),
                                                   1.0 - std::abs(pp.y() - ih*0.5) / (ih*0.5));
                                }
                                if (us && sb) pw *= double(sb->getval(int(pp.x()), int(pp.y())) / msd);
                                if (ualpha) pw *= qAlpha(c) / 255.0;
                                wts[size_t(bi)] += pw;
                                ar[size_t(bi)] += qRed(c)   * pw / 255.0;
                                ag[size_t(bi)] += qGreen(c) * pw / 255.0;
                                ab[size_t(bi)] += qBlue(c)  * pw / 255.0;
                            }
                        }
                    }
                }
                ++bi;
            }
            delete sb; ++ci;
        }

        bool hasB = blank.red() || blank.green() || blank.blue() || blank.alpha();
        int bi = 0;
        for (VCGVertex &v : m.vert) {
            if (!v.IsD() && (!onSel || v.IsS())) {
                if (wts[size_t(bi)] > 0) {
                    double w = wts[size_t(bi)];
                    v.C() = vcg::Color4b(uchar(ar[size_t(bi)]/w*255), uchar(ag[size_t(bi)]/w*255),
                                         uchar(ab[size_t(bi)]/w*255), 255);
                } else if (!preserveOccluded && hasB) {
                    v.C() = vcg::Color4b(blank.red(), blank.green(), blank.blue(), blank.alpha());
                }
            }
            ++bi;
        }
        ent.ioMask |= Mask::IOM_VERTCOLOR;
        doc.markMeshGeometryChanged(mi, QObject::tr("Projected rasters onto '%1'.").arg(ent.name));
        return success();
    }

    // --- multi texture ---
    if (fid == QString::fromLatin1(kFilterMultiTexture)) {
        if ((ent.ioMask & Mask::IOM_WEDGTEXCOORD) == 0)
            return fail(QObject::tr("No wedge texcoords."));
        float eta = float(p.getDouble(QStringLiteral("deptheta"), 0.5));
        int ts = p.getInt(QStringLiteral("texsize"), 1024);
        bool dr = p.getBool(QStringLiteral("dorefill"), true);
        bool ua = p.getBool(QStringLiteral("useangle"), true);
        bool ud = p.getBool(QStringLiteral("usedistance"), true);
        bool ub = p.getBool(QStringLiteral("useborders"), true);
        bool us = p.getBool(QStringLiteral("usesilhouettes"), true);
        bool ualpha = p.getBool(QStringLiteral("usealpha"), false);
        QString tn = p.getFileSave(QStringLiteral("textName")).trimmed();
        if (ts <= 0) return fail(QObject::tr("texsize must be positive."));
        if (tn.isEmpty()) return fail(QObject::tr("Texture file not specified."));
        if (doc.rasterCount() == 0) return fail(QObject::tr("No rasters."));

        if (ua) vcg::tri::UpdateNormal<VCGMesh>::PerVertex(m);
        QImage img(QSize(ts, ts), QImage::Format_ARGB32);
        img.fill(qRgba(0,0,0,0));
        if (dr) {
            VCGMeshFFAdjScope _a(m);
            vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(m);
            vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(m);
        }

        std::vector<TexelDesc> td; std::vector<TexelAccum> ta;
        td.reserve(size_t(ts*ts)); ta.reserve(size_t(ts*ts));
        QMatrix4x4 nmat = ent.transform.inverted().transposed();
        TexFillerSampler tfs(img, ent.transform, nmat);
        tfs.td = &td; tfs.ta = &ta;
        {
            VCGMeshFFAdjScope _a2(m);
            vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(m);
            vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(m);
            vcg::tri::SurfaceSampling<VCGMesh, TexFillerSampler>::Texture(m, tfs, ts, ts, true);
            vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
            vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(m);
        }
        for (int y = 0; y < ts; ++y)
            for (int x = 0; x < ts; ++x) {
                QRgb px = img.pixel(x, y);
                if (qAlpha(px) < 255 && qAlpha(px) > 0) img.setPixel(x, y, px | 0xff000000u);
            }

        std::vector<float> nr, fr;
        calculateNearFar(doc, m, ent.transform, nr, fr);
        float allMx = -1000000.0f, allMn = 1000000.0f;
        for (int ri=0; ri<doc.rasterCount(); ++ri) {
            if (fr[size_t(ri)] > allMx) allMx = fr[size_t(ri)];
            if (nr[size_t(ri)] < allMn) allMn = nr[size_t(ri)];
        }

        int ci = 0;
        for (int ri=0; ri<doc.rasterCount(); ++ri) {
            Document::RasterEntry &re = doc.raster(ri);
            if (!re.visible || !re.shot.isValid() || re.planes.empty()) { ++ci; continue; }
            const CameraShot &shot = re.shot;
            RasterPlane *rp = re.currentPlane();
            if (!rp) { ++ci; continue; }
            Document::ensureRasterPlaneImage(*rp);
            if (!rp->hasImage()) { ++ci; continue; }
            const QImage &rimg = rp->image;
            int iw = shot.viewportPx().width(), ih = shot.viewportPx().height();
            QVector3D cz = shot.referenceAxis(2);

            auto dbuf = buildDepthBuffer(shot, m, ent.transform);
            FloatBuffer *sb = nullptr;
            float msd = float(iw+ih);
            if (us) { sb = new FloatBuffer(); sb->init(iw,ih);
                      sb->applysobel(*dbuf); sb->initborder(*dbuf);
                      float d = sb->distancefield(); if (d>0) msd=d; }

            for (size_t tc=0; tc<td.size(); ++tc) {
                QVector3D &mp = td[tc].mp, &mn = td[tc].mn;
                QVector2D pp = shot.project(mp);
                if (pp.x() <= 0 || pp.y() <= 0 || pp.x() >= float(iw) || pp.y() >= float(ih)) continue;
                QVector3D pray = (shot.viewPoint() - mp).normalized();
                if (QVector3D::dotProduct(pray, -cz) > 0) continue;
                float d = shot.depth(mp);
                float pd = dbuf->getval(int(pp.x()), int(pp.y()));
                if (d > pd + eta) continue;
                QRgb c = rimg.pixel(int(pp.x()), ih - int(pp.y()));
                double pw = 1.0;
                if (ua) pw *= double(std::min(1.0f, std::abs(QVector3D::dotProduct(mn, pray))));
                if (ud && allMx>allMn) {
                    float dw = 1.0f - (d-allMn*0.99f)/(allMx*1.01f-allMn*0.99f);
                    pw *= double(dw*dw);
                }
                if (ub) pw *= std::min(1.0-std::abs(pp.x()-iw*0.5)/(iw*0.5), 1.0-std::abs(pp.y()-ih*0.5)/(ih*0.5));
                if (us && sb) pw *= double(sb->getval(int(pp.x()), int(pp.y()))/msd);
                if (ualpha) pw *= qAlpha(c)/255.0;
                ta[tc].w += float(pw);
                ta[tc].r += float(qRed(c)*pw/255.0);
                ta[tc].g += float(qGreen(c)*pw/255.0);
                ta[tc].b += float(qBlue(c)*pw/255.0);
            }
            delete sb; ++ci;
        }

        for (size_t tc=0; tc<td.size(); ++tc) {
            int ix = td[tc].tc.X(), iy = ts-1-td[tc].tc.Y();
            if (ta[tc].w > 0) {
                float w = ta[tc].w;
                img.setPixel(ix, iy, qRgba(int(ta[tc].r/w*255), int(ta[tc].g/w*255), int(ta[tc].b/w*255), 255));
            } else img.setPixel(ix, iy, qRgba(0,0,0,0));
        }
        if (dr) vcg::PullPush(img, qRgba(0,0,0,0));

        QString se;
        if (!TextureAssociationUtils::saveImages({tn}, {img}, se)) return fail(se);
        auto oa = TextureAssociationUtils::makeTextureAssetsFromSavedImages({tn}, {img});
        TextureAssociationUtils::replaceTextureAssociations(ent, oa);
        ent.ioMask |= Mask::IOM_WEDGTEXCOORD;
        doc.markMeshMaterialChanged(mi, QObject::tr("Projected rasters onto texture '%1'.").arg(tn));
        return success({QObject::tr("Saved: %1").arg(tn)});
    }

    return fail(QObject::tr("Unknown filter: %1").arg(fid));
}

void registerColorProjectionFilterPlugin(MeshFilterPluginManager &pm)
{
    pm.registerPlugin(std::make_unique<ColorProjectionFilterPlugin>());
}

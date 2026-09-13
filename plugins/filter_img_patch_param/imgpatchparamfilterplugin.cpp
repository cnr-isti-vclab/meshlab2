#include "imgpatchparamfilterplugin.h"

#include "camerashot.h"
#include "document.h"
#include "filterparam.h"
#include "meshfilterpluginmanager.h"
#include "textureassociationutils.h"
#include "vcgmesh.h"

#include "floatbuffer.h"
#include "softdepthbuffer.h"

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/math/similarity2.h>
#include <vcg/space/rect_packer.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QMap>
#include <QMatrix4x4>
#include <QRgb>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <vector>

using NeighbSet = std::set<VCGFace *>;

namespace {

constexpr QLatin1StringView kPatchParamOnly(
    "parametrize_from_registered_rasters");
constexpr QLatin1StringView kPatchParamTex(
    "parametrize_from_registered_rasters_with_texture");
constexpr QLatin1StringView kCoverageVert(
    "compute_vertex_scalar_from_raster_coverage");
constexpr QLatin1StringView kCoverageFace(
    "compute_face_scalar_from_raster_coverage");

MeshFilterRunResult fail(const QString &m) { return {false, false, m}; }
MeshFilterRunResult ok(const QStringList &info = {}) {
    MeshFilterRunResult r; r.success = true; r.documentModified = true;
    r.infoMessages = info; return r;
}

// ---------------------------------------------------------------------------
// Patch data structures
// ---------------------------------------------------------------------------

struct TriangleUV { vcg::TexCoord2f v[3]; };

struct Patch {
    int                     refRaster;
    std::vector<VCGFace *>  faces;
    std::vector<VCGFace *>  boundary;
    std::vector<TriangleUV> boundaryUV;
    vcg::Box2f              bbox;
    QMatrix4x4              img2tex;
    bool                    valid = true;
};

using PatchVec       = QVector<Patch>;
using RasterPatchMap = QMap<int, PatchVec>;

// ---------------------------------------------------------------------------
// Visibility weights
// ---------------------------------------------------------------------------

enum WeightMask : int {
    W_ORIENTATION = 0x01,
    W_DISTANCE    = 0x02,
    W_IMG_BORDER  = 0x04,
    W_IMG_ALPHA   = 0x08,
};

struct FaceVisInfo {
    float          refWeight = -std::numeric_limits<float>::max();
    int            ref = -1;
    std::vector<int> visible;

    void add(float w, int ri) {
        visible.push_back(ri);
        if (w > refWeight) { refWeight = w; ref = ri; }
    }
    bool contains(int ri) const {
        return std::find(visible.begin(), visible.end(), ri) != visible.end();
    }
};

// ---------------------------------------------------------------------------
// Raster context (per-raster precomputed data)
// ---------------------------------------------------------------------------

struct RasterContext {
    const CameraShot *shot = nullptr;
    std::unique_ptr<FloatBuffer> dbuf;
    DepthBufferVertexCache vcache;
    int iw = 0, ih = 0;
    QVector3D cz;
    QImage rimg;
    float nearPlane = 0.1f;
    float farPlane  = 1000.0f;
    float depthRangeInv = 1.0f;
};

// ---------------------------------------------------------------------------
// Neighborhood helpers
// ---------------------------------------------------------------------------

void getNeighbors(VCGVertex *v, NeighbSet &neighb)
{
    if (!v->IsVFInitialized()) return;
    vcg::face::Pos<VCGFace> p(v->VFp(), v), ori = p;
    do { neighb.insert(p.F()); p.FlipF(); p.FlipE(); } while (ori != p);
}

void getFaceNeighbors(VCGFace *f, NeighbSet &neighb)
{
    getNeighbors(f->V(0), neighb);
    getNeighbors(f->V(1), neighb);
    getNeighbors(f->V(2), neighb);
}

// ---------------------------------------------------------------------------
// Per-vertex visibility test
// ---------------------------------------------------------------------------

bool checkVertVisibleFast(int vi, const RasterContext &rc, float depthEpsilon)
{
    if (size_t(vi) >= rc.vcache.proj.size()) return false;
    if (!rc.vcache.frontFace[size_t(vi)]) return false;
    const QVector2D &pp = rc.vcache.proj[size_t(vi)];
    const float d = rc.vcache.depth[size_t(vi)];
    if (d <= 0.0f) return false;
    if (pp.x() <= 0 || pp.y() <= 0 || pp.x() >= float(rc.iw) || pp.y() >= float(rc.ih))
        return false;
    float pd = rc.dbuf->getval(int(pp.x()), int(pp.y()));
    if (pd <= 0.0f) return false;
    return d <= pd + depthEpsilon;
}

// ---------------------------------------------------------------------------
// Per-face weight computation
// ---------------------------------------------------------------------------

float faceWeight(const VCGMesh &mesh, VCGFace &f,
                  const QMatrix4x4 &tx, int ri, const RasterContext &rc,
                  int weightMask)
{
    vcg::Point3f centroid = (f.cP(0) + f.cP(1) + f.cP(2)) / 3.0f;
    vcg::Point3f wcP(transformPoint(tx, centroid).x(),
                     transformPoint(tx, centroid).y(),
                     transformPoint(tx, centroid).z());

    float w = 1.0f;

    if (weightMask & W_ORIENTATION) {
        vcg::Point3f fn = f.cN();
        if (fn == vcg::Point3f(0, 0, 0))
            fn = ((f.cP(1) - f.cP(0)) ^ (f.cP(2) - f.cP(0))).Normalize();
        vcg::Point3f vp = rc.shot->toVcgShot().GetViewPoint();
        float dot = (vp - wcP).Normalize() * fn;
        w *= dot;
    }

    if ((weightMask & W_DISTANCE) && w > 0.0f) {
        float d = (rc.shot->toVcgShot().GetViewPoint() - wcP).Norm();
        w *= (rc.farPlane - d) * rc.depthRangeInv;
    }

    if ((weightMask & W_IMG_BORDER) && w > 0.0f) {
        vcg::Point2f cam = rc.shot->toVcgShot().Project(wcP);
        QSize vp = rc.shot->viewportPx();
        w *= 1.0f - std::max(
            std::abs(2.0f * cam.X() / float(vp.width()) - 1.0f),
            std::abs(2.0f * cam.Y() / float(vp.height()) - 1.0f));
    }

    if ((weightMask & W_IMG_ALPHA) && w > 0.0f && !rc.rimg.isNull()) {
        float alpha[3];
        for (int i = 0; i < 3; ++i) {
            vcg::Point3f vp = f.cP(i);
            vcg::Point3f wvP(transformPoint(tx, vp).x(),
                             transformPoint(tx, vp).y(),
                             transformPoint(tx, vp).z());
            vcg::Point2f pp = rc.shot->toVcgShot().Project(wvP);
            if (pp.X() < 0 || pp.Y() < 0 ||
                pp.X() >= float(rc.iw) || pp.Y() >= float(rc.ih))
                alpha[i] = 0;
            else {
                int py = rc.ih - 1 - int(pp.Y());
                if (py < 0 || py >= rc.ih) alpha[i] = 0;
                else alpha[i] = qAlpha(rc.rimg.pixel(int(pp.X()), py));
            }
        }
        float minA = std::min({alpha[0], alpha[1], alpha[2]});
        if (minA == 0) return -1.0f;
        w *= float(minA) / 255.0f;
    }

    return w;
}

// ---------------------------------------------------------------------------
// boundaryOptimization
// ---------------------------------------------------------------------------

void boundaryOptimization(VCGMesh &mesh, std::vector<FaceVisInfo> &faceVis)
{
    vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);

    std::set<VCGFace *> toOptim;
    for (auto &f : mesh.face) {
        vcg::face::Pos<VCGFace> p(&f, f.V(0));
        for (int i = 0; i < 3; ++i) {
            const VCGFace *f2 = p.FFlip();
            if (f2 && !f2->IsV()) {
                int fIdx = vcg::tri::Index(mesh, &f);
                int f2Idx = vcg::tri::Index(mesh, f2);
                if (faceVis[size_t(f2Idx)].ref != faceVis[size_t(fIdx)].ref) {
                    NeighbSet neighb;
                    getNeighbors(p.V(), neighb);
                    getNeighbors(p.VFlip(), neighb);
                    for (auto *n : neighb) toOptim.insert(n);
                }
            }
            p.FlipV(); p.FlipE();
        }
        f.SetV();
    }

    while (!toOptim.empty()) {
        VCGFace *f = *toOptim.begin();
        toOptim.erase(toOptim.begin());

        NeighbSet neighb;
        getFaceNeighbors(f, neighb);

        QMap<int, int> neighRefCount;
        int fIdx = vcg::tri::Index(mesh, f);
        for (auto *n : neighb) {
            if (n && n != f) {
                int nIdx = vcg::tri::Index(mesh, n);
                neighRefCount[faceVis[size_t(nIdx)].ref]++;
            }
        }

        int bestRef = faceVis[size_t(fIdx)].ref;
        int nbMax = 0;
        for (auto it = neighRefCount.begin(); it != neighRefCount.end(); ++it)
            if (it.value() > nbMax && faceVis[size_t(fIdx)].contains(it.key())) {
                nbMax = it.value(); bestRef = it.key();
            }

        if (bestRef != faceVis[size_t(fIdx)].ref) {
            faceVis[size_t(fIdx)].ref = bestRef;
            for (auto *n : neighb)
                if (n && n != f) {
                    int nIdx = vcg::tri::Index(mesh, n);
                    if (faceVis[size_t(nIdx)].ref != bestRef) toOptim.insert(n);
                }
        }
    }
}

// ---------------------------------------------------------------------------
// cleanIsolatedTriangles
// ---------------------------------------------------------------------------

int cleanIsolatedTriangles(VCGMesh &mesh, std::vector<FaceVisInfo> &faceVis)
{
    int changed = 0;
    for (auto &f : mesh.face) {
        QMap<int, int> neigh;
        for (int i = 0; i < 3; ++i) {
            if (f.FFp(i)) {
                int nIdx = vcg::tri::Index(mesh, f.FFp(i));
                neigh[faceVis[size_t(nIdx)].ref]++;
            }
        }
        int fIdx = vcg::tri::Index(mesh, &f);
        if (!neigh.contains(faceVis[size_t(fIdx)].ref)) {
            int bestRef = -1, bestCnt = 0;
            for (auto it = neigh.begin(); it != neigh.end(); ++it)
                if (it.value() > bestCnt) { bestCnt = it.value(); bestRef = it.key(); }
            if (bestRef >= 0) { faceVis[size_t(fIdx)].ref = bestRef; ++changed; }
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// extractPatches
// ---------------------------------------------------------------------------

int extractPatches(RasterPatchMap &patches, PatchVec &nullPatches,
                    VCGMesh &mesh, std::vector<FaceVisInfo> &faceVis,
                    const std::vector<int> &rasterIndices)
{
    for (int ri : rasterIndices) patches[ri] = PatchVec();

    vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
    for (auto &f : mesh.face) f.SetV();

    int nb = 0;
    for (auto &fSeed : mesh.face) {
        if (!fSeed.IsV()) continue;

        std::queue<VCGFace *> q;
        q.push(&fSeed);
        fSeed.ClearV();

        Patch patch;
        patch.refRaster = faceVis[size_t(vcg::tri::Index(mesh, &fSeed))].ref;

        do {
            VCGFace *f = q.front(); q.pop();
            patch.faces.push_back(f);
            for (int i = 0; i < 3; ++i) {
                VCGFace *fa = f->FFp(i);
                if (fa && fa->IsV()) {
                    int faIdx = vcg::tri::Index(mesh, fa);
                    if (faceVis[size_t(faIdx)].ref == patch.refRaster) {
                        fa->ClearV();
                        q.push(fa);
                    }
                }
            }
        } while (!q.empty());

        if (patch.refRaster >= 0) {
            patches[patch.refRaster].push_back(patch);
            ++nb;
        } else {
            nullPatches.push_back(patch);
        }
    }
    return nb;
}

// ---------------------------------------------------------------------------
// constructPatchBoundary
// ---------------------------------------------------------------------------

void constructPatchBoundary(Patch &p, VCGMesh &mesh,
                             const std::vector<FaceVisInfo> &faceVis)
{
    for (auto *f : p.faces) {
        int fRef = faceVis[size_t(vcg::tri::Index(mesh, f))].ref;
        vcg::face::Pos<VCGFace> pos(f, f->V(0));
        for (int i = 0; i < 3; ++i) {
            const VCGFace *f2 = pos.FFlip();
            if (f2) {
                int f2Ref = faceVis[size_t(vcg::tri::Index(mesh, f2))].ref;
                if (f2Ref >= 0 && f2Ref != fRef) {
                    NeighbSet neighb;
                    getNeighbors(pos.V(), neighb);
                    getNeighbors(pos.VFlip(), neighb);
                    for (auto *n : neighb) {
                        if (!n || n->IsV()) continue;
                        int nRef = faceVis[size_t(vcg::tri::Index(mesh, n))].ref;
                        if (nRef != fRef && faceVis[size_t(vcg::tri::Index(mesh, n))].contains(fRef)) {
                            p.boundary.push_back(n);
                            n->SetV();
                        }
                    }
                }
            }
            pos.FlipV(); pos.FlipE();
        }
    }
    for (auto *f : p.boundary) f->ClearV();
}

// ---------------------------------------------------------------------------
// computePatchUV — projects faces onto the reference raster's image plane
// ---------------------------------------------------------------------------

void computePatchUV(VCGMesh &mesh, int ri, PatchVec &patches,
                     const RasterContext &rc, const QMatrix4x4 &transform)
{
    for (auto &p : patches) {
        p.bbox.SetNull();
        p.boundaryUV.clear();
        p.boundaryUV.reserve(p.boundary.size());

        for (auto *f : p.faces)
            for (int i = 0; i < 3; ++i) {
                QVector3D wv = transformPoint(transform, f->cP(i));
                QVector2D pp = rc.shot->project(wv);
                f->WT(i).U() = pp.x();
                f->WT(i).V() = pp.y();
                p.bbox.Add(vcg::Point2f(pp.x(), pp.y()));
            }

        for (size_t n = 0; n < p.boundary.size(); ++n) {
            TriangleUV ftuv;
            for (int i = 0; i < 3; ++i) {
                QVector3D wv = transformPoint(transform, p.boundary[n]->cP(i));
                QVector2D pp = rc.shot->project(wv);
                ftuv.v[i].U() = pp.x();
                ftuv.v[i].V() = pp.y();
                p.bbox.Add(vcg::Point2f(pp.x(), pp.y()));
            }
            p.boundaryUV.push_back(ftuv);
        }
    }
}

// ---------------------------------------------------------------------------
// mergeOverlappingPatches
// ---------------------------------------------------------------------------

void mergeOverlappingPatches(PatchVec &patches)
{
    if (patches.size() <= 1) return;
    for (auto &p : patches) p.valid = true;

    float globalGain = 0.0f;
    for (int i1 = 0; i1 < patches.size(); ++i1) {
        if (!patches[i1].valid) continue;
        float maxGain = -globalGain;
        int candidate = -1;
        for (int i2 = 0; i2 < patches.size(); ++i2) {
            if (i2 == i1 || !patches[i2].valid) continue;
            if (!patches[i1].bbox.Collide(patches[i2].bbox)) continue;
            vcg::Box2f merged = patches[i1].bbox;
            merged.Add(patches[i2].bbox);
            float gain = patches[i1].bbox.Area() + patches[i2].bbox.Area() - merged.Area();
            if (gain > maxGain) { maxGain = gain; candidate = i2; }
        }
        if (candidate >= 0) {
            auto &p1 = patches[i1], &p2 = patches[candidate];
            p1.faces.insert(p1.faces.end(), p2.faces.begin(), p2.faces.end());
            p1.boundary.insert(p1.boundary.end(), p2.boundary.begin(), p2.boundary.end());
            p1.boundaryUV.insert(p1.boundaryUV.end(), p2.boundaryUV.begin(), p2.boundaryUV.end());
            p1.bbox.Add(p2.bbox);
            p2.valid = false;
            globalGain += maxGain;
        }
    }

    patches.erase(std::remove_if(patches.begin(), patches.end(),
        [](const Patch &p) { return !p.valid; }), patches.end());
}

// ---------------------------------------------------------------------------
// Helpers (forward decl needed by patchPacking)
// ---------------------------------------------------------------------------

int computePatchCount(const RasterPatchMap &patches)
{
    int n = 0;
    for (auto i = patches.cbegin(); i != patches.cend(); ++i) n += i.value().size();
    return n;
}

float computeTotalPatchArea(const RasterPatchMap &patches)
{
    float a = 0;
    for (auto i = patches.cbegin(); i != patches.cend(); ++i)
        for (const auto &p : i.value()) a += p.bbox.Area();
    return a;
}

// ---------------------------------------------------------------------------
// patchPacking
// ---------------------------------------------------------------------------

void patchPacking(RasterPatchMap &patches, int textureGutter, bool allowUVStretching,
                    int maxPackingSize, QStringList &log)
{
    std::vector<vcg::Box2f> patchRect;
    patchRect.reserve(size_t(computePatchCount(patches)));

    float totalArea = 0;
    for (auto it = patches.begin(); it != patches.end(); ++it)
        for (auto &p : it.value()) {
            p.bbox.Offset(vcg::Point2f(float(textureGutter), float(textureGutter)));
            patchRect.push_back(p.bbox);
            totalArea += p.bbox.Area();
        }

    if (patchRect.empty()) return;

    float edgeLen = std::sqrt(totalArea);
    if (maxPackingSize > 0 && int(edgeLen) > maxPackingSize)
        edgeLen = float(maxPackingSize);

    log << QStringLiteral("Packing %1 rectangles, grid %2x%2 (area=%.0f, max=%3)")
           .arg(patchRect.size())
           .arg(int(edgeLen)).arg(totalArea, 0, 'f', 0).arg(maxPackingSize);
    std::vector<vcg::Similarity2f> patchTr(patchRect.size());
    vcg::Point2f coveredArea(0, 0);
    vcg::RectPacker<float>::Pack(patchRect,
        vcg::Point2i(int(edgeLen), int(edgeLen)), patchTr, coveredArea);

    float sU, sV;
    if (allowUVStretching) { sU = 1.0f / coveredArea.X(); sV = 1.0f / coveredArea.Y(); }
    else sU = sV = 1.0f / std::max(coveredArea.X(), coveredArea.Y());

    int n = 0;
    for (auto it = patches.begin(); it != patches.end(); ++it)
        for (auto &p : it.value()) {
            const auto &tr = patchTr[size_t(n++)];
            float c = std::cos(tr.rotRad), s = std::sin(tr.rotRad);

            p.img2tex.setToIdentity();
            p.img2tex(0, 0) =  c * tr.sca * sU;
            p.img2tex(0, 1) = -s * tr.sca * sU;
            p.img2tex(0, 3) =  tr.tra.X() * sU;
            p.img2tex(1, 0) =  s * tr.sca * sV;
            p.img2tex(1, 1) =  c * tr.sca * sV;
            p.img2tex(1, 3) =  tr.tra.Y() * sV;

            for (auto *f : p.faces)
                for (int i = 0; i < 3; ++i) {
                    f->WT(i).P() = tr * f->WT(i).P();
                    f->WT(i).U() *= sU;
                    f->WT(i).V() *= sV;
                }
            for (auto &tuv : p.boundaryUV)
                for (int i = 0; i < 3; ++i) {
                    tuv.v[i].P() = tr * tuv.v[i].P();
                    tuv.v[i].U() *= sU;
                    tuv.v[i].V() *= sV;
                }
        }
}

// ---------------------------------------------------------------------------
// CPU texture painting
// ---------------------------------------------------------------------------

QImage paintTexture(const RasterPatchMap &patches, int texSize,
                     const std::vector<RasterContext> &rcs)
{
    QImage tex(texSize, texSize, QImage::Format_ARGB32);
    tex.fill(qRgba(0, 0, 0, 0));

    int writtenPixels = 0;
    for (auto it = patches.begin(); it != patches.end(); ++it) {
        int ri = it.key();
        const QImage &rimg = rcs[size_t(ri)].rimg;
        if (rimg.isNull()) continue;

        for (auto &p : it.value()) {
            // Transform image-space bbox corners to UV [0,1], then to texels
            vcg::Point2f ic[4] = {
                vcg::Point2f(p.bbox.min.X(), p.bbox.min.Y()),
                vcg::Point2f(p.bbox.max.X(), p.bbox.min.Y()),
                vcg::Point2f(p.bbox.min.X(), p.bbox.max.Y()),
                vcg::Point2f(p.bbox.max.X(), p.bbox.max.Y()),
            };
            QVector3D tc[4];
            for (int k = 0; k < 4; ++k)
                tc[k] = p.img2tex.map(QVector3D(ic[k].X(), ic[k].Y(), 0.0f));
            int tMinX = int(std::min({tc[0].x(), tc[1].x(), tc[2].x(), tc[3].x()}) * texSize - 1.0f);
            int tMinY = int(std::min({tc[0].y(), tc[1].y(), tc[2].y(), tc[3].y()}) * texSize - 1.0f);
            int tMaxX = int(std::max({tc[0].x(), tc[1].x(), tc[2].x(), tc[3].x()}) * texSize + 1.0f);
            int tMaxY = int(std::max({tc[0].y(), tc[1].y(), tc[2].y(), tc[3].y()}) * texSize + 1.0f);
            tMinX = std::max(tMinX, 0);
            tMinY = std::max(tMinY, 0);
            tMaxX = std::min(tMaxX, texSize - 1);
            tMaxY = std::min(tMaxY, texSize - 1);

            QMatrix4x4 tex2img = p.img2tex.inverted();
            for (int ty = tMinY; ty <= tMaxY; ++ty)
                for (int tx = tMinX; tx <= tMaxX; ++tx) {
                    float u = (float(tx) + 0.5f) / float(texSize);
                    float v = (float(ty) + 0.5f) / float(texSize);
                    QVector3D imgPt = tex2img.map(QVector3D(u, v, 0.0f));
                    int ix = int(imgPt.x() + 0.5f);
                    int iy = int(imgPt.y() + 0.5f);
                    if (ix < 0 || iy < 0 || ix >= rimg.width() || iy >= rimg.height())
                        continue;
                    QRgb c = rimg.pixel(ix, rimg.height() - 1 - iy);
                    if (qAlpha(c) > 0) {
                        tex.setPixel(tx, texSize - 1 - ty, c);
                        ++writtenPixels;
                    }
                }
        }
    }
    qDebug() << "paintTexture:" << writtenPixels << "pixels written, texture size" << texSize;
    return tex;
}

// ---------------------------------------------------------------------------
// CPU color correction via push-pull difference propagation
// ---------------------------------------------------------------------------

static float clamp01f(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Push: downsample higher-res level into lower-res level using alpha-weighted
// averaging of 2x2 blocks.  Original GPU: avg += color; avg.xyz/avg.w if w >= 0.5
static void pushPullPush(const std::vector<float> &src, int sw, int sh,
                          std::vector<float> &dst, int dw, int dh)
{
    dst.assign(size_t(dw * dh * 4), 0);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            float sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    int sx = x * 2 + dx, sy = y * 2 + dy;
                    if (sx < sw && sy < sh) {
                        size_t i = size_t((sy * sw + sx) * 4);
                        float a = src[i + 3];
                        sumR += src[i]     * a;
                        sumG += src[i + 1] * a;
                        sumB += src[i + 2] * a;
                        sumA += a;
                    }
                }
            size_t oi = size_t((y * dw + x) * 4);
            if (sumA >= 0.5f) {
                dst[oi]     = sumR / sumA;
                dst[oi + 1] = sumG / sumA;
                dst[oi + 2] = sumB / sumA;
                dst[oi + 3] = 1.0f;
            }
        }
}

// Pull: upsample coarser level into finer level.
// Original GPU (GL_LINEAR sampling): creates tmp at finer res,
// tries higher (finer) first, falls back to lower (coarser) with bilinear.
static void pushPullPull(std::vector<float> &finer, int fw, int fh,
                          const std::vector<float> &coarser, int cw, int ch)
{
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            size_t fi = size_t((y * fw + x) * 4);
            if (finer[fi + 3] >= 0.5f) continue;
            float cx = (float(x) + 0.5f) / float(fw) * float(cw) - 0.5f;
            float cy = (float(y) + 0.5f) / float(fh) * float(ch) - 0.5f;
            int cx0 = int(std::floor(cx));
            int cy0 = int(std::floor(cy));
            int cx1 = std::min(cx0 + 1, cw - 1);
            int cy1 = std::min(cy0 + 1, ch - 1);
            float tx = cx - float(cx0), ux = 1.0f - tx;
            float ty = cy - float(cy0), uy = 1.0f - ty;

            float sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            auto sample = [&](int sx, int sy, float wt) {
                if (sx < 0 || sy < 0 || sx >= cw || sy >= ch) return;
                size_t si = size_t((sy * cw + sx) * 4);
                float a = coarser[si + 3];
                if (a < 0.5f) return;
                sumR += coarser[si]     * a * wt;
                sumG += coarser[si + 1] * a * wt;
                sumB += coarser[si + 2] * a * wt;
                sumA += a * wt;
            };
            sample(cx0, cy0, ux * uy);
            sample(cx1, cy0, tx * uy);
            sample(cx0, cy1, ux * ty);
            sample(cx1, cy1, tx * ty);

            if (sumA >= 0.5f) {
                finer[fi]     = sumR / sumA;
                finer[fi + 1] = sumG / sumA;
                finer[fi + 2] = sumB / sumA;
                finer[fi + 3] = 1.0f;
            }
        }
}

QImage rectifyColorCPU(const RasterPatchMap &patches, const QImage &paintedTex,
                        int texSize, int filterSize)
{
    int totalPixels = texSize * texSize;
    std::vector<float> diff(size_t(totalPixels * 4), 0);

    // --- Init: compute boundary color differences (matches GPU pushPullInit) ---
    // For each boundary triangle vertex, sample the painted texture at the
    // CURRENT patch's UV (boundaryUV) and the ADJACENT patch's UV (bf->WT).
    // GPU: gl_TexCoord[0]=gl_Vertex=boundaryUV, gl_TexCoord[1]=gl_MultiTexCoord0=bf->WT
    // Diff = 0.5 * (adjacent_color - current_color), stored at boundaryUV position.

    int writtenPoints = 0;
    for (auto it = patches.begin(); it != patches.end(); ++it) {
        for (auto &p : it.value()) {
            for (size_t n = 0; n < p.boundary.size(); ++n) {
                VCGFace *bf = p.boundary[n];
                for (int i = 0; i < 3; ++i) {
                    // CURRENT side position (= boundaryUV)
                    int txC = int(p.boundaryUV[n].v[i].U() * float(texSize));
                    int tyC = int(p.boundaryUV[n].v[i].V() * float(texSize));
                    // ADJACENT side position (= bf->WT)
                    int txA = int(bf->WT(i).U() * float(texSize));
                    int tyA = int(bf->WT(i).V() * float(texSize));

                    // α-weighted neighborhood averages for each side
                    float sumR0 = 0, sumG0 = 0, sumB0 = 0, sumA0 = 0; // current
                    float sumR1 = 0, sumG1 = 0, sumB1 = 0, sumA1 = 0; // adjacent

                    for (int dy = -filterSize; dy <= filterSize; ++dy)
                        for (int dx = -filterSize; dx <= filterSize; ++dx) {
                            int pxC = txC + dx, pyVcgC = tyC + dy;
                            int pyQC = texSize - 1 - pyVcgC;
                            if (pxC >= 0 && pyQC >= 0 && pxC < texSize && pyQC < texSize) {
                                QRgb c = paintedTex.pixel(pxC, pyQC);
                                float a = qAlpha(c) / 255.0f;
                                if (a > 0) {
                                    sumR0 += (qRed(c)   / 255.0f) * a;
                                    sumG0 += (qGreen(c) / 255.0f) * a;
                                    sumB0 += (qBlue(c)  / 255.0f) * a;
                                    sumA0 += a;
                                }
                            }
                            int pxA = txA + dx, pyVcgA = tyA + dy;
                            int pyQA = texSize - 1 - pyVcgA;
                            if (pxA >= 0 && pyQA >= 0 && pxA < texSize && pyQA < texSize) {
                                QRgb c = paintedTex.pixel(pxA, pyQA);
                                float a = qAlpha(c) / 255.0f;
                                if (a > 0) {
                                    sumR1 += (qRed(c)   / 255.0f) * a;
                                    sumG1 += (qGreen(c) / 255.0f) * a;
                                    sumB1 += (qBlue(c)  / 255.0f) * a;
                                    sumA1 += a;
                                }
                            }
                        }

                    if (sumA0 <= 0.1f || sumA1 <= 0.1f) continue;

                    float avgR0 = sumR0 / sumA0, avgG0 = sumG0 / sumA0, avgB0 = sumB0 / sumA0;
                    float avgR1 = sumR1 / sumA1, avgG1 = sumG1 / sumA1, avgB1 = sumB1 / sumA1;

                    // Diff = 0.5 * (adjacent - current), stored at CURRENT position
                    float dR = 0.5f * (avgR1 - avgR0);
                    float dG = 0.5f * (avgG1 - avgG0);
                    float dB = 0.5f * (avgB1 - avgB0);

                    if (txC >= 0 && tyC >= 0 && txC < texSize && tyC < texSize) {
                        int qyC = texSize - 1 - tyC;
                        size_t di = size_t((qyC * texSize + txC) * 4);
                        diff[di]     = dR;
                        diff[di + 1] = dG;
                        diff[di + 2] = dB;
                        diff[di + 3] = 1.0f;
                        ++writtenPoints;
                    }
                }
            }
        }
    }
    qDebug() << "rectifyColor init:" << writtenPoints << "boundary diff points";

    // --- Build mipmap pyramid (push) ---
    std::vector<std::pair<int, int>> sizes;
    std::vector<std::vector<float>> levels;
    levels.push_back(diff);
    sizes.push_back({texSize, texSize});
    while (sizes.back().first > 1 || sizes.back().second > 1) {
        int nw = sizes.back().first  / 2 + (sizes.back().first  & 1);
        int nh = sizes.back().second / 2 + (sizes.back().second & 1);
        sizes.push_back({nw, nh});
        std::vector<float> nd;
        pushPullPush(levels.back(), sizes[sizes.size() - 2].first,
                     sizes[sizes.size() - 2].second, nd, nw, nh);
        levels.push_back(std::move(nd));
    }

    // --- Pull back up with bilinear interpolation ---
    for (int i = (int)levels.size() - 1; i > 0; --i)
        pushPullPull(levels[size_t(i - 1)],
                     sizes[size_t(i - 1)].first, sizes[size_t(i - 1)].second,
                     levels[size_t(i)],
                     sizes[size_t(i)].first, sizes[size_t(i)].second);

    // --- Apply corrections ---
    QImage out = paintedTex.copy();
    for (int y = 0; y < texSize; ++y)
        for (int x = 0; x < texSize; ++x) {
            size_t di = size_t((y * texSize + x) * 4);
            if (levels[0][di + 3] < 0.5f) continue;
            QRgb c = paintedTex.pixel(x, y);
            int nr = int(clamp01f(qRed(c)   / 255.0f + levels[0][di])     * 255);
            int ng = int(clamp01f(qGreen(c) / 255.0f + levels[0][di + 1]) * 255);
            int nb = int(clamp01f(qBlue(c)  / 255.0f + levels[0][di + 2]) * 255);
            out.setPixel(x, y, qRgba(nr, ng, nb, qAlpha(c)));
        }
    return out;
}

// ---------------------------------------------------------------------------
// Main parameterization pipeline
// ---------------------------------------------------------------------------

QStringList patchBasedTextureParameterization(
    RasterPatchMap &patches, PatchVec &nullPatches,
    VCGMesh &mesh, const QMatrix4x4 &transform,
    const std::vector<int> &rasterIndices,
    std::vector<RasterContext> &rcs,
    int weightMask, bool cleanIsolated, int textureGutter, bool stretchUV,
    float depthEpsilon, int maxPackingSize)
{
    QStringList log;

    // Compute per-face visibility and weights
    QElapsedTimer t, tVis;
    t.start();
    std::vector<FaceVisInfo> faceVis(mesh.FN());

    double accumVertTestMs = 0;
    for (int ri : rasterIndices) {
        auto &rc = rcs[size_t(ri)];

        // Pre-compute per-vertex visibility for this raster
        tVis.start();
        std::vector<bool> vVis(size_t(mesh.VN()), false);
        for (int vi = 0; vi < mesh.VN(); ++vi)
            if (!mesh.vert[size_t(vi)].IsD())
                vVis[size_t(vi)] = checkVertVisibleFast(vi, rc, depthEpsilon);
        accumVertTestMs += 0.001 * tVis.elapsed();

        for (int fi = 0; fi < mesh.FN(); ++fi) {
            VCGFace &f = mesh.face[size_t(fi)];
            if (f.IsD()) continue;
            if (vVis[vcg::tri::Index(mesh, f.V(0))] ||
                vVis[vcg::tri::Index(mesh, f.V(1))] ||
                vVis[vcg::tri::Index(mesh, f.V(2))]) {
                float w = faceWeight(mesh, f, transform, ri, rc, weightMask);
                if (w >= 0.0f)
                    faceVis[size_t(fi)].add(w, ri);
            }
        }
    }
    log << QStringLiteral("VISIBILITY CHECK: %1 sec. (vert depth tests: %2 sec.)")
           .arg(0.001f * t.elapsed(), 0, 'f', 3)
           .arg(accumVertTestMs, 0, 'f', 3);

    // Boundary optimization
    t.start();
    boundaryOptimization(mesh, faceVis);
    log << QStringLiteral("BOUNDARY OPTIMIZATION: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);

    // Clean isolated triangles
    if (cleanIsolated) {
        t.start();
        int triCleaned = cleanIsolatedTriangles(mesh, faceVis);
        log << QStringLiteral("CLEANING ISOLATED TRIANGLES: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);
        log << QStringLiteral("  * %1 triangles cleaned.").arg(triCleaned);
    }

    // Extract patches
    t.start();
    int nbPatches = extractPatches(patches, nullPatches, mesh, faceVis, rasterIndices);
    log << QStringLiteral("PATCH EXTRACTION: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);
    log << QStringLiteral("  * %1 patches extracted, %2 null patches.")
           .arg(nbPatches).arg(nullPatches.size());

    // Extend patch boundaries
    t.start();
    for (auto it = patches.begin(); it != patches.end(); ++it)
        for (auto &p : it.value())
            constructPatchBoundary(p, mesh, faceVis);
    log << QStringLiteral("PATCH EXTENSION: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);

    // Compute patch UVs
    t.start();
    for (auto it = patches.begin(); it != patches.end(); ++it)
        computePatchUV(mesh, it.key(), it.value(), rcs[size_t(it.key())], transform);
    log << QStringLiteral("PATCHES UV COMPUTATION: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);

    // Merge overlapping patches
    t.start();
    float oldArea = computeTotalPatchArea(patches);
    for (auto it = patches.begin(); it != patches.end(); ++it)
        mergeOverlappingPatches(it.value());
    log << QStringLiteral("PATCH MERGING: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);
    log << QStringLiteral("  * Area reduction: %1%.")
           .arg(100.0f * computeTotalPatchArea(patches) / oldArea, 0, 'f', 1);
    log << QStringLiteral("  * Patches number reduced from %1 to %2.")
           .arg(nbPatches).arg(computePatchCount(patches));

    // Pack patches
    t.start();
    patchPacking(patches, textureGutter, stretchUV, maxPackingSize, log);
    log << QStringLiteral("PATCH TEXTURE PACKING: %1 sec.").arg(0.001f * t.elapsed(), 0, 'f', 3);

    // Clear null patch UVs
    for (auto &p : nullPatches)
        for (auto *f : p.faces)
            for (int i = 0; i < 3; ++i)
                f->WT(i).P() = vcg::Point2f(0.0f, 0.0f);

    for (auto &f : mesh.face)
        for (int i = 0; i < 3; ++i)
            f.WT(i).N() = 0;

    return log;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

QString ImgPatchParamFilterPlugin::pluginId() const
{ return QStringLiteral("meshlab2.filter.img_patch_param"); }

QString ImgPatchParamFilterPlugin::name() const
{ return QStringLiteral("Image Patch Parametrization Filters"); }

MeshFilterRunResult ImgPatchParamFilterPlugin::runFilter(
    const QString &fid, const FilterParams &p, Document &doc) const
{
    using namespace vcg::tri::io;

    int mi = doc.currentMeshIndex();
    if (mi < 0 || mi >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    VCGMesh &m = doc.mesh(mi).mesh;
    QMatrix4x4 tf = doc.mesh(mi).transform;

    // Collect active rasters with valid camera and image
    std::vector<int> activeRasters;
    int maxRi = -1;
    for (int ri = 0; ri < doc.rasterCount(); ++ri) {
        auto &re = doc.raster(ri);
        if (re.visible && re.shot.isValid() && !re.planes.empty()) {
            RasterPlane *rp = re.currentPlane();
            if (rp) {
                Document::ensureRasterPlaneImage(*rp);
                if (rp->hasImage()) {
                    activeRasters.push_back(ri);
                    if (ri > maxRi) maxRi = ri;
                }
            }
        }
    }
    if (activeRasters.empty())
        return fail(QObject::tr("No active rasters with valid cameras and images."));

    // Build raster contexts (including depth buffers)
    QElapsedTimer tDepth;
    tDepth.start();
    std::vector<RasterContext> rcs(size_t(maxRi + 1));
    for (int ri : activeRasters) {
        auto &re = doc.raster(ri);
        RasterContext &rc = rcs[size_t(ri)];
        rc.shot = &re.shot;
        rc.iw = re.shot.viewportPx().width();
        rc.ih = re.shot.viewportPx().height();
        rc.cz = re.shot.referenceAxis(2);
        rc.rimg = re.currentPlane()->image;

        QElapsedTimer tOne;
        tOne.start();
        rc.dbuf = buildDepthBuffer(re.shot, m, tf, &rc.vcache);
        qint64 oneMs = tOne.elapsed();

        float zN = std::numeric_limits<float>::max();
        float zF = -std::numeric_limits<float>::max();
        for (const auto &v : m.vert) {
            if (v.IsD()) continue;
            QVector3D wv = transformPoint(tf, v.cP());
            float d = re.shot.depth(wv);
            if (d > 0) { if (d < zN) zN = d; if (d > zF) zF = d; }
        }
        if (zN < 0.0001f) zN = 0.1f;
        if (zF < zN) zF = zN + 1000.0f;
        rc.nearPlane = zN;
        rc.farPlane  = zF;
        rc.depthRangeInv = 1.0f / (zF - zN);

        doc.writeLog(QObject::tr("Depth buffer %1/%2 (%3x%4): %5 ms")
            .arg(ri + 1).arg(doc.rasterCount())
            .arg(rc.iw).arg(rc.ih)
            .arg(oneMs),
            Document::LogSource::Application, Document::LogLevel::Debug);
    }
    qint64 totalDepthMs = tDepth.elapsed();
    doc.writeLog(QObject::tr("All depth buffers: %1 ms").arg(totalDepthMs),
                 Document::LogSource::Application, Document::LogLevel::Debug);

    // --- Coverage (Vertex) ---
    if (fid == QString::fromLatin1(kCoverageVert)) {
        float depthEps = float(p.getDouble(QStringLiteral("depthEpsilon"), 0.0));
        for (auto &v : m.vert) v.Q() = 0.0f;

        for (int ri : activeRasters) {
            auto &rc = rcs[size_t(ri)];
            for (int vi = 0; vi < m.VN(); ++vi) {
                if (m.vert[size_t(vi)].IsD()) continue;
                if (checkVertVisibleFast(vi, rc, depthEps))
                    m.vert[size_t(vi)].Q() += 1.0f;
            }
        }

        if (p.getBool(QStringLiteral("normalizeQuality"), false)) {
            float nf = 1.0f / float(activeRasters.size());
            for (auto &v : m.vert) v.Q() *= nf;
        }

        doc.mesh(mi).ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(mi,
            QObject::tr("Computed raster coverage per vertex on '%1'.").arg(doc.mesh(mi).name));
        return ok({QObject::tr("Processed %1 vertices across %2 rasters.")
                     .arg(m.VN()).arg(activeRasters.size())});
    }

    // --- Coverage (Face) ---
    if (fid == QString::fromLatin1(kCoverageFace)) {
        float depthEps = float(p.getDouble(QStringLiteral("depthEpsilon"), 0.0));
        for (auto &f : m.face) f.Q() = 0.0f;

        for (int ri : activeRasters) {
            auto &rc = rcs[size_t(ri)];
            std::vector<bool> vVis(size_t(m.VN()), false);
            for (int vi = 0; vi < m.VN(); ++vi) {
                if (m.vert[size_t(vi)].IsD()) continue;
                vVis[size_t(vi)] = checkVertVisibleFast(vi, rc, depthEps);
            }
            for (int fi = 0; fi < m.FN(); ++fi) {
                auto &f = m.face[size_t(fi)];
                if (f.IsD()) continue;
                if (vVis[vcg::tri::Index(m, f.V(0))] ||
                    vVis[vcg::tri::Index(m, f.V(1))] ||
                    vVis[vcg::tri::Index(m, f.V(2))])
                    f.Q() += 1.0f;
            }
        }

        if (p.getBool(QStringLiteral("normalizeQuality"), false)) {
            float nf = 1.0f / float(activeRasters.size());
            for (auto &f : m.face) f.Q() *= nf;
        }

        doc.mesh(mi).ioMask |= Mask::IOM_FACEQUALITY;
        doc.markMeshGeometryChanged(mi,
            QObject::tr("Computed raster coverage per face on '%1'.").arg(doc.mesh(mi).name));
        return ok({QObject::tr("Processed %1 faces across %2 rasters.")
                     .arg(m.FN()).arg(activeRasters.size())});
    }

    // --- Parametrization filters ---
    if (fid == QString::fromLatin1(kPatchParamOnly) ||
        fid == QString::fromLatin1(kPatchParamTex)) {

        // Ensure manifoldness and required topology
        VCGMeshFFAdjScope ffAdj(m);
        VCGMeshVFAdjScope vfAdj(m);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
        vcg::tri::UpdateTopology<VCGMesh>::VertexFace(m);

        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(m) > 0)
            return fail(QObject::tr("Mesh has non 2-manifold faces, this filter requires manifoldness."));

        vcg::tri::Allocator<VCGMesh>::CompactFaceVector(m);
        vcg::tri::Allocator<VCGMesh>::CompactVertexVector(m);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
        vcg::tri::UpdateTopology<VCGMesh>::VertexFace(m);

        // Rebuild raster contexts after compaction (depth buffers invalidated)
        QElapsedTimer tDbRebuild;
        tDbRebuild.start();
        for (int ri : activeRasters) {
            rcs[size_t(ri)].dbuf = buildDepthBuffer(doc.raster(ri).shot, m, tf, &rcs[size_t(ri)].vcache);
        }
        doc.writeLog(QObject::tr("Depth buffers rebuilt after compaction: %1 ms")
            .arg(tDbRebuild.elapsed()),
            Document::LogSource::Application, Document::LogLevel::Debug);

        int wMask = W_ORIENTATION;
        if (p.getBool(QStringLiteral("useDistanceWeight"), true))  wMask |= W_DISTANCE;
        if (p.getBool(QStringLiteral("useImgBorderWeight"), true)) wMask |= W_IMG_BORDER;
        if (p.getBool(QStringLiteral("useAlphaWeight"), false))    wMask |= W_IMG_ALPHA;

        bool cleanIso = p.getBool(QStringLiteral("cleanIsolatedTriangles"), true);
        bool stretch  = p.getBool(QStringLiteral("stretchingAllowed"), false);
        int  gutter   = p.getInt(QStringLiteral("textureGutter"), 4);
        float depthEps  = float(p.getDouble(QStringLiteral("depthEpsilon"), 0.0));
        int maxPackSize = p.getInt(QStringLiteral("maxPackingSize"), 0);

        RasterPatchMap patches;
        PatchVec nullPatches;
        QStringList sLog = patchBasedTextureParameterization(
            patches, nullPatches, m, tf, activeRasters, rcs,
            wMask, cleanIso, gutter, stretch, depthEps, maxPackSize);

        doc.mesh(mi).ioMask |= Mask::IOM_WEDGTEXCOORD;
        doc.markMeshGeometryChanged(mi,
            QObject::tr("Parameterized '%1' from registered rasters.").arg(doc.mesh(mi).name));

        if (fid == QString::fromLatin1(kPatchParamOnly))
            return ok(sLog);

        // --- Texturing ---
        int     texSize  = p.getInt(QStringLiteral("textureSize"), 1024);
        QString texName  = p.getFileSave(QStringLiteral("textureName"),
                                         QStringLiteral("texture.png")).trimmed();
        bool    doCorr   = p.getBool(QStringLiteral("colorCorrection"), true);
        int     corrSize = p.getInt(QStringLiteral("colorCorrectionFilterSize"), 1);

        QElapsedTimer pt;
        pt.start();
        QImage tex = paintTexture(patches, texSize, rcs);
        if (doCorr)
            tex = rectifyColorCPU(patches, tex, texSize, corrSize);
        sLog << QStringLiteral("TEXTURE PAINTING: %1 sec.").arg(0.001f * pt.elapsed(), 0, 'f', 3);

        QString saveError;
        if (!TextureAssociationUtils::saveImages({texName}, {tex}, saveError))
            return fail(saveError);
        auto oa = TextureAssociationUtils::makeTextureAssetsFromSavedImages({texName}, {tex});
        TextureAssociationUtils::replaceTextureAssociations(doc.mesh(mi), oa);
        doc.mesh(mi).ioMask |= Mask::IOM_WEDGTEXCOORD;
        doc.markMeshMaterialChanged(mi,
            QObject::tr("Textured '%1' from registered rasters.").arg(doc.mesh(mi).name));
        sLog << QObject::tr("Saved: %1").arg(texName);
        return ok(sLog);
    }

    return fail(QObject::tr("Unknown filter: %1").arg(fid));
}

void registerImgPatchParamFilterPlugin(MeshFilterPluginManager &pm)
{
    pm.registerPlugin(std::make_unique<ImgPatchParamFilterPlugin>());
}

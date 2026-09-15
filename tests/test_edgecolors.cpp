#include <QtTest/QtTest>
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include "document.h"
#include "document_internal.h"
#include "meshgpuresourcecache.h"
#include <wrap/io_trimesh/import_ply.h>

using Mask = vcg::tri::io::Mask;
using Importer = vcg::tri::io::ImporterPLY<VCGMesh>;

namespace {
const QString kFixture = QStringLiteral(TEST_SOURCE_DIR "/tests/data/edge_colors.ply");
const vcg::Color4b kYellow(255, 255, 0, 255);
const vcg::Color4b kCyan(0, 255, 255, 255);

QByteArray fixture()
{
    QFile file(kFixture);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

void checkExample(const VCGMesh &mesh)
{
    QCOMPARE(mesh.VN(), 3);
    QCOMPARE(mesh.EN(), 2);
    QCOMPARE(mesh.FN(), 0);
    QCOMPARE(mesh.vert[0].cC(), vcg::Color4b(255, 0, 0, 255));
    QCOMPARE(mesh.vert[1].cC(), vcg::Color4b(0, 255, 0, 255));
    QCOMPARE(mesh.vert[2].cC(), vcg::Color4b(0, 0, 255, 255));
    QCOMPARE(mesh.edge[0].cC(), kYellow);
    QCOMPARE(mesh.edge[1].cC(), kCyan);
    QCOMPARE(mesh.edge[0].cV(0), &mesh.vert[0]);
    QCOMPARE(mesh.edge[0].cV(1), &mesh.vert[1]);
    QCOMPARE(mesh.edge[1].cV(0), &mesh.vert[0]);
    QCOMPARE(mesh.edge[1].cV(1), &mesh.vert[2]);
}
}

class EdgeColorTests : public QObject
{
    Q_OBJECT
private slots:
    void importsExampleAndDetectsMasks();
    void optionalProperties_data();
    void optionalProperties();
    void binaryImport_data();
    void binaryImport();
    void invalidInput_data();
    void invalidInput();
    void roundTrip_data();
    void roundTrip();
    void copiesAndUndoPreserveColors();
    void deletedEdgesSurviveCopyAndCompaction();
    void gpuCacheTracksVariantsAndColorChanges();
};

void EdgeColorTests::importsExampleAndDetectsMasks()
{
    int mask = 0;
    QVERIFY(Importer::LoadMask(qPrintable(kFixture), mask));
    QVERIFY(mask & Mask::IOM_VERTCOLOR);
    QVERIFY(mask & Mask::IOM_EDGEINDEX);
    QVERIFY(mask & Mask::IOM_EDGECOLOR);
    Document doc;
    QCOMPARE(doc.loadMesh(kFixture), 0);
    QVERIFY(doc.mesh(0).ioMask & Mask::IOM_EDGECOLOR);
    checkExample(doc.mesh(0).mesh);
    const int capability = doc.saveMaskCapability(QStringLiteral("output.ply"));
    QVERIFY(capability & Mask::IOM_EDGEINDEX);
    QVERIFY(capability & Mask::IOM_EDGECOLOR);
}

void EdgeColorTests::optionalProperties_data()
{
    QTest::addColumn<QByteArray>("properties");
    QTest::addColumn<QByteArray>("values");
    QTest::addColumn<bool>("colored");
    QTest::addColumn<int>("alpha");
    QTest::newRow("rgb") << QByteArray("property uchar red\nproperty uchar green\nproperty uchar blue\n")
                        << QByteArray("255 255 0") << true << 255;
    QTest::newRow("rgba-reordered") << QByteArray("property uchar alpha\nproperty uchar blue\nproperty uchar red\nproperty uchar green\n")
                                   << QByteArray("72 0 255 255") << true << 72;
    QTest::newRow("absent") << QByteArray() << QByteArray() << false << 255;
    QTest::newRow("partial") << QByteArray("property uchar red\nproperty uchar blue\n")
                            << QByteArray("255 0") << false << 255;
    QTest::newRow("alpha-only") << QByteArray("property uchar alpha\n") << QByteArray("72") << false << 255;
}

void EdgeColorTests::optionalProperties()
{
    QFETCH(QByteArray, properties);
    QFETCH(QByteArray, values);
    QFETCH(bool, colored);
    QFETCH(int, alpha);
    const QByteArray data = "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\n"
        "element edge 1\nproperty int vertex1\nproperty int vertex2\n" + properties
        + "end_header\n0 0 0\n1 0 0\n0 1 " + values + "\n";
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("optional.ply"));
    QVERIFY(writeFile(path, data));
    int mask = 0;
    QVERIFY(Importer::LoadMask(qPrintable(path), mask));
    QCOMPARE(bool(mask & Mask::IOM_EDGECOLOR), colored);
    Document doc;
    QCOMPARE(doc.loadMesh(path), 0);
    QCOMPARE(bool(doc.mesh(0).ioMask & Mask::IOM_EDGECOLOR), colored);
    QVERIFY(!(doc.mesh(0).ioMask & Mask::IOM_VERTCOLOR));
    QCOMPARE(doc.mesh(0).mesh.EN(), 1);
    if (colored)
        QCOMPARE(doc.mesh(0).mesh.edge[0].cC(), vcg::Color4b(255, 255, 0, alpha));
}

void EdgeColorTests::binaryImport_data()
{
    QTest::addColumn<bool>("littleEndian");
    QTest::newRow("little-endian") << true;
    QTest::newRow("big-endian") << false;
}

void EdgeColorTests::binaryImport()
{
    QFETCH(bool, littleEndian);
    QByteArray data = fixture();
    const int end = data.indexOf("end_header\n") + int(sizeof("end_header\n") - 1);
    data.truncate(end);
    data.replace("ascii", littleEndian ? "binary_little_endian" : "binary_big_endian");
    QDataStream stream(&data, QIODevice::Append);
    stream.setByteOrder(littleEndian ? QDataStream::LittleEndian : QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << float(0) << float(0) << float(0) << quint8(255) << quint8(0) << quint8(0);
    stream << float(1) << float(0) << float(0) << quint8(0) << quint8(255) << quint8(0);
    stream << float(0) << float(1) << float(0) << quint8(0) << quint8(0) << quint8(255);
    stream << qint32(0) << qint32(1) << quint8(255) << quint8(255) << quint8(0);
    stream << qint32(0) << qint32(2) << quint8(0) << quint8(255) << quint8(255);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("binary.ply"));
    QVERIFY(writeFile(path, data));
    Document doc;
    QCOMPARE(doc.loadMesh(path), 0);
    checkExample(doc.mesh(0).mesh);
}

void EdgeColorTests::invalidInput_data()
{
    QTest::addColumn<QByteArray>("data");
    QByteArray invalid = fixture();
    invalid.replace("0 2 0 255 255", "0 3 0 255 255");
    QTest::newRow("invalid-index") << invalid;
    invalid = fixture();
    invalid.replace("0 2 0 255 255", "-1 2 0 255 255");
    QTest::newRow("negative-index") << invalid;
    invalid = fixture();
    invalid.truncate(invalid.lastIndexOf("0 2 0 255 255"));
    QTest::newRow("truncated") << invalid;
}

void EdgeColorTests::invalidInput()
{
    QFETCH(QByteArray, data);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("bad.ply"));
    QVERIFY(writeFile(path, data));
    Document doc;
    QVERIFY(doc.loadMesh(path) != 0);
    QCOMPARE(doc.meshCount(), 0);
}

void EdgeColorTests::roundTrip_data()
{
    QTest::addColumn<bool>("binary");
    QTest::addColumn<bool>("saveColors");
    QTest::newRow("ascii-color") << false << true;
    QTest::newRow("binary-color") << true << true;
    QTest::newRow("ascii-no-color") << false << false;
    QTest::newRow("binary-no-color") << true << false;
}

void EdgeColorTests::roundTrip()
{
    QFETCH(bool, binary);
    QFETCH(bool, saveColors);
    Document doc;
    QCOMPARE(doc.loadMesh(kFixture), 0);
    doc.mesh(0).mesh.edge[1].C()[3] = 72;
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("saved.ply"));
    MeshIOSaveOptions options;
    options.binary = binary;
    // Geometry must be saved even if the caller only selects optional colors.
    options.mask = Mask::IOM_VERTCOLOR | (saveColors ? Mask::IOM_EDGECOLOR : 0);
    QCOMPARE(doc.saveMesh(0, path, options), 0);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray data = file.readAll();
    const QByteArray header = data.left(data.indexOf("end_header"));
    const QByteArray edgeHeader = header.mid(header.indexOf("element edge"));
    QCOMPARE(edgeHeader.contains("property uchar red"), saveColors);
    QCOMPARE(edgeHeader.contains("property uchar alpha"), saveColors);
    Document loaded;
    QCOMPARE(loaded.loadMesh(path), 0);
    const auto &entry = loaded.mesh(0);
    QCOMPARE(entry.mesh.EN(), 2);
    QCOMPARE(entry.mesh.edge[1].cV(1), &entry.mesh.vert[2]);
    QCOMPARE(entry.mesh.vert[1].cC(), vcg::Color4b(0, 255, 0, 255));
    QCOMPARE(bool(entry.ioMask & Mask::IOM_EDGECOLOR), saveColors);
    if (saveColors) {
        QCOMPARE(entry.mesh.edge[0].cC(), kYellow);
        QCOMPARE(entry.mesh.edge[1].cC(), vcg::Color4b(0, 255, 255, 72));
    }
}

void EdgeColorTests::copiesAndUndoPreserveColors()
{
    Document doc;
    QCOMPARE(doc.loadMesh(kFixture), 0);
    QCOMPARE(doc.duplicateMesh(0), 1);
    checkExample(doc.mesh(1).mesh);
    QVERIFY(doc.mesh(1).ioMask & Mask::IOM_EDGECOLOR);
    QVERIFY(doc.undo());
    QCOMPARE(doc.meshCount(), 1);
    checkExample(doc.mesh(0).mesh);
    QVERIFY(doc.redo());
    QCOMPARE(doc.meshCount(), 2);
    checkExample(doc.mesh(1).mesh);
}

void EdgeColorTests::deletedEdgesSurviveCopyAndCompaction()
{
    VCGMesh mesh;
    int mask = 0;
    QCOMPARE(Importer::Open(mesh, qPrintable(kFixture), mask), 0);
    vcg::tri::Allocator<VCGMesh>::DeleteEdge(mesh, mesh.edge[0]);
    VCGMesh copy;
    DocumentInternal::deepCopyMesh(mesh, copy);
    QCOMPARE(copy.EN(), 1);
    QCOMPARE(copy.edge.size(), size_t(1));
    QCOMPARE(copy.edge[0].cC(), kCyan);
    QCOMPARE(copy.edge[0].cV(1), &copy.vert[2]);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    QCOMPARE(mesh.EN(), 1);
    QCOMPARE(mesh.edge[0].cC(), kCyan);
    QCOMPARE(mesh.edge[0].cV(1), &mesh.vert[2]);
}

void EdgeColorTests::gpuCacheTracksVariantsAndColorChanges()
{
    VCGMesh mesh;
    int mask = 0;
    QCOMPARE(Importer::Open(mesh, qPrintable(kFixture), mask), 0);
    QRhiNullInitParams params;
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
    QVERIFY(rhi);
    MeshGpuResourceCache cache;
    MeshGpuResourceCache::MeshSource source;
    source.meshId = 1;
    source.mesh = &mesh;
    source.ioMask = mask;
    using Variant = MeshGpuResourceCache::EdgeVariant;
    const auto ensure = [&](Variant variant) {
        QRhiCommandBuffer *cb = nullptr;
        rhi->beginOffscreenFrame(&cb);
        const auto result = cache.ensureMeshResources(rhi.get(), cb, source,
            MeshGpuResourceCache::FillVariant::Constant, MeshGpuResourceCache::PointVariant::Constant,
            false, false, true, false, false, false, false, false, variant);
        rhi->endOffscreenFrame();
        return result;
    };
    QVERIFY(ensure(Variant::PerEdge).rebuiltEdges);
    const auto lines = cache.edgePassView(rhi.get(), 1, Variant::PerEdge);
    const auto fat = cache.edgeFatPassView(rhi.get(), 1, Variant::PerEdge);
    QCOMPARE(lines.vertexCount, 4);
    QCOMPARE(fat.vertexCount, 12);
    QCOMPARE(lines.vertexBuffer->size(), 4 * 7 * int(sizeof(float)));
    QCOMPARE(fat.vertexBuffer->size(), 12 * 12 * int(sizeof(float)));
    auto *constantBuffer = cache.edgePassView(rhi.get(), 1).vertexBuffer;
    QVERIFY(constantBuffer);
    QCOMPARE(cache.gpuMemoryStats().front().edgeBufferBytes,
             qint64((4 * 3 + 12 * 8 + 4 * 7 + 12 * 12) * sizeof(float)));
    QVERIFY(!ensure(Variant::PerEdge).rebuiltEdges);
    QVERIFY(!ensure(Variant::Constant).rebuiltEdges);
    mesh.edge[0].C() = kCyan;
    ++source.materialRevision;
    QVERIFY(ensure(Variant::PerEdge).rebuiltEdges);
    QCOMPARE(cache.edgePassView(rhi.get(), 1).vertexBuffer, constantBuffer);
    source.ioMask &= ~Mask::IOM_EDGECOLOR;
    QVERIFY(ensure(Variant::PerEdge).rebuiltEdges);
    QVERIFY(!cache.edgePassView(rhi.get(), 1, Variant::PerEdge).vertexBuffer);
    source.ioMask |= Mask::IOM_EDGECOLOR;
    QVERIFY(ensure(Variant::PerEdge).rebuiltEdges);
    // A deleted entry before the remaining edge must not truncate iteration at EN().
    vcg::tri::Allocator<VCGMesh>::DeleteEdge(mesh, mesh.edge[0]);
    ++source.geometryRevision;
    QVERIFY(ensure(Variant::PerEdge).rebuiltEdges);
    QCOMPARE(cache.edgePassView(rhi.get(), 1, Variant::PerEdge).vertexCount, 2);
    QCOMPARE(cache.edgeFatPassView(rhi.get(), 1, Variant::PerEdge).vertexCount, 6);
}

QTEST_MAIN(EdgeColorTests)
#include "test_edgecolors.moc"

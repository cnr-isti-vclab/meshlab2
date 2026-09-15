#include <QtTest/QtTest>
#include <QComboBox>
#include <QPushButton>
#include "renderoverlaypanel.h"

class EdgeColorUiTests : public QObject
{
    Q_OBJECT
private slots:
    void colorSourceUpdatesTheConstantColorControl();
    void availabilityAndUvMode();
};

void EdgeColorUiTests::colorSourceUpdatesTheConstantColorControl()
{
    RenderOverlayPanel panel;
    auto *source = panel.findChild<QComboBox *>(QStringLiteral("edgeColorSource"));
    auto *color = panel.findChild<QPushButton *>(QStringLiteral("edgeColor"));
    QVERIFY(source);
    QVERIFY(color);
    PerMeshRenderSettings settings;
    settings.edgeColorSource = EdgeColorSource::PerEdge;
    panel.setMeshSettings(settings);
    panel.setEdgeColorSourceAvailability(true);
    QVERIFY(!color->isEnabled());
    QSignalSpy spy(&panel, &RenderOverlayPanel::meshSettingsChanged);
    source->setCurrentIndex(source->findData(int(EdgeColorSource::Constant)));
    QCOMPARE(panel.meshSettings().edgeColorSource, EdgeColorSource::Constant);
    QVERIFY(color->isEnabled());
    QCOMPARE(spy.count(), 1);
    source->setCurrentIndex(source->findData(int(EdgeColorSource::PerEdge)));
    QCOMPARE(panel.meshSettings().edgeColorSource, EdgeColorSource::PerEdge);
    QVERIFY(!color->isEnabled());
    QCOMPARE(spy.count(), 2);
}

void EdgeColorUiTests::availabilityAndUvMode()
{
    RenderOverlayPanel panel;
    auto *source = panel.findChild<QComboBox *>(QStringLiteral("edgeColorSource"));
    auto *color = panel.findChild<QPushButton *>(QStringLiteral("edgeColor"));
    QVERIFY(source);
    QVERIFY(color);
    const auto index = source->model()->index(source->findData(int(EdgeColorSource::PerEdge)), 0);
    panel.setEdgeColorSourceAvailability(false);
    QVERIFY(!(source->model()->flags(index) & Qt::ItemIsEnabled));
    panel.setEdgeColorSourceAvailability(true);
    QVERIFY(source->model()->flags(index) & Qt::ItemIsEnabled);
    PerMeshRenderSettings settings;
    settings.edgeColorSource = EdgeColorSource::PerEdge;
    panel.setMeshSettings(settings);
    panel.setViewerModeUv(true);
    QVERIFY(!source->isEnabled());
    QVERIFY(color->isEnabled());
    panel.setViewerModeUv(false);
    QVERIFY(source->isEnabled());
    QVERIFY(!color->isEnabled());
    QCOMPARE(panel.meshSettings().edgeColorSource, EdgeColorSource::PerEdge);
}

QTEST_MAIN(EdgeColorUiTests)
#include "test_edgecolor_ui.moc"

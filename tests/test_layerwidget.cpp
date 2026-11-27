#include <QtTest/QtTest>

#include "document.h"
#include "layerwidget.h"
#include "textureassociationutils.h"

#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QApplication>
#include <QContextMenuEvent>
#include <QTimer>
#include <QMenu>
#include <QScrollBar>
#include <QScrollArea>
#include <QTableWidget>
#include <QTreeWidget>
#include <QToolButton>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <wrap/io_trimesh/io_mask.h>

class LayerWidgetTests : public QObject
{
    Q_OBJECT

private slots:
    void visibilitySetChipsAreFullyVisibleAndNormallySized();
    void hoveringASetChipShadesItsLayersRows();
    void theEyeShowsAllOrHidesAllAndSaysWhatIsShown();
    void theTitleBarButtonsGoOverTheArrowsAndTheCheckMarks();
    void theListStaysWhereTheUserScrolledItWhenALayerChanges();
    void theMarkSaysWhetherAFileHoldsTheLayer();
    void typingANameRunsTheRenameFilter();
    void ctrlDDuplicatesTheCurrentLayer();
    void selectedCountsFollowEverySelectionChange();
    void countRowsListOnlyTheKindsALayerHas();
};

namespace {

int addTinyMesh(Document &doc, const QString &name)
{
    VCGMesh m;
    vcg::tri::Allocator<VCGMesh>::AddVertices(m, 3);
    m.vert[0].P() = vcg::Point3f(0, 0, 0);
    m.vert[1].P() = vcg::Point3f(1, 0, 0);
    m.vert[2].P() = vcg::Point3f(0, 1, 0);
    vcg::tri::Allocator<VCGMesh>::AddFace(m, 0, 1, 2);
    vcg::tri::UpdateBounding<VCGMesh>::Box(m);
    return doc.addMesh(m, name, vcg::tri::io::Mask::IOM_VERTCOORD);
}

} // namespace

// The chips under the layer list were first sized from the host widget before any chip was in
// it, which left the row a few pixels high and every chip cut off. They have to be as tall as a
// tool button needs, and wholly inside the row.
void LayerWidgetTests::visibilitySetChipsAreFullyVisibleAndNormallySized()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    doc.addVisibilitySet(QStringLiteral("front"), doc.visibleMeshIds());
    doc.addVisibilitySet(QStringLiteral("a long name that gets cut"), { doc.mesh(0).meshId });

    LayerWidget widget(&doc);
    widget.resize(320, 420);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);   // the chips are made when the queued rebuild runs

    const QString dir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!dir.isEmpty())
        widget.grab().save(dir + QStringLiteral("/layerwidget.png"));

    QList<QToolButton *> chips;
    for (QToolButton *button : widget.findChildren<QToolButton *>()) {
        if (button->isCheckable())
            chips << button;
    }
    QCOMPARE(chips.size(), 2);

    QToolButton reference;
    reference.setText(QStringLiteral("front"));
    reference.setAutoRaise(true);
    for (QToolButton *chip : chips) {
        QVERIFY2(chip->height() >= reference.sizeHint().height() - 2, qPrintable(chip->text()));
        QVERIFY2(chip->height() <= reference.sizeHint().height() + 12, qPrintable(chip->text()));
        const QWidget *viewport = chip->parentWidget()->parentWidget();   // host -> scroll viewport
        QVERIFY2(viewport->rect().contains(chip->geometry().adjusted(0, 0, -1, -1)),
                 qPrintable(chip->text()));
    }

}

// Hovering a chip previews its set: the rows of its layers are shaded, the others are not, and the
// shade goes when the pointer leaves.
void LayerWidgetTests::hoveringASetChipShadesItsLayersRows()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    doc.addVisibilitySet(QStringLiteral("only a"), { doc.mesh(0).meshId });

    LayerWidget widget(&doc);
    widget.resize(320, 420);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 2)
            tree = candidate;
    }
    QVERIFY(tree);
    QToolButton *chip = nullptr;
    for (QToolButton *button : widget.findChildren<QToolButton *>()) {
        if (button->isCheckable())
            chip = button;
    }
    QVERIFY(chip);

    // The right-hand end of each layer's row, clear of its text.
    const auto sample = [&](int row) {
        const QRect rect = tree->visualItemRect(tree->topLevelItem(row));
        return tree->viewport()->grab().toImage().pixelColor(rect.right() - 6, rect.center().y());
    };
    const QColor rowABefore = sample(0);
    const QColor rowBBefore = sample(1);

    QEvent enter(QEvent::Enter);
    QCoreApplication::sendEvent(chip, &enter);
    QTest::qWait(20);
    QVERIFY(sample(0) != rowABefore);
    QCOMPARE(sample(1), rowBBefore);
    // The detail rows under a layer are shaded with it, and the other layer's are not.
    const auto detailSample = [&](int top) {
        QTreeWidgetItem *child = tree->topLevelItem(top)->child(0);
        const QRect rect = tree->visualItemRect(child);
        return tree->viewport()->grab().toImage().pixelColor(rect.right() - 6, rect.center().y());
    };
    QVERIFY(tree->topLevelItem(0)->childCount() > 0);
    QVERIFY(detailSample(0) != detailSample(1));
    const QString dir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!dir.isEmpty())
        widget.grab().save(dir + QStringLiteral("/layerwidget_hover.png"));

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(chip, &leave);
    QTest::qWait(20);
    QCOMPARE(sample(0), rowABefore);
}

// The eye in the title bar: its action shows every layer, or hides every layer when they all are
// shown, and the panel says how many are shown so that the eye can be drawn open, closed or half.
void LayerWidgetTests::theEyeShowsAllOrHidesAllAndSaysWhatIsShown()
{
    Document doc;
    LayerWidget widget(&doc);
    QCOMPARE(widget.layerVisibility(), std::make_pair(0, 0));
    widget.toggleAllVisible();   // no layers: nothing to do, and no harm

    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    QSignalSpy spy(&widget, &LayerWidget::layerVisibilityChanged);
    QCOMPARE(widget.layerVisibility(), std::make_pair(2, 2));

    widget.toggleAllVisible();
    QVERIFY(!doc.mesh(0).visible && !doc.mesh(1).visible);
    QCOMPARE(widget.layerVisibility(), std::make_pair(0, 2));
    QTRY_VERIFY(!spy.isEmpty());   // the rebuild that follows reports it
    QCOMPARE(spy.last().at(0).toInt(), 0);
    QCOMPARE(spy.last().at(1).toInt(), 2);

    // Partly shown: the next action shows the rest, rather than hiding what is shown.
    doc.setMeshVisible(0, true);
    QCOMPARE(widget.layerVisibility(), std::make_pair(1, 2));
    widget.toggleAllVisible();
    QVERIFY(doc.mesh(0).visible && doc.mesh(1).visible);
    widget.toggleAllVisible();
    QVERIFY(!doc.mesh(0).visible && !doc.mesh(1).visible);
}

// The title bar puts its collapse-all button over the disclosure arrows and its eye over the check
// marks, at the places the panel reports; they have to be where the style really draws them.
void LayerWidgetTests::theTitleBarButtonsGoOverTheArrowsAndTheCheckMarks()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    LayerWidget widget(&doc);
    widget.resize(320, 420);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 1)
            tree = candidate;
    }
    QVERIFY(tree);
    QCOMPARE(tree->mapTo(&widget, QPoint(0, 0)).x(), 0);   // the places are counted from this edge

    // The first run of columns with ink in a stretch of the row: the arrow, the check mark, and
    // not the text that follows it.
    const QImage image = tree->viewport()->grab().toImage();
    const QRect rowRect = tree->visualItemRect(tree->topLevelItem(0));
    const QColor background = image.pixelColor(image.width() - 2, rowRect.center().y());
    const auto inkCenter = [&](int from, int to) {
        int first = -1, last = -1;
        for (int x = from; x < to; ++x) {
            bool ink = false;
            for (int y = rowRect.top() + 2; y < rowRect.bottom() - 1 && !ink; ++y)
                ink = image.pixelColor(x, y) != background;
            if (ink) {
                first = first < 0 ? x : first;
                last = x;
            } else if (first >= 0) {
                break;
            }
        }
        return first < 0 ? -1.0 : (first + last) / 2.0;
    };
    const int indentation = tree->indentation();
    const int offset = tree->frameWidth();
    const double arrow = inkCenter(0, indentation);
    const double check = inkCenter(indentation, indentation + tree->columnWidth(0));
    QVERIFY(arrow >= 0 && check >= 0);
    QVERIFY2(qAbs(arrow + offset - widget.disclosureArrowCenter()) <= 3,
             qPrintable(QStringLiteral("arrow ink %1, reported %2").arg(arrow + offset).arg(widget.disclosureArrowCenter())));
    QVERIFY2(qAbs(check + offset - widget.visibilityCheckCenter()) <= 3,
             qPrintable(QStringLiteral("check ink %1, reported %2").arg(check + offset).arg(widget.visibilityCheckCenter())));
}

// A change to any layer rebuilds the list, and the rebuild used to send the list to the top and then
// back down to the current layer: with the current layer at the end, a scroll up to switch an early
// layer on or off was undone at once, and a click scrolled the clicked layer to the bottom edge.
void LayerWidgetTests::theListStaysWhereTheUserScrolledItWhenALayerChanges()
{
    Document doc;
    for (int i = 0; i < 40; ++i)
        addTinyMesh(doc, QStringLiteral("layer %1").arg(i));
    LayerWidget widget(&doc);
    widget.resize(320, 300);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 40)
            tree = candidate;
    }
    QVERIFY(tree);
    QScrollBar *bar = tree->verticalScrollBar();
    QVERIFY(bar->maximum() > 10);   // there is something to scroll

    // The last layer is the current one; the user scrolls up and switches the first off.
    bar->setValue(0);
    doc.setMeshVisible(0, false);
    QTest::qWait(50);
    QCOMPARE(bar->value(), 0);

    // Somewhere in the middle, a change to a layer out of sight leaves it where it is.
    bar->setValue(10);
    doc.setMeshVisible(1, false);
    QTest::qWait(50);
    QCOMPARE(bar->value(), 10);

    // So does choosing a layer that is wholly in view.
    const QRect row = tree->visualItemRect(tree->topLevelItem(14));
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, row.center());
    QTest::qWait(50);
    QCOMPARE(doc.currentMeshIndex(), 14);
    QCOMPARE(bar->value(), 10);

    // A layer that has just come is the current one and is brought into view.
    addTinyMesh(doc, QStringLiteral("new"));
    QTest::qWait(50);
    QCOMPARE(tree->topLevelItemCount(), 41);
    QVERIFY(tree->viewport()->rect().contains(tree->visualItemRect(tree->topLevelItem(40)).center()));

    // The table is rebuilt in the same way and stays put as well.
    widget.setViewMode(LayerWidget::ViewMode::Table);
    QTest::qWait(50);
    QTableWidget *table = nullptr;
    for (QTableWidget *candidate : widget.findChildren<QTableWidget *>()) {
        if (candidate->rowCount() == 41)
            table = candidate;
    }
    QVERIFY(table);
    QScrollBar *tableBar = table->verticalScrollBar();
    QVERIFY(tableBar->maximum() > 10);
    tableBar->setValue(0);
    doc.setMeshVisible(0, true);
    QTest::qWait(50);
    QCOMPARE(tableBar->value(), 0);
}

// Stored-state labels follow the same save/edit transitions for layers and textures.
// The label is separate from the layer name, and the table exposes the same words.
void LayerWidgetTests::theMarkSaysWhetherAFileHoldsTheLayer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Document doc;
    addTinyMesh(doc, QStringLiteral("tiny"));
    doc.mesh(0).mesh.face.EnableWedgeTexCoord();
    doc.mesh(0).ioMask |= vcg::tri::io::Mask::IOM_WEDGTEXCOORD;
    QImage red(2, 2, QImage::Format_RGBA8888);
    red.fill(Qt::red);
    TextureAssociationUtils::replaceTextureAssociations(
        doc.mesh(0), { TextureAssociationUtils::makeTextureAssetFromImage(red, QStringLiteral("atlas.png")) });
    LayerWidget widget(&doc);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 1)
            tree = candidate;
    }
    QVERIFY(tree);
    const auto layerText = [&] { return tree->topLevelItem(0)->text(0); };
    const auto layerStatus = [&] {
        return tree->topLevelItem(0)->data(0, Qt::AccessibleDescriptionRole).toString();
    };
    const auto textureMark = [&] {
        QTreeWidgetItem *layer = tree->topLevelItem(0);
        for (int c = 0; c < layer->childCount(); ++c)
            if (layer->child(c)->text(1).startsWith(QStringLiteral("Tex"))) {
                QWidget *info = tree->itemWidget(layer->child(c), 2);
                for (QLabel *label : info->findChildren<QLabel *>()) {
                    if (label->text().contains(QStringLiteral(" · ")))
                        return label->text().section(QStringLiteral(" · "), 0, 0);
                }
            }
        return QStringLiteral("no texture row");
    };
    QTableWidget *table = nullptr;
    for (QTableWidget *candidate : widget.findChildren<QTableWidget *>()) {
        if (candidate->columnCount() == 9)
            table = candidate;
    }
    QVERIFY(table);
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("Status"));
    const auto tableStatus = [&] {
        // Only the visible view is rebuilt; switch views as the user does before reading it.
        widget.setViewMode(LayerWidget::ViewMode::Table);
        const QTableWidgetItem *status = table->item(0, 2);
        const QString text = status ? status->text() : QStringLiteral("missing status");
        widget.setViewMode(LayerWidget::ViewMode::Tree);
        return text;
    };
    const QString notStored = QStringLiteral("Not saved");
    const QString changed = QStringLiteral("Modified");

    QCOMPARE(layerText(), QStringLiteral("tiny"));
    QCOMPARE(layerStatus(), notStored);
    QCOMPARE(tableStatus(), notStored);
    QCOMPARE(textureMark(), notStored);

    QCOMPARE(doc.saveMesh(0, dir.filePath(QStringLiteral("tiny.ply"))), 0);
    QTest::qWait(50);
    QCOMPARE(layerText(), QStringLiteral("tiny"));
    QCOMPARE(layerStatus(), QStringLiteral("Saved"));
    QCOMPARE(tableStatus(), QStringLiteral("Saved"));
    QCOMPARE(textureMark(), QStringLiteral("Saved"));

    doc.mesh(0).mesh.face[0].SetS();
    doc.markMeshSelectionChanged(0);
    QTest::qWait(50);
    QCOMPARE(layerText(), QStringLiteral("tiny"));
    QCOMPARE(layerStatus(), changed);
    QCOMPARE(tableStatus(), changed);

    const QString grabDir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!grabDir.isEmpty())
        widget.grab().save(grabDir + QStringLiteral("/layerwidget_status.png"));
}

// A name typed in the panel is applied by the rename filter of what was renamed -- run on that
// layer, so it is an action in the history and a line in a generated script -- never written
// into the document directly. The main window runs what the panel asks for; here the request
// is the whole story. An unchanged or empty name, or Escape, asks for nothing.
void LayerWidgetTests::typingANameRunsTheRenameFilter()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("first"));
    addTinyMesh(doc, QStringLiteral("second"));
    doc.mesh(0).mesh.face.EnableWedgeTexCoord();
    QImage red(2, 2, QImage::Format_RGBA8888);
    red.fill(Qt::red);
    TextureAssociationUtils::replaceTextureAssociations(
        doc.mesh(0), { TextureAssociationUtils::makeTextureAssetFromImage(red, QStringLiteral("atlas.png")) });
    QImage photo(4, 4, QImage::Format_RGBA8888);
    photo.fill(Qt::blue);
    doc.addRasterImage(photo, QStringLiteral("photo"));
    doc.setCurrentMeshIndex(1);

    LayerWidget widget(&doc);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QSignalSpy runs(&widget, &LayerWidget::filterRunRequested);
    const auto lastRun = [&] { return runs.last(); };
    const auto keyOf = [&](const QString &id) {
        for (const Document::FilterInfo &info : doc.filterInfos())
            if (info.descriptor.id == id)
                return info.key;
        return QString();
    };

    QTreeWidget *meshTree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 2)
            meshTree = candidate;
    }
    QVERIFY(meshTree);

    // The tree: an edited mesh name renames that layer, which is made current first.
    meshTree->topLevelItem(0)->setText(0, QStringLiteral("  renamed  "));
    QCOMPARE(runs.count(), 1);
    QCOMPARE(lastRun().at(0).toString(), keyOf(QStringLiteral("rename_current_mesh_layer")));
    QCOMPARE(lastRun().at(2).toMap().value(QStringLiteral("newName")).toString(),
             QStringLiteral("renamed"));
    QCOMPARE(doc.currentMeshIndex(), 0);
    QCOMPARE(doc.mesh(0).name, QStringLiteral("first")); // the filter, not the panel, renames

    // Unchanged or empty: nothing is run, and the old name comes back.
    QTest::qWait(50);
    meshTree->topLevelItem(1)->setText(0, QStringLiteral("   "));
    QTest::qWait(50);
    QCOMPARE(runs.count(), 1);
    QCOMPARE(meshTree->topLevelItem(1)->text(0), QStringLiteral("second"));

    // The raster table's name column.
    widget.setViewMode(LayerWidget::ViewMode::Table);
    QTableWidget *rasterTable = nullptr;
    for (QTableWidget *candidate : widget.findChildren<QTableWidget *>()) {
        if (candidate->rowCount() == 1)
            rasterTable = candidate;
    }
    QVERIFY(rasterTable);
    rasterTable->item(0, 2)->setText(QStringLiteral("photo 2"));
    QCOMPARE(runs.count(), 2);
    QCOMPARE(lastRun().at(0).toString(), keyOf(QStringLiteral("rename_current_raster")));
    QCOMPARE(lastRun().at(2).toMap().value(QStringLiteral("newName")).toString(),
             QStringLiteral("photo 2"));
    widget.setViewMode(LayerWidget::ViewMode::Tree);
    QTest::qWait(50);

    // A texture: a double-click puts an editor where its name is.
    QTreeWidgetItem *layer = meshTree->topLevelItem(0);
    QTreeWidgetItem *texture = nullptr;
    for (int c = 0; c < layer->childCount(); ++c)
        if (layer->child(c)->text(1).startsWith(QStringLiteral("Tex")))
            texture = layer->child(c);
    QVERIFY(texture);
    emit meshTree->itemDoubleClicked(texture, 1);
    auto *edit = qobject_cast<QLineEdit *>(meshTree->itemWidget(texture, 2));
    QVERIFY(edit);
    QCOMPARE(edit->text(), QStringLiteral("atlas.png"));

    // Escape leaves the name alone.
    QTest::keyClick(edit, Qt::Key_Escape);
    QTest::qWait(50);
    QCOMPARE(runs.count(), 2);

    layer = meshTree->topLevelItem(0);
    texture = nullptr;
    for (int c = 0; c < layer->childCount(); ++c)
        if (layer->child(c)->text(1).startsWith(QStringLiteral("Tex")))
            texture = layer->child(c);
    QVERIFY(texture);
    emit meshTree->itemDoubleClicked(texture, 1);
    edit = qobject_cast<QLineEdit *>(meshTree->itemWidget(texture, 2));
    QVERIFY(edit);
    edit->setText(QStringLiteral("albedo.png"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(runs.count(), 3);
    QCOMPARE(lastRun().at(0).toString(), keyOf(QStringLiteral("rename_texture")));
    const QVariantMap textureParams = lastRun().at(2).toMap();
    QCOMPARE(textureParams.value(QStringLiteral("texture")).toInt(), 1);
    QCOMPARE(textureParams.value(QStringLiteral("textureName")).toString(),
             QStringLiteral("albedo.png"));

    // The texture row's menu removes that one texture, through Remove Textures.
    QTest::qWait(50);
    layer = meshTree->topLevelItem(0);
    texture = nullptr;
    for (int c = 0; c < layer->childCount(); ++c)
        if (layer->child(c)->text(1).startsWith(QStringLiteral("Tex")))
            texture = layer->child(c);
    QVERIFY(texture);
    meshTree->scrollToItem(texture);
    const QPoint texturePoint = widget.mapFrom(meshTree->viewport(), meshTree->visualItemRect(texture).center());
    QTimer::singleShot(0, &widget, [] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            for (QAction *action : menu->actions())
                if (action->text() == QStringLiteral("Remove"))
                    action->trigger();
            menu->close();
        }
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, texturePoint, widget.mapToGlobal(texturePoint));
    QApplication::sendEvent(&widget, &event);
    QCOMPARE(runs.count(), 4);
    QCOMPARE(lastRun().at(0).toString(), keyOf(QStringLiteral("remove_textures")));
    QCOMPARE(lastRun().at(2).toMap().value(QStringLiteral("texture")).toInt(), 1);
}

// Ctrl+D duplicates the current layer through the filter, wherever the focus is; and a mesh
// row's context menu puts what is done to that layer first.
void LayerWidgetTests::ctrlDDuplicatesTheCurrentLayer()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("only"));
    LayerWidget widget(&doc);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QSignalSpy runs(&widget, &LayerWidget::filterRunRequested);
    QTest::keyClick(&widget, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(runs.count(), 1);
    QCOMPARE(runs.last().at(1).toString(), QStringLiteral("Duplicate Current Layer"));

    // A mesh row's menu: that layer's own commands first, Remove last, no raster filters.
    QTreeWidget *meshTree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>())
        if (candidate->topLevelItemCount() == 1)
            meshTree = candidate;
    QVERIFY(meshTree);
    const QPoint rowCenter = widget.mapFrom(
        meshTree->viewport(), meshTree->visualItemRect(meshTree->topLevelItem(0)).center());
    QStringList entries;
    QTimer::singleShot(0, &widget, [&] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            for (QAction *action : menu->actions())
                entries << (action->isSeparator() ? QStringLiteral("-") : action->text());
            menu->close();
        }
    });
    QContextMenuEvent event(QContextMenuEvent::Mouse, rowCenter, widget.mapToGlobal(rowCenter));
    QApplication::sendEvent(&widget, &event);
    QCOMPARE(entries.mid(0, 6), (QStringList{ QStringLiteral("Rename"), QStringLiteral("Duplicate"),
                                              QStringLiteral("-"), QStringLiteral("Save Mesh..."),
                                              QStringLiteral("Reload"), QStringLiteral("-") }));
    QCOMPARE(entries.last(), QStringLiteral("Remove"));
    QVERIFY(!entries.join(QLatin1Char(' ')).contains(QStringLiteral("Raster")));
}

// The selected column of the Vert, Edge and Face rows. A selection change is not a data change,
// so the counts must follow it on their own: rebuilding only when the stored-state mark turned
// left every selection after the first showing the first one's numbers.
void LayerWidgetTests::selectedCountsFollowEverySelectionChange()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("tiny"));

    LayerWidget widget(&doc);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 1)
            tree = candidate;
    }
    QVERIFY(tree);
    const auto row = [&](const QString &label) {
        QTreeWidgetItem *layer = tree->topLevelItem(0);
        for (int c = 0; c < layer->childCount(); ++c)
            if (layer->child(c)->text(1) == label)
                return layer->child(c)->text(0) + QLatin1Char('/') + layer->child(c)->text(2);
        return QStringLiteral("no row");
    };
    QCOMPARE(row(QStringLiteral("Face")), QStringLiteral("1/"));

    // Never saved, so the mark stays Not saved through every change below.
    VCGMesh &mesh = doc.mesh(0).mesh;
    mesh.vert[0].SetS();
    doc.markMeshSelectionChanged(0);
    QTest::qWait(50);
    QCOMPARE(row(QStringLiteral("Vert")), QStringLiteral("3/1"));

    mesh.vert[1].SetS();
    mesh.face[0].SetS();
    doc.markMeshSelectionChanged(0);
    QTest::qWait(50);
    QCOMPARE(row(QStringLiteral("Vert")), QStringLiteral("3/2"));
    QCOMPARE(row(QStringLiteral("Face")), QStringLiteral("1/1"));

    mesh.vert[0].ClearS();
    mesh.vert[1].ClearS();
    mesh.face[0].ClearS();
    doc.markMeshSelectionChanged(0);
    QTest::qWait(50);
    QCOMPARE(row(QStringLiteral("Vert")), QStringLiteral("3/"));
    QCOMPARE(row(QStringLiteral("Face")), QStringLiteral("1/"));
}

// No row of zeros: a point cloud is counted in vertices, a polyline in vertices and edges, a
// surface in vertices and faces. The table leaves the cells blank, and the summary under the
// list leaves out a kind no layer has.
void LayerWidgetTests::countRowsListOnlyTheKindsALayerHas()
{
    Document doc;
    VCGMesh cloud;
    vcg::tri::Allocator<VCGMesh>::AddVertices(cloud, 2);
    doc.addMesh(cloud, QStringLiteral("cloud"), vcg::tri::io::Mask::IOM_VERTCOORD);
    VCGMesh polyline;
    vcg::tri::Allocator<VCGMesh>::AddVertices(polyline, 3);
    vcg::tri::Allocator<VCGMesh>::AddEdge(polyline, &polyline.vert[0], &polyline.vert[1]);
    vcg::tri::Allocator<VCGMesh>::AddEdge(polyline, &polyline.vert[1], &polyline.vert[2]);
    doc.addMesh(polyline, QStringLiteral("polyline"), vcg::tri::io::Mask::IOM_EDGEINDEX);

    LayerWidget widget(&doc);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QCOMPARE(widget.summaryText(), QStringLiteral("2 E  5 V"));
    addTinyMesh(doc, QStringLiteral("surface"));
    QTest::qWait(50);
    QCOMPARE(widget.summaryText(), QStringLiteral("1 T  2 E  8 V"));

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 3)
            tree = candidate;
    }
    QVERIFY(tree);
    const auto counts = [&](int layer) {
        QStringList rows;
        QTreeWidgetItem *item = tree->topLevelItem(layer);
        for (int c = 0; c < item->childCount(); ++c)
            if (!item->child(c)->text(0).isEmpty())
                rows << item->child(c)->text(1) + QLatin1Char('=') + item->child(c)->text(0);
        return rows.join(QLatin1Char(' '));
    };
    QCOMPARE(counts(0), QStringLiteral("Vert=2"));
    QCOMPARE(counts(1), QStringLiteral("Vert=3 Edge=2"));
    QCOMPARE(counts(2), QStringLiteral("Vert=3 Face=1"));

    QTableWidget *table = nullptr;
    for (QTableWidget *candidate : widget.findChildren<QTableWidget *>()) {
        if (candidate->columnCount() == 9)
            table = candidate;
    }
    QVERIFY(table);
    widget.setViewMode(LayerWidget::ViewMode::Table);
    const auto cells = [&](int row) {
        return QStringList{ table->item(row, 3)->text(), table->item(row, 4)->text(),
                            table->item(row, 5)->text() }.join(QLatin1Char('|'));
    };
    QCOMPARE(cells(0), QStringLiteral("2||"));
    QCOMPARE(cells(1), QStringLiteral("3|2|"));
    QCOMPARE(cells(2), QStringLiteral("3||1"));
}

QTEST_MAIN(LayerWidgetTests)
#include "test_layerwidget.moc"

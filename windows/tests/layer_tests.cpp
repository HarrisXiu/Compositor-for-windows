// SPDX-License-Identifier: MIT
#include "editor.h"
#include "image_operations.h"
#include "language.h"
#include "layer_operations.h"
#include "layer_transfer.h"
#include "render.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QJsonArray>
#include <QRadioButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>
using namespace compositor;
namespace {
QImage pixels(QSize size, QColor color) {
    QImage result(size, QImage::Format_RGBA8888_Premultiplied);
    result.fill(color);
    return result;
}
QString add(Document &d, QColor color, QRect bounds) {
    const auto id = d.addImage("Layer", pixels(bounds.size(), color));
    d.find(id)->setBounds(bounds);
    return id;
}
QAction *action(EditorWindow &window, const QString &key) {
    for (auto result : window.findChildren<QAction *>())
        if (result->property("layerAction").toString() == key)
            return result;
    return nullptr;
}
EditorPage *page(EditorWindow &window) {
    return qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
}
QStringList siblings(const Document &d, const QString &parent = {}) {
    QStringList result;
    for (const auto &layer : d.layers)
        if (layer.parent() == parent)
            result << layer.id();
    return result;
}
void mask(Layer &layer, int value, QRect bounds) {
    layer.mask = QImage(1, 1, QImage::Format_Grayscale8);
    layer.mask.fill(value);
    layer.metadata["maskFile"] = layer.id() + ".mask.png";
    layer.metadata["maskPlacement"] = makeTransform(bounds);
    layer.metadata["maskLinked"] = false;
}
} // namespace
class LayerTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void cleanup() {
        QApplication::clipboard()->clear();
        UiLanguage::instance().setLanguage("en", false);
    }
    void mergeDownPreservesPixelsAndRedirectsClips() {
        auto d = Document::create({8, 8});
        auto a = add(d, Qt::red, {0, 0, 5, 5});
        auto b = add(d, QColor(0, 0, 255, 128), {2, 2, 5, 5});
        d.find(b)->metadata["opacity"] = .7;
        d.metadata["activeLayerID"] = b;
        const auto expected = renderDocument(d);
        const auto merged = mergeLayers(d, {b});
        QCOMPARE(d.layers.size(), 1);
        QCOMPARE(renderDocument(d), expected);
        QVERIFY(merged != a && merged != b);
        QCOMPARE(d.active()->blend(), QString("Normal"));
        d.validateAssets();
        auto clip = add(d, Qt::green, {0, 0, 8, 8});
        d.find(clip)->metadata["maskSourceID"] = merged;
        auto top = add(d, Qt::white, {0, 0, 1, 1});
        d.metadata["activeLayerID"] = top;
        auto next = mergeLayers(d, {merged, top});
        QCOMPARE(d.find(clip)->metadata.value("maskSourceID").toString(), next);
        d.validateAssets();
    }
    void groupMergeDoesNotDoubleApplyOutsideOpacity() {
        auto d = Document::create({8, 8});
        auto outer = d.addGroup("Outer");
        d.find(outer)->metadata["opacity"] = .5;
        auto inner = d.addGroup("Inner");
        d.find(inner)->metadata["parentID"] = outer;
        auto a = add(d, Qt::red, {1, 1, 6, 6});
        d.find(a)->metadata["parentID"] = inner;
        d.metadata["activeLayerID"] = inner;
        const auto expected = renderDocument(d);
        QCOMPARE(mergeLabel(d, {inner}), QString("Merge Group"));
        auto merged = mergeLayers(d, {inner});
        QCOMPARE(d.find(merged)->parent(), outer);
        QCOMPARE(renderDocument(d), expected);
        d.validateAssets();
    }
    void selectedAncestorMergeLeavesValidHierarchy() {
        auto d = Document::create({8, 8});
        auto group = d.addGroup("Group");
        auto a = add(d, Qt::red, {0, 0, 2, 2});
        d.find(a)->metadata["parentID"] = group;
        auto b = add(d, Qt::green, {3, 0, 2, 2});
        d.find(b)->metadata["parentID"] = group;
        const auto expected = renderDocument(d);
        QCOMPARE(mergeLabel(d, {group, b}), QString("Merge Layers"));
        mergeLayers(d, {group, b});
        QCOMPARE(d.layers.size(), 1);
        QVERIFY(d.active()->parent().isEmpty());
        QCOMPARE(renderDocument(d), expected);
        d.validateAssets();
    }
    void transparentAndUnsupportedMerge() {
        auto d = Document::create({8, 8});
        auto a = add(d, Qt::transparent, {1, 1, 3, 3});
        auto b = add(d, Qt::transparent, {2, 2, 2, 2});
        mergeLayers(d, {b});
        QCOMPARE(d.active()->image.size(), QSize(1, 1));
        d.validateAssets();
        QVERIFY(mergeLabel(d, {d.activeId()}).isEmpty());
        Q_UNUSED(a);
    }
    void groupingAcrossFoldersAndUngroupOrder() {
        auto d = Document::create({8, 8});
        auto f = d.addGroup("Old folder");
        auto a = add(d, Qt::red, {0, 0, 2, 2});
        d.find(a)->metadata["parentID"] = f;
        auto b = add(d, Qt::blue, {3, 0, 2, 2});
        auto group = groupLayers(d, {a, b});
        QVERIFY(d.find(group)->parent().isEmpty());
        QCOMPARE(siblings(d, group), QStringList({a, b}));
        d.validateAssets();
        QCOMPARE(ungroupLayer(d, group), QStringList({a, b}));
        QCOMPARE(siblings(d), QStringList({f, a, b}));
        d.validateAssets();
    }
    void moveBlocksAndMoveOutRetainSiblingOrder() {
        auto d = Document::create({8, 8});
        auto a = add(d, Qt::red, {0, 0, 1, 1});
        auto b = add(d, Qt::blue, {1, 0, 1, 1});
        auto c = add(d, Qt::green, {2, 0, 1, 1});
        auto e = add(d, Qt::white, {3, 0, 1, 1});
        moveLayers(d, {b, c}, 1);
        QCOMPARE(siblings(d), QStringList({a, e, b, c}));
        moveLayers(d, {b, c}, -1);
        QCOMPARE(siblings(d), QStringList({a, b, c, e}));
        auto group = groupLayers(d, {b, c});
        moveLayersOut(d, {b, c});
        QCOMPARE(siblings(d), QStringList({a, group, b, c, e}));
        d.validateAssets();
    }
    void subtreeCopyRemapsIdsAndPreservesMetadata() {
        auto source = Document::create({8, 8});
        auto f = source.addGroup("Folder");
        auto a = add(source, Qt::red, {0, 0, 2, 2});
        auto b = add(source, Qt::blue, {0, 0, 2, 2});
        source.find(a)->metadata["parentID"] = f;
        source.find(b)->metadata["parentID"] = f;
        source.find(b)->metadata["maskSourceID"] = a;
        source.find(b)->metadata["customRecord"] = QJsonObject{{"value", 42}};
        mask(*source.find(b), 170, {0, 0, 2, 2});
        auto destination = Document::create({20, 20});
        auto ids = copyLayers(destination, source, {f, b}, QPointF(8, 8));
        QCOMPARE(ids.size(), 1);
        auto roots = siblings(destination);
        QCOMPARE(roots, ids);
        auto children = siblings(destination, ids.front());
        QCOMPARE(children.size(), 2);
        QCOMPARE(destination.find(children[1])->metadata.value("maskSourceID").toString(),
                 children[0]);
        QVERIFY(!source.find(children[0]));
        QCOMPARE(destination.find(children[1])
                     ->metadata.value("customRecord")
                     .toObject()
                     .value("value")
                     .toInt(),
                 42);
        QCOMPARE(source.find(b)->mask.cacheKey(), destination.find(children[1])->mask.cacheKey());
        destination.validateAssets();
        QTemporaryDir dir;
        saveProject(destination, dir.filePath("copy.comp"));
        const auto loaded = loadProject(dir.filePath("copy.comp"));
        QCOMPARE(loaded.manifest(), destination.manifest());
        QCOMPARE(renderDocument(loaded), renderDocument(destination));
    }
    void externalClipBakedAndIndependentMaskMoved() {
        auto source = Document::create({8, 8});
        auto a = add(source, Qt::red, {0, 0, 4, 4});
        auto b = add(source, Qt::blue, {0, 0, 8, 8});
        source.find(b)->metadata["maskSourceID"] = a;
        mask(*source.find(b), 255, {0, 0, 8, 8});
        auto target = Document::create({12, 12});
        const auto id = copyLayers(target, source, {b}).front();
        auto layer = target.find(id);
        QVERIFY(!layer->metadata.contains("maskSourceID"));
        QCOMPARE(layer->image.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(layer->image.pixelColor(6, 6).alpha(), 0);
        QCOMPARE(layer->transform().value("origin").toArray(), QJsonArray({2, 2}));
        QCOMPARE(layer->metadata.value("maskPlacement").toObject().value("origin").toArray(),
                 QJsonArray({2, 2}));
        QCOMPARE(source.find(b)->metadata.value("maskSourceID").toString(), a);
        target.validateAssets();
    }
    void clipboardSnapshotSurvivesSourceEditsAndClosure() {
        auto d = Document::create({4, 4});
        auto id = add(d, Qt::red, {0, 0, 4, 4});
        QApplication::clipboard()->setMimeData(new LayerTransferMimeData(d, {id}));
        auto transfer = layerTransfer(QApplication::clipboard()->mimeData());
        QVERIFY(transfer);
        d.find(id)->image.fill(Qt::blue);
        d = {};
        QCOMPARE(transfer->source.find(id)->image.pixelColor(0, 0), QColor(Qt::red));
        QMimeData untrusted;
        untrusted.setData("application/x-compositor-layer-transfer", "arbitrary-external-input");
        QVERIFY(!layerTransfer(&untrusted));
    }
    void pasteSnapshotAfterOriginalParentAndClipWereDeleted() {
        auto source = Document::create({8, 8});
        auto folder = source.addGroup("Folder");
        auto a = add(source, Qt::red, {0, 0, 4, 4});
        auto b = add(source, Qt::blue, {0, 0, 8, 8});
        source.find(a)->metadata["parentID"] = folder;
        source.find(b)->metadata["parentID"] = folder;
        source.find(b)->metadata["maskSourceID"] = a;
        auto destination = source;
        destination.remove(folder);
        const auto id = copyLayers(destination, source, {b}, std::nullopt, true).front();
        QVERIFY(destination.find(id)->parent().isEmpty());
        QVERIFY(!destination.find(id)->metadata.contains("maskSourceID"));
        QCOMPARE(destination.find(id)->image.pixelColor(6, 6).alpha(), 0);
        destination.validateAssets();
    }
    void dragReparentRejectsCyclesAndPreservesSelectedBlock() {
        auto d = Document::create({8, 8});
        auto a = add(d, Qt::red, {0, 0, 1, 1});
        auto b = add(d, Qt::blue, {1, 0, 1, 1});
        auto c = add(d, Qt::green, {2, 0, 1, 1});
        auto folder = d.addGroup("Folder");
        moveLayersTo(d, {a, b}, folder);
        QCOMPARE(siblings(d, folder), QStringList({a, b}));
        moveLayersTo(d, {a, b}, {}, c, false);
        QCOMPARE(siblings(d), QStringList({a, b, c, folder}));
        moveLayersTo(d, {a, b}, {}, folder, true);
        QCOMPARE(siblings(d), QStringList({c, folder, a, b}));
        auto nested = d.addGroup("Nested");
        d.find(nested)->metadata["parentID"] = folder;
        const auto before = d.manifest();
        QVERIFY_EXCEPTION_THROWN(moveLayersTo(d, {folder}, nested), Error);
        QCOMPARE(d.manifest(), before);
        d.validateAssets();
    }
    void thumbnailClickLoadsSelectionAndSwitchesMaskTarget() {
        EditorWindow window;
        auto p = page(window);
        resizeCanvas(p->document, {20, 20}, 0);
        auto a = add(p->document, QColor(255, 0, 0, 128), {2, 3, 4, 4});
        mask(*p->document.find(a), 200, {8, 9, 4, 4});
        p->changed();
        window.show();
        QApplication::processEvents();
        auto tree = window.findChild<QTreeWidget *>("layerTree");
        auto row = tree->visualItemRect(tree->topLevelItem(0));
        const QPoint maskPoint(tree->columnWidth(0) + tree->columnWidth(1) / 2, row.center().y());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ControlModifier, maskPoint);
        QVERIFY(!p->session.selection.isNull());
        QCOMPARE(p->session.selection.constScanLine(9)[8], uchar(200));
        QVERIFY(!p->isModified());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, maskPoint);
        QCOMPARE(p->session.target, EditTarget::Mask);
        QApplication::processEvents();
        row = tree->visualItemRect(tree->topLevelItem(0));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ControlModifier,
                          QPoint(row.left() + 40, row.center().y()));
        QVERIFY(!p->session.selection.isNull());
        QCOMPARE(p->session.selection.constScanLine(3)[2], uchar(128));
        QVERIFY(!p->isModified());
        tree->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
        QApplication::processEvents();
        QCOMPARE(p->document.find(a)->visible(), false);
        p->history.undo();
        QCOMPARE(p->document.find(a)->visible(), true);
    }
    void thumbnailAlphaUsesPlacementAndIgnoresLayerOpacity() {
        auto d = Document::create({8, 8});
        auto a = add(d, QColor(255, 0, 0, 128), {2, 3, 2, 2});
        d.find(a)->metadata["opacity"] = 0;
        mask(*d.find(a), 200, {5, 5, 2, 2});
        const auto alpha = layerAlphaSelection(d, *d.find(a), false);
        QCOMPARE(alpha.constScanLine(3)[2], uchar(128));
        QCOMPARE(alpha.constScanLine(0)[0], uchar(0));
        const auto coverage = layerAlphaSelection(d, *d.find(a), true);
        QCOMPARE(coverage.constScanLine(5)[5], uchar(200));
        QCOMPARE(coverage.constScanLine(3)[2], uchar(0));
    }
    void canvasAnchorsOffsetsMasksGuidesAndExtension() {
        auto d = Document::create({4, 4});
        auto a = add(d, Qt::red, {0, 0, 4, 4});
        mask(*d.find(a), 255, {0, 0, 4, 4});
        d.metadata["guides"] =
            QJsonArray{QJsonObject{{"id", newId()}, {"axis", "vertical"}, {"position", 1}}};
        resizeCanvas(d, {7, 7}, 4, Qt::white);
        QCOMPARE(d.find(a)->transform().value("origin").toArray(), QJsonArray({1, 1}));
        QCOMPARE(d.find(a)->metadata.value("maskPlacement").toObject().value("origin").toArray(),
                 QJsonArray({1, 1}));
        QCOMPARE(d.metadata.value("guides").toArray()[0].toObject().value("position").toInt(), 2);
        QCOMPARE(d.activeId(), a);
        QCOMPARE(d.layers.front().image.pixelColor(2, 2).alpha(), 0);
        const auto rendered = renderDocument(d);
        QCOMPARE(rendered.pixelColor(0, 0), QColor(Qt::white));
        QCOMPARE(rendered.pixelColor(2, 2), QColor(Qt::red));
        cropCanvas(d, QRect(-2, -3, 5, 6));
        QCOMPARE(d.find(a)->transform().value("origin").toArray(), QJsonArray({3, 4}));
        d.validateAssets();
    }
    void imageResizeNonuniformRotationAndUniformMask() {
        auto d = Document::create({20, 20});
        auto a = add(d, Qt::red, {4, 4, 6, 4});
        auto transform = d.find(a)->transform();
        transform["rotation"] = 30;
        d.find(a)->metadata["transform"] = transform;
        mask(*d.find(a), 255, {4, 4, 6, 4});
        auto expectedTransform = d.find(a)->placement(d.find(a)->image.size());
        QTransform scale;
        scale.scale(2, 1);
        auto expectedBounds =
            (expectedTransform * scale).mapRect(QRectF(0, 0, 6, 4)).toAlignedRect();
        const auto oldMask = d.find(a)->mask.cacheKey();
        resizeImage(d, {40, 20}, 144, Qt::FastTransformation);
        QCOMPARE(d.find(a)->image.size(), expectedBounds.size());
        QCOMPARE(d.find(a)->transform().value("rotation").toDouble(), 0);
        QCOMPARE(d.find(a)->transform().value("origin").toArray(),
                 QJsonArray({expectedBounds.x(), expectedBounds.y()}));
        QCOMPARE(d.find(a)->mask.cacheKey(), oldMask);
        QCOMPARE(d.metadata.value("resolution").toInt(), 144);
        d.validateAssets();
        auto image = d.find(a)->image;
        resizeImage(d, d.size(), 300);
        QCOMPARE(d.find(a)->image.cacheKey(), image.cacheKey());
    }
    void trimTransparentAndCornerColorSides() {
        auto image = pixels({6, 6}, Qt::transparent);
        image.setPixelColor(2, 3, Qt::red);
        TrimOptions options;
        QCOMPARE(trimBounds(image, options), QRect(2, 3, 1, 1));
        options.left = false;
        options.bottom = false;
        QCOMPARE(trimBounds(image, options), QRect(0, 3, 3, 3));
        image.fill(Qt::white);
        image.setPixelColor(2, 3, Qt::red);
        options = {};
        options.basis = TrimOptions::Basis::TopLeft;
        QCOMPARE(trimBounds(image, options), QRect(2, 3, 1, 1));
        image.fill(Qt::transparent);
        options = {};
        QVERIFY(trimBounds(image, options).isEmpty());
    }
    void rotatedUniformIndependentMaskKeepsCoverageAfterResampling() {
        auto d = Document::create({20, 20});
        auto a = add(d, Qt::red, {0, 0, 20, 20});
        mask(*d.find(a), 255, {4, 4, 8, 6});
        auto placement = d.find(a)->metadata.value("maskPlacement").toObject();
        placement["rotation"] = 30;
        d.find(a)->metadata["maskPlacement"] = placement;
        resizeImage(d, {40, 20}, 72, Qt::FastTransformation);
        QVERIFY(d.find(a)->mask.size() != QSize(1, 1));
        const auto result = renderDocument(d);
        QCOMPARE(result.pixelColor(16, 7).alpha(), 255);
        QCOMPARE(result.pixelColor(7, 2).alpha(), 0);
        d.validateAssets();
    }
    void canvasFlipMirrorsRotationAndIndependentMask() {
        auto d = Document::create({20, 20});
        auto a = add(d, Qt::red, {3, 4, 8, 6});
        auto t = d.find(a)->transform();
        t["rotation"] = 30;
        d.find(a)->metadata["transform"] = t;
        mask(*d.find(a), 255, {0, 0, 10, 20});
        auto before = renderDocument(d);
        auto original = d.manifest();
        flipCanvas(d, true);
        const auto actual = renderDocument(d), expected = before.mirrored(true, false);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x)
                QVERIFY(std::abs(actual.pixelColor(x, y).alpha() -
                                 expected.pixelColor(x, y).alpha()) <= 1);
        flipCanvas(d, true);
        QCOMPARE(d.manifest(), original);
    }
    void invalidResizeRollsBackPixelsSelectionAndLayerIds() {
        auto d = Document::create({4, 4});
        auto a = add(d, Qt::red, {0, 0, 4, 4});
        auto b = add(d, Qt::blue, {0, 0, 4, 4});
        EditorPage p(d);
        p.session.selectedLayerIDs = {a, b};
        p.session.selection = QImage(4, 4, QImage::Format_Grayscale8);
        p.session.selection.fill(128);
        const auto original = p.document.manifest();
        const auto selection = p.session.selection;
        QSignalSpy errors(&p, &EditorPage::error);
        p.edit("Invalid", [&](Document &document) {
            p.session.selectedLayerIDs = {a};
            document.metadata["width"] = 0;
        });
        QCOMPARE(errors.count(), 1);
        QCOMPARE(p.document.manifest(), original);
        QCOMPARE(p.session.selectedLayerIDs, QSet<QString>({a, b}));
        QCOMPARE(p.session.selection, selection);
        QCOMPARE(p.history.count(), 0);
    }
    void multiSelectionMergeMenuAndUndoRestore() {
        EditorWindow window;
        auto p = page(window);
        auto a = add(p->document, Qt::red, {0, 0, 4, 4});
        auto b = add(p->document, Qt::blue, {1, 1, 4, 4});
        p->session.selectedLayerIDs = {a, b};
        p->changed();
        auto tree = window.findChild<QTreeWidget *>("layerTree");
        QCOMPARE(tree->selectedItems().size(), 2);
        auto merge = action(window, "Merge Down");
        QVERIFY(merge);
        QCOMPARE(merge->text(), QString("Merge Layers"));
        merge->trigger();
        QCOMPARE(p->document.layers.size(), 1);
        QCOMPARE(p->history.count(), 1);
        p->history.undo();
        QCOMPARE(p->document.layers.size(), 2);
        QCOMPARE(p->session.selectedLayerIDs, QSet<QString>({a, b}));
        QCOMPARE(tree->selectedItems().size(), 2);
        p->history.redo();
        QCOMPARE(p->document.layers.size(), 1);
    }
    void pasteAcrossProjectsAndDropOnTabAreCopies() {
        EditorWindow window;
        auto target = page(window);
        auto source = Document::create({8, 8});
        auto a = add(source, Qt::red, {0, 0, 4, 4});
        LayerTransferMimeData mime(source, {a});
        auto bar = window.findChild<QTabWidget *>()->tabBar();
        const auto position = bar->tabRect(0).center();
        QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(target->document.layers.size(), 1);
        QCOMPARE(source.layers.size(), 1);
        QVERIFY(target->document.activeId() != a);
        target->history.undo();
        QCOMPARE(target->document.layers.size(), 0);
        QApplication::clipboard()->setMimeData(new LayerTransferMimeData(source, {a}));
        action(window, "Paste Image")->trigger();
        QCOMPARE(target->document.layers.size(), 1);
    }
    void cropDraftMovesResizesCommitsAndUndoRestoresSelection() {
        EditorWindow window;
        auto p = page(window);
        p->edit("Fixture", [](Document &d) { resizeCanvas(d, {100, 100}, 0); });
        p->history.clear();
        p->canvas->resize(400, 400);
        p->canvas->zoom = 1;
        p->canvas->setTool(Tool::Crop);
        p->session.selection = QImage(100, 100, QImage::Format_Grayscale8);
        p->session.selection.fill(128);
        const auto before = p->session.selection;
        const QPoint offset(150, 150);
        QTest::mousePress(p->canvas, Qt::LeftButton, Qt::ControlModifier, offset + QPoint(10, 10));
        QTest::mouseMove(p->canvas, offset + QPoint(60, 60));
        QTest::mouseRelease(p->canvas, Qt::LeftButton, Qt::ControlModifier,
                            offset + QPoint(60, 60));
        QCOMPARE(p->session.cropFrame, QRectF(10, 10, 50, 50));
        QCOMPARE(p->history.count(), 0);
        QTest::mousePress(p->canvas, Qt::LeftButton, Qt::ControlModifier, offset + QPoint(30, 30));
        QTest::mouseRelease(p->canvas, Qt::LeftButton, Qt::ControlModifier,
                            offset + QPoint(40, 40));
        QCOMPARE(p->session.cropFrame, QRectF(20, 20, 50, 50));
        QTest::mousePress(p->canvas, Qt::LeftButton, Qt::ControlModifier, offset + QPoint(70, 70));
        QTest::mouseRelease(p->canvas, Qt::LeftButton, Qt::ControlModifier,
                            offset + QPoint(80, 80));
        QCOMPARE(p->session.cropFrame, QRectF(20, 20, 60, 60));
        QTest::keyClick(p->canvas, Qt::Key_Return);
        QCOMPARE(p->document.size(), QSize(60, 60));
        QCOMPARE(p->history.count(), 1);
        QVERIFY(p->session.selection.isNull());
        p->history.undo();
        QCOMPARE(p->document.size(), QSize(100, 100));
        QCOMPARE(p->session.selection, before);
        p->session.cropFrame = QRectF(1, 1, 10, 10);
        QTest::keyClick(p->canvas, Qt::Key_Escape);
        QVERIFY(p->session.cropFrame.isEmpty());
    }
    void canvasSizeDialogAnchorsAndCancel() {
        EditorWindow window;
        auto p = page(window);
        auto a = add(p->document, Qt::red, {0, 0, 4, 4});
        p->changed();
        QTimer::singleShot(0, [] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QDoubleSpinBox *>("sizeWidth")->setValue(1210);
            dialog->findChild<QDoubleSpinBox *>("sizeHeight")->setValue(810);
            dialog->findChild<QRadioButton *>("canvasAnchor8")->setChecked(true);
            dialog->accept();
        });
        action(window, "Canvas Size…")->trigger();
        QCOMPARE(p->document.size(), QSize(1210, 810));
        QCOMPARE(p->document.find(a)->transform().value("origin").toArray(), QJsonArray({10, 10}));
        QCOMPARE(p->history.count(), 1);
        QTimer::singleShot(
            0, [] { qobject_cast<QDialog *>(QApplication::activeModalWidget())->reject(); });
        action(window, "Canvas Size…")->trigger();
        QCOMPARE(p->history.count(), 1);
        p->history.undo();
        QCOMPARE(p->document.size(), QSize(1200, 800));
    }
    void imageSizeResolutionOnlyAndCanvasRelativePercent() {
        EditorWindow window;
        auto p = page(window);
        auto a = add(p->document, Qt::red, {0, 0, 4, 4});
        p->changed();
        const auto key = p->document.find(a)->image.cacheKey();
        QTimer::singleShot(0, [] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QCheckBox *>("imageResample")->setChecked(false);
            dialog->findChild<QDoubleSpinBox *>("sizeResolution")->setValue(300);
            dialog->accept();
        });
        action(window, "Image Size…")->trigger();
        QCOMPARE(p->document.size(), QSize(1200, 800));
        QCOMPARE(p->document.metadata.value("resolution").toInt(), 300);
        QCOMPARE(p->document.find(a)->image.cacheKey(), key);
        p->history.undo();
        QCOMPARE(p->document.metadata.value("resolution").toInt(), 72);
        QTimer::singleShot(0, [] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QComboBox *>("sizeUnits")->setCurrentIndex(1);
            dialog->findChild<QCheckBox *>("sizeRelative")->setChecked(true);
            dialog->findChild<QDoubleSpinBox *>("sizeWidth")->setValue(10);
            dialog->findChild<QDoubleSpinBox *>("sizeHeight")->setValue(-5);
            dialog->accept();
        });
        action(window, "Canvas Size…")->trigger();
        QCOMPARE(p->document.size(), QSize(1320, 760));
        QCOMPARE(p->document.find(a)->transform().value("origin").toArray(), QJsonArray({60, -20}));
        p->history.undo();
        QCOMPARE(p->document.size(), QSize(1200, 800));
    }
    void cropRatioCenteredGestureAndFocusCancellation() {
        auto d = Document::create({100, 100});
        EditorPage p(d);
        p.canvas->resize(400, 400);
        p.canvas->zoom = 1;
        p.canvas->setTool(Tool::Crop);
        p.session.cropRatio = 9.0 / 16;
        const QPoint offset(150, 150);
        QTest::mousePress(p.canvas, Qt::LeftButton, Qt::AltModifier | Qt::ControlModifier,
                          offset + QPoint(50, 50));
        QTest::mouseRelease(p.canvas, Qt::LeftButton, Qt::AltModifier | Qt::ControlModifier,
                            offset + QPoint(59, 66));
        QCOMPARE(p.session.cropFrame, QRectF(41, 34, 18, 32));
        auto before = p.session.cropFrame;
        QTest::mousePress(p.canvas, Qt::LeftButton, Qt::ControlModifier, offset + QPoint(50, 50));
        QTest::mouseMove(p.canvas, offset + QPoint(60, 60));
        QFocusEvent lost(QEvent::FocusOut);
        QApplication::sendEvent(p.canvas, &lost);
        QCOMPARE(p.session.cropFrame, before);
        QCOMPARE(p.history.count(), 0);
    }
    void japaneseAndChineseLayerActions() {
        EditorWindow window;
        auto p = page(window);
        auto a = add(p->document, Qt::red, {0, 0, 4, 4});
        auto b = add(p->document, Qt::blue, {0, 0, 4, 4});
        p->session.selectedLayerIDs = {a, b};
        p->changed();
        UiLanguage::instance().setLanguage("ja_JP", false);
        QCOMPARE(action(window, "Merge Down")->text(), QString::fromUtf8("レイヤーを結合"));
        QCOMPARE(action(window, "Image Size…")->text(), QString::fromUtf8("画像サイズ…"));
        UiLanguage::instance().setLanguage("zh_CN", false);
        QCOMPARE(action(window, "Merge Down")->text(), QString::fromUtf8("合并图层"));
        UiLanguage::instance().setLanguage("en", false);
        QCOMPARE(action(window, "Merge Down")->text(), QString("Merge Layers"));
    }
};
QTEST_MAIN(LayerTests)
#include "layer_tests.moc"

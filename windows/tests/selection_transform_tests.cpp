// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include "render.h"
#include "selection_float.h"
#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
using namespace compositor;
namespace {
// 64 × 64 canvas with one full-size layer: a red 16-pixel square at (8, 8) and a blue pixel at (60, 60).
Document pixelDocument() {
    auto d = Document::create({64, 64});
    QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(8, 8, 16, 16, Qt::red);
    painter.fillRect(60, 60, 1, 1, Qt::blue);
    painter.end();
    d.addImage("Pixels", image);
    return d;
}
QImage rectangleMask(QSize size, QRect rectangle) {
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = rectangle.top(); y <= rectangle.bottom(); ++y)
        for (int x = rectangle.left(); x <= rectangle.right(); ++x)
            mask.scanLine(y)[x] = 255;
    return mask;
}
int selectedPixels(const QImage &mask) {
    int total = 0;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x)
            total += mask.constScanLine(y)[x] >= 128;
    return total;
}
QRect selectedBounds(const QImage &mask) {
    QRect bounds;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x)
            if (mask.constScanLine(y)[x] >= 128)
                bounds |= QRect(x, y, 1, 1);
    return bounds;
}
void ready(EditorPage &p) {
    p.resize(320, 320);
    p.show();
    QTest::qWait(20);
    p.canvas->zoomTo(2);
    p.canvas->waitForRendering();
    p.session.view.snap = false;
}
QPoint at(EditorPage &p, double x, double y) {
    return QPoint(
        qRound((p.canvas->width() - p.document.size().width() * p.canvas->zoom) / 2 +
               x * p.canvas->zoom),
        qRound((p.canvas->height() - p.document.size().height() * p.canvas->zoom) / 2 +
               y * p.canvas->zoom));
}
void mouse(Canvas *c, QEvent::Type type, QPoint position, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent e(type, QPointF(position), QPointF(c->mapToGlobal(position)),
                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(c, &e);
}
void drag(EditorPage &p, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}) {
    mouse(p.canvas, QEvent::MouseButtonPress, at(p, from.x(), from.y()), mods);
    mouse(p.canvas, QEvent::MouseMove, at(p, to.x(), to.y()), mods);
    mouse(p.canvas, QEvent::MouseButtonRelease, at(p, to.x(), to.y()), mods);
}
QColor pixelAt(const EditorPage &p, int x, int y) {
    return p.document.active()->image.pixelColor(x, y);
}
// Where a layer's pixel (x, y) lies on the document.
QColor documentPixel(const EditorPage &p, int x, int y) {
    return renderDocument(p.document).pixelColor(x, y);
}
QAction *menuAction(EditorWindow &w, const QString &title) {
    for (auto a : w.findChildren<QAction *>())
        if (a->property("layerAction").toString() == title)
            return a;
    return nullptr;
}
EditorPage *currentPage(EditorWindow &w) {
    return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
}
} // namespace
class SelectionTransformTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void shiftingASelectionKeepsWhatStaysOnTheCanvas() {
        const auto mask = rectangleMask({16, 16}, QRect(0, 0, 8, 8));
        const auto moved = shiftSelection(mask, {4, -2});
        QCOMPARE(selectedBounds(moved), QRect(4, 0, 8, 6));
        QCOMPARE(selectedBounds(shiftSelection(mask, {-20, 0})), QRect());
        QCOMPARE(shiftSelection(mask, {0, 0}), mask);
    }
    void liftingAndMergingAgainChangesNothing() {
        auto d = pixelDocument();
        const auto source = d.activeId();
        const auto before = d.find(source)->image;
        const auto id = liftSelection(d, source, rectangleMask(d.size(), QRect(8, 8, 16, 16)), false);
        QVERIFY(!id.isEmpty());
        QCOMPARE(d.layers.size(), 2);
        QCOMPARE(d.layers.back().id(), id); // Directly above its source.
        QCOMPARE(d.find(source)->image.pixelColor(10, 10).alpha(), 0);
        QCOMPARE(d.find(source)->image.pixelColor(60, 60), QColor(Qt::blue));
        QCOMPARE(d.find(id)->image.size(), QSize(16, 16));
        mergeFloatingLayer(d, id, source);
        QCOMPARE(d.layers.size(), 1);
        QCOMPARE(d.find(source)->image, before);
        QCOMPARE(d.activeId(), source);
    }
    void liftingNothingReturnsNothing() {
        auto d = pixelDocument();
        QImage empty(d.size(), QImage::Format_Grayscale8);
        empty.fill(0);
        QVERIFY(liftSelection(d, d.activeId(), empty, false).isEmpty());
        QCOMPARE(d.layers.size(), 1);
    }
    void duplicatingLeavesTheOriginalPixels() {
        auto d = pixelDocument();
        const auto source = d.activeId();
        const auto id = liftSelection(d, source, rectangleMask(d.size(), QRect(8, 8, 16, 16)), true);
        QCOMPARE(d.find(source)->image.pixelColor(10, 10), QColor(Qt::red));
        d.find(id)->move({20, 0});
        mergeFloatingLayer(d, id, source);
        QCOMPARE(d.find(source)->image.pixelColor(10, 10), QColor(Qt::red));
        QCOMPARE(d.find(source)->image.pixelColor(30, 10), QColor(Qt::red));
        QCOMPARE(d.find(source)->image.pixelColor(26, 10).alpha(), 0);
    }
    void mergingPastTheEdgeGrowsTheLayerInsteadOfCuttingOff() {
        auto d = pixelDocument();
        const auto source = d.activeId();
        // The blue pixel and its neighbors move 20 pixels past the right edge.
        const auto id = liftSelection(d, source, rectangleMask(d.size(), QRect(56, 56, 8, 8)), false);
        d.find(id)->move({20, 0});
        mergeFloatingLayer(d, id, source);
        const auto &layer = *d.find(source);
        QCOMPARE(layer.image.size(), QSize(84, 64));
        QCOMPARE(layer.transform()["size"].toArray()[0].toDouble(), 84.0);
        QCOMPARE(layer.image.pixelColor(80, 60), QColor(Qt::blue));
        // Nothing else moved: the red square is where it was on the document.
        QCOMPARE(renderDocument(d).pixelColor(10, 10), QColor(Qt::red));
    }
    void aTransformedMaskDescribesTheMovedSelection() {
        auto d = pixelDocument();
        const auto source = d.activeId();
        const auto id = liftSelection(d, source, rectangleMask(d.size(), QRect(8, 8, 16, 16)), false);
        auto &floating = *d.find(id);
        auto t = floating.transform();
        t["size"] = QJsonArray{32.0, 32.0}; // Doubled about its top-left corner.
        floating.metadata["transform"] = t;
        const auto selection = floatingSelection(d, floating);
        QCOMPARE(selectedBounds(selection), QRect(8, 8, 32, 32));
        QCOMPARE(selectedPixels(selection), 32 * 32);
        floating.move({10, 5});
        QCOMPARE(selectedBounds(floatingSelection(d, floating)), QRect(18, 13, 32, 32));
    }

    void draggingInsideASelectionMovesItsOutlineAlone() {
        EditorPage p(pixelDocument());
        ready(p);
        const auto pixels = p.document.active()->image;
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::RectangleSelect);
        drag(p, {16, 16}, {26, 20});
        QCOMPARE(selectedBounds(p.session.selection), QRect(18, 12, 16, 16));
        QCOMPARE(p.document.active()->image, pixels);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Move Selection"));
        QVERIFY(!p.isModified());
        p.history.undo();
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
    }
    void anOutlineMovedOffTheCanvasComesBackWhole() {
        // As on the Mac, the outline is not cut off at the canvas: moved back, it is whole again.
        EditorPage p(pixelDocument());
        ready(p);
        const auto original = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.session.selection = original;
        p.canvas->setTool(Tool::RectangleSelect);
        // 20 pixels left: only columns 0 to 3 of it stay on the canvas.
        drag(p, {20, 16}, {0, 16});
        QCOMPARE(selectedBounds(p.session.selection), QRect(0, 8, 4, 16));
        // Grabbed by what is left of it and dragged back.
        drag(p, {2, 16}, {22, 16});
        QCOMPARE(p.session.selection, original);
        // Undo and Redo pass through the same states, each whole.
        p.history.undo();
        QCOMPARE(selectedBounds(p.session.selection), QRect(0, 8, 4, 16));
        p.history.undo();
        QCOMPARE(p.session.selection, original);
        p.history.redo();
        p.history.redo();
        QCOMPARE(p.session.selection, original);
        // Arrow nudges keep it too: ten pixels at a time, off the top and back.
        p.canvas->setFocus();
        for (int i = 0; i < 2; ++i)
            QTest::keyClick(p.canvas, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 0, 16, 4));
        for (int i = 0; i < 2; ++i)
            QTest::keyClick(p.canvas, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(p.session.selection, original);
    }
    void escapeDuringAnOutlineDragPutsTheSelectionBack() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::EllipseSelect);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 16, 16));
        mouse(p.canvas, QEvent::MouseMove, at(p, 30, 30));
        QCOMPARE(selectedBounds(p.session.selection), QRect(22, 22, 16, 16));
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
        QCOMPARE(p.history.count(), 0);
    }
    void clickingInsideASelectionDeselectsOrReselectsWithTheWand() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::RectangleSelect);
        drag(p, {16, 16}, {16, 16});
        QVERIFY(p.session.selection.isNull());
        QCOMPARE(p.history.undoText(), QString("Deselect"));
        p.history.undo();
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
        // The wand selects afresh from the clicked pixel: the red square, not the whole canvas.
        p.canvas->setTool(Tool::Wand);
        p.session.wandTolerance = 8;
        p.session.selection = rectangleMask(p.document.size(), QRect(0, 0, 40, 40));
        drag(p, {16, 16}, {16, 16});
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
        QCOMPARE(p.history.undoText(), QString("Magic Wand Selection"));
    }
    void draggingOutsideASelectionStillMakesANewOne() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::RectangleSelect);
        drag(p, {30, 30}, {40, 40});
        QCOMPARE(selectedBounds(p.session.selection), QRect(30, 30, 10, 10));
    }
    void ctrlDraggingMovesTheSelectedPixels() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::RectangleSelect);
        const auto before = p.document.manifest();
        drag(p, {16, 16}, {31, 26}, Qt::ControlModifier);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Move Selected Pixels"));
        QCOMPARE(p.document.layers.size(), 1);
        QCOMPARE(pixelAt(p, 10, 10).alpha(), 0);
        QCOMPARE(pixelAt(p, 25, 20), QColor(Qt::red));
        QCOMPARE(selectedBounds(p.session.selection), QRect(23, 18, 16, 16));
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(pixelAt(p, 10, 10), QColor(Qt::red));
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
    }
    void ctrlAltDraggingDuplicatesThem() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::Lasso);
        drag(p, {16, 16}, {36, 16}, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(p.history.undoText(), QString("Duplicate Selected Pixels"));
        QCOMPARE(pixelAt(p, 10, 10), QColor(Qt::red));
        QCOMPARE(pixelAt(p, 30, 10), QColor(Qt::red));
        QCOMPARE(selectedBounds(p.session.selection), QRect(28, 8, 16, 16));
    }
    void ctrlDragLetGoWhereGrabbedChangesNothing() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::RectangleSelect);
        const auto before = p.document.manifest();
        drag(p, {16, 16}, {16, 16}, Qt::ControlModifier);
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.layers.size(), 1);
    }
    void escapeDuringAPixelDragRestoresTheLayer() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->setTool(Tool::RectangleSelect);
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 16, 16), Qt::ControlModifier);
        mouse(p.canvas, QEvent::MouseMove, at(p, 30, 30), Qt::ControlModifier);
        QCOMPARE(p.document.layers.size(), 2); // The pixels float while dragged.
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.layers.size(), 1);
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
    }
    void ctrlArrowMovesPixelsPastTheLayerEdgeWithoutLosingThem() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(52, 56, 12, 8));
        p.canvas->setFocus();
        QTest::keyClick(p.canvas, Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Move Selected Pixels"));
        // Ten pixels right: the layer grew instead of cutting the blue pixel off.
        QCOMPARE(p.document.active()->image.width(), 74);
        QCOMPARE(pixelAt(p, 70, 60), QColor(Qt::blue));
        QCOMPARE(pixelAt(p, 60, 60).alpha(), 0);
        QCOMPARE(selectedBounds(p.session.selection), QRect(62, 56, 2, 8));
        p.history.undo();
        QCOMPARE(p.document.active()->image.width(), 64);
        QCOMPARE(pixelAt(p, 60, 60), QColor(Qt::blue));
    }

    void controlTFloatsTheSelectedPixels() {
        EditorPage p(pixelDocument());
        ready(p);
        QVERIFY(!p.canvas->canTransformSelection());
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        QVERIFY(p.canvas->canTransformSelection());
        const auto source = p.document.activeId();
        p.canvas->beginSelectionTransform();
        QVERIFY(p.canvas->hasFloatingSelection());
        QCOMPARE(p.document.layers.size(), 2);
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.active()->name(), QString("Floating Selection"));
        QCOMPARE(p.session.tool, Tool::Move);
        // The picture looks as it did while the pixels float.
        QCOMPARE(documentPixel(p, 10, 10), QColor(Qt::red));
        QVERIFY(!p.canvas->canTransformSelection());
        p.canvas->commitFloatingSelection();
        QVERIFY(!p.canvas->hasFloatingSelection());
        QCOMPARE(p.document.activeId(), source);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Transform Selection"));
        QCOMPARE(pixelAt(p, 10, 10), QColor(Qt::red));
        QCOMPARE(selectedBounds(p.session.selection), QRect(8, 8, 16, 16));
    }
    void scalingTheFloatingSelectionIsOneUndoStep() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        const auto selection = p.session.selection;
        p.canvas->beginSelectionTransform();
        // Two gestures: the bottom-right handle out to double size, then the box moved.
        drag(p, {24, 24}, {40, 40});
        drag(p, {20, 20}, {22, 21});
        QCOMPARE(p.history.count(), 0);
        QVERIFY(p.canvas->hasFloatingSelection());
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QVERIFY(!p.canvas->hasFloatingSelection());
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Transform Selection"));
        QCOMPARE(p.document.layers.size(), 1);
        QCOMPARE(pixelAt(p, 8, 8).alpha(), 0);
        QCOMPARE(pixelAt(p, 30, 30), QColor(Qt::red));
        QCOMPARE(pixelAt(p, 36, 36), QColor(Qt::red));
        QCOMPARE(pixelAt(p, 45, 45).alpha(), 0);
        // The selection went with the pixels.
        const auto bounds = selectedBounds(p.session.selection);
        QCOMPARE(bounds, QRect(10, 9, 32, 32));
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.session.selection, selection);
        p.history.redo();
        QCOMPARE(selectedBounds(p.session.selection), QRect(10, 9, 32, 32));
    }
    void escapeRestoresTheDocumentExactly() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        const auto selection = p.session.selection;
        p.canvas->beginSelectionTransform();
        drag(p, {24, 24}, {40, 40});
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QVERIFY(!p.canvas->hasFloatingSelection());
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.layers.size(), 1);
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.session.selection, selection);
        QVERIFY(!p.isModified());
    }
    void escapeDuringAHandleDragKeepsTheFloatingSelection() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->beginSelectionTransform();
        const auto box = p.document.active()->transform();
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 24, 24));
        mouse(p.canvas, QEvent::MouseMove, at(p, 40, 40));
        QVERIFY(p.document.active()->transform() != box);
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        // Only the gesture was abandoned.
        QVERIFY(p.canvas->hasFloatingSelection());
        QCOMPARE(p.document.active()->transform(), box);
        QCOMPARE(p.document.layers.size(), 2);
    }
    void editsAndToolChangesMergeTheFloatingSelectionFirst() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->beginSelectionTransform();
        drag(p, {20, 20}, {25, 20});
        p.canvas->setTool(Tool::Brush);
        QVERIFY(!p.canvas->hasFloatingSelection());
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(pixelAt(p, 14, 10), QColor(Qt::red));
        p.canvas->setTool(Tool::RectangleSelect);
        p.canvas->setTool(Tool::Move);
        p.canvas->beginSelectionTransform();
        p.edit("Rename", [](Document &d) { d.active()->metadata["name"] = "Renamed"; });
        QVERIFY(!p.canvas->hasFloatingSelection());
        QCOMPARE(p.history.count(), 3);
        QCOMPARE(p.history.undoText(), QString("Rename"));
        QCOMPARE(p.document.layers.size(), 1);
    }
    void selectionLeftOfTheCanvasCanBeDropped() {
        EditorPage p(pixelDocument());
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        p.canvas->beginSelectionTransform();
        p.document.find(p.document.activeId())->move({-100, 0});
        p.canvas->commitFloatingSelection();
        QVERIFY(p.session.selection.isNull());
        QCOMPARE(p.document.layers.size(), 1);
    }
    void menuCommandsFloatTheSelectionAndUndoAbandonsIt() {
        EditorWindow w;
        QTemporaryDir dir;
        saveProject(pixelDocument(), dir.path() + "/Float.comp");
        w.openPath(dir.path() + "/Float.comp");
        auto p = currentPage(w);
        w.show();
        QTest::qWait(20);
        // Without a selection Ctrl+T is still the plain layer transform.
        menuAction(w, "Transform Layer")->trigger();
        QVERIFY(!p->canvas->hasFloatingSelection());
        p->session.selection = rectangleMask(p->document.size(), QRect(8, 8, 16, 16));
        menuAction(w, "Transform Layer")->trigger();
        QVERIFY(p->canvas->hasFloatingSelection());
        menuAction(w, "Undo")->trigger();
        QVERIFY(!p->canvas->hasFloatingSelection());
        QCOMPARE(p->history.count(), 0);
        QCOMPARE(p->document.layers.size(), 1);
        QCOMPARE(selectedBounds(p->session.selection), QRect(8, 8, 16, 16));
        menuAction(w, "Transform Selected Pixels")->trigger();
        QVERIFY(p->canvas->hasFloatingSelection());
        // Deselecting merges the floating pixels first, then deselects: two undo steps.
        menuAction(w, "Deselect")->trigger();
        QVERIFY(!p->canvas->hasFloatingSelection());
        QVERIFY(p->session.selection.isNull());
        QCOMPARE(p->history.count(), 2);
        QCOMPARE(p->document.layers.size(), 1);
    }
    void aSelectionTransformNeedsAPixelLayer() {
        auto d = pixelDocument();
        d.addGroup("Folder");
        EditorPage p(d);
        ready(p);
        p.session.selection = rectangleMask(p.document.size(), QRect(8, 8, 16, 16));
        QVERIFY(!p.canvas->canTransformSelection());
        QVERIFY_EXCEPTION_THROWN(p.canvas->beginSelectionTransform(), std::exception);
        QCOMPARE(p.history.count(), 0);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("SelectionTransformTests");
    SelectionTransformTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "selection_transform_tests.moc"

// SPDX-License-Identifier: MIT
#include "canvas_layout.h"
#include "demo.h"
#include "editor.h"
#include "language.h"
#include "live_dialog.h"
#include "paint_surface.h"
#include "render.h"
#include "shortcuts.h"
#include "selection_operations.h"
#include <QScopeGuard>
#include <QSpinBox>
#include <QInputDialog>
#include <QProgressDialog>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <algorithm>
using namespace compositor;
namespace {
Document smallDocument() {
    auto d = Document::create({64, 64});
    QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::red);
    d.addImage("Layer", image);
    return d;
}
Document patchDocument() {
    auto d = Document::create({64, 64});
    QImage image(8, 8, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::red);
    auto id = d.addImage("Patch", image);
    d.find(id)->setBounds({28, 28, 8, 8});
    return d;
}
void ready(EditorPage &p) {
    p.resize(320, 320);
    p.show();
    QTest::qWait(20);
    p.canvas->zoomTo(1);
    p.canvas->waitForRendering();
}
QPoint at(EditorPage &p, double x, double y) {
    const double inset = p.session.view.rulers ? 24 : 0;
    return QPoint(
        qRound((p.canvas->width() + inset - p.document.size().width() * p.canvas->zoom) / 2 +
               x * p.canvas->zoom),
        qRound((p.canvas->height() + inset - p.document.size().height() * p.canvas->zoom) / 2 +
               y * p.canvas->zoom));
}
void mouse(Canvas *c, QEvent::Type type, QPoint position, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent e(type, QPointF(position), QPointF(c->mapToGlobal(position)),
                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(c, &e);
}
QAction *action(EditorWindow &w, const QString &title) {
    for (auto a : w.findChildren<QAction *>())
        if (a->property("layerAction").toString() == title)
            return a;
    return nullptr;
}
EditorPage *page(EditorWindow &w) {
    return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
}
void tool(EditorWindow &w, Tool t) {
    for (auto a : w.findChildren<QAction *>())
        if (a->property("canvasTool").isValid() && a->property("canvasTool").toInt() == int(t)) {
            a->trigger();
            return;
        }
}
QMap<QString, QKeySequence> assignments() {
    QMap<QString, QKeySequence> values;
    for (const auto &e : Shortcuts::instance().entries())
        values[e.id] = Shortcuts::instance().sequence(e);
    return values;
}
} // namespace
class CanvasTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QSettings().remove("canvasView");
        UiLanguage::instance().setLanguage("en", false);
        QMap<QString, QKeySequence> defaults;
        for (const auto &e : Shortcuts::instance().entries())
            defaults[e.id] = e.original;
        QVERIFY(Shortcuts::instance().save(defaults));
    }
    void polygonLassoCommitUndoAndCancel() {
        EditorPage p(smallDocument());
        ready(p);
        p.canvas->setTool(Tool::Lasso);
        p.session.polygonalLasso = true;
        p.session.selectionAntialiased = false;
        for (auto point : {QPoint(10, 10), QPoint(40, 10), QPoint(10, 40)})
            QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, point.x(), point.y()));
        QCOMPARE(p.history.count(), 0);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.session.selection.constScanLine(15)[15], uchar(255));
        QCOMPARE(p.session.selection.constScanLine(45)[45], uchar(0));
        QVERIFY(!p.isModified());
        auto result = p.session.selection;
        p.history.undo();
        QVERIFY(p.session.selection.isNull());
        p.history.redo();
        QCOMPARE(p.session.selection, result);
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 4, 4));
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 20, 4));
        QTest::keyClick(p.canvas, Qt::Key_Backspace);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QCOMPARE(p.history.count(), 1);
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.session.selection, result);
        QCOMPARE(p.history.count(), 1);
    }
    void polygonLassoCloseAndModes() {
        EditorPage p(smallDocument());
        ready(p);
        p.canvas->setTool(Tool::Lasso);
        QTest::keyClick(p.canvas, Qt::Key_Tab);
        QVERIFY(p.session.polygonalLasso);
        p.session.selectionAntialiased = false;
        const auto polygon = [&](int x, int mode, bool doubleClick) {
            p.session.selectionMode = mode;
            for (auto point : {QPoint(x, 10), QPoint(x + 10, 10), QPoint(x + 10, 30), QPoint(x, 30)})
                QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, point.x(), point.y()));
            if (doubleClick)
                QTest::mouseDClick(p.canvas, Qt::LeftButton, {}, at(p, x, 30));
            else
                QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, x, 10));
        };
        polygon(10, 0, false);
        QCOMPARE(p.session.selection.constScanLine(20)[15], uchar(255));
        polygon(30, 1, true);
        QCOMPARE(p.session.selection.constScanLine(20)[15], uchar(255));
        QCOMPARE(p.session.selection.constScanLine(20)[35], uchar(255));
        polygon(10, 2, false);
        QCOMPARE(p.session.selection.constScanLine(20)[15], uchar(0));
        polygon(30, 3, false);
        QCOMPARE(p.session.selection.constScanLine(20)[35], uchar(255));
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 5, 5));
        QTest::keyClick(p.canvas, Qt::Key_Tab);
        QVERIFY(!p.session.polygonalLasso);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QCOMPARE(p.history.count(), 3); // Intersect was identical.
    }
    void selectionDiskMorphology() {
        QImage mask(9, 9, QImage::Format_Grayscale8);
        mask.fill(0);
        mask.scanLine(4)[4] = 128;
        auto disk = resizeSelectionMask(mask, 2, true);
        QCOMPARE(disk.constScanLine(4)[6], uchar(128));
        QCOMPARE(disk.constScanLine(5)[5], uchar(128));
        QCOMPARE(disk.constScanLine(6)[6], uchar(0));
        auto contracted = resizeSelectionMask(disk, 2, false);
        QCOMPARE(contracted, mask);
        mask.fill(255);
        auto edges = resizeSelectionMask(mask, 2, false);
        QCOMPARE(edges.constScanLine(0)[4], uchar(0));
        QCOMPARE(edges.constScanLine(4)[4], uchar(255));
        QCOMPARE(resizeSelectionMask(mask, 0, false), mask);
        std::atomic<bool> canceled = true;
        QVERIFY(resizeSelectionMask(mask, 20, false, &canceled).isNull());
        auto empty = resizeSelectionMask(mask, 20, false);
        QCOMPARE(empty.constScanLine(4)[4], uchar(0));
        QVERIFY_EXCEPTION_THROWN(resizeSelectionMask(mask, -1, true), std::runtime_error);
    }
    void selectionMorphologyUndoAndEmpty() {
        EditorPage p(smallDocument());
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(0);
        mask.scanLine(32)[32] = 255;
        p.session.selection = mask;
        p.canvas->resizeSelection(3, true);
        QCOMPARE(p.canvas->selectionBounds(), QRect(29, 29, 7, 7));
        p.history.undo();
        QCOMPARE(p.session.selection, mask);
        p.history.redo();
        p.canvas->resizeSelection(20, false);
        QVERIFY(!p.session.selection.isNull()); // Explicitly empty is distinct from deselect.
        QVERIFY(p.canvas->selectionBounds().isEmpty());
        QVERIFY(!p.isModified());
    }
    void maskFromSelectionPlacementAndUndo() {
        EditorWindow w;
        auto p = page(w);
        p->document = smallDocument();
        auto transform = p->document.active()->transform();
        transform["rotation"] = 90;
        p->document.active()->metadata["transform"] = transform;
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 10; y < 30; ++y)
            for (int x = 10; x < 30; ++x)
                mask.scanLine(y)[x] = 128;
        p->session.selection = mask;
        auto a = action(w, "Mask from Selection");
        QVERIFY(a);
        a->trigger();
        QCOMPARE(p->document.active()->mask, mask);
        QVERIFY(p->isModified());
        const auto rendered = renderDocument(p->document);
        QCOMPARE(rendered.pixelColor(15, 15).alpha(), 128);
        QCOMPARE(rendered.pixelColor(40, 40).alpha(), 0);
        p->history.undo();
        QVERIFY(p->document.active()->mask.isNull());
        QCOMPARE(p->session.selection, mask);
        QVERIFY(!p->isModified());
        p->history.redo();
        QCOMPARE(p->document.active()->mask, mask);
        QTemporaryDir folder;
        const auto path = folder.filePath("mask.comp");
        saveProject(p->document, path);
        auto loaded = loadProject(path);
        QCOMPARE(renderDocument(loaded), rendered);
    }
    void maskFromSelectionUpgradesOlderProjects() {
        EditorWindow w;
        auto p = page(w);
        p->document = smallDocument();
        // Masks need format version 4, and a folder's version 6.
        p->document.metadata["version"] = 3;
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(255);
        p->session.selection = mask;
        int warnings = 0;
        QTimer closer;
        QObject::connect(&closer, &QTimer::timeout, [&] {
            if (auto *box = activeLiveDialog()) {
                ++warnings;
                box->close();
            }
        });
        closer.start(10);
        action(w, "Mask from Selection")->trigger();
        QCOMPARE(warnings, 0);
        QVERIFY(!p->document.active()->mask.isNull());
        QCOMPARE(p->document.metadata["version"].toInt(), CurrentVersion);
        QTemporaryDir folder;
        saveProject(p->document, folder.filePath("old.comp"));
        QVERIFY(!loadProject(folder.filePath("old.comp")).active()->mask.isNull());
    }
    void colorRangePremultipliedAndExclusions() {
        QImage image(5, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        image.setPixelColor(0, 0, QColor(255, 0, 0, 128));
        image.setPixelColor(1, 0, QColor(250, 5, 0));
        image.setPixelColor(2, 0, Qt::blue);
        auto exact = colorRangeMask(image, {Qt::red}, {}, 0, false);
        QCOMPARE(exact.constScanLine(0)[0], uchar(255));
        QCOMPARE(exact.constScanLine(0)[1], uchar(0));
        auto included = colorRangeMask(image, {Qt::red, Qt::blue}, {Qt::blue}, 5, false);
        QCOMPARE(included.constScanLine(0)[1], uchar(255));
        QCOMPARE(included.constScanLine(0)[2], uchar(0));
        QCOMPARE(included.constScanLine(0)[3], uchar(0));
        auto inverse = colorRangeMask(image, {Qt::red}, {}, 5, true);
        QCOMPARE(inverse.constScanLine(0)[0], uchar(0));
        QCOMPARE(inverse.constScanLine(0)[2], uchar(255));
        QCOMPARE(inverse.constScanLine(0)[3], uchar(255));
    }
    void colorRangeDialogCommitAndCancel_data() {
        QTest::addColumn<bool>("accept");
        QTest::newRow("accept") << true;
        QTest::newRow("cancel") << false;
    }
    void colorRangeDialogCommitAndCancel() {
        QFETCH(bool, accept);
        EditorWindow w;
        auto p = page(w);
        p->document = smallDocument();
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                p->document.active()->image.setPixelColor(x, y, Qt::blue);
        p->canvas->refresh();
        p->canvas->selectAll();
        auto original = p->session.selection;
        const auto count = p->history.count();
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("colorRangeDialog");
            QVERIFY(dialog);
            auto close = qScopeGuard([&] { dialog->reject(); });
            auto source = dialog->findChild<QWidget *>("colorRangeSource");
            auto spin = dialog->findChild<QSpinBox *>("colorRangeFuzziness");
            auto buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(source && spin && buttons);
            const int side = std::min(source->width(), source->height());
            QTest::mouseClick(source, Qt::LeftButton, {},
                              QPoint(source->width()/2-side/4, source->height()/2));
            QTRY_VERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
            QCOMPARE(p->session.selection.constScanLine(20)[20], uchar(255));
            QCOMPARE(p->session.selection.constScanLine(20)[40], uchar(0));
            QCOMPARE(p->history.count(), count);
            spin->setValue(10);
            spin->setValue(20);
            spin->setValue(0);
            QTRY_VERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
            close.dismiss();
            if (accept)
                buttons->button(QDialogButtonBox::Ok)->click();
            else
                dialog->reject();
        });
        action(w, "Color Range…")->trigger();
        QCOMPARE(p->history.count(), count + (accept ? 1 : 0));
        QVERIFY(!p->isModified());
        if (accept) {
            auto result = p->session.selection;
            p->history.undo();
            QCOMPARE(p->session.selection, original);
            p->history.redo();
            QCOMPARE(p->session.selection, result);
        } else
            QCOMPARE(p->session.selection, original);
    }
    void colorRangeAddSubtractInvertAndClear() {
        EditorWindow w;
        auto p = page(w);
        p->document = smallDocument();
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                p->document.active()->image.setPixelColor(x, y, Qt::blue);
        p->canvas->refresh();
        p->canvas->selectAll();
        const auto original = p->session.selection;
        const auto count = p->history.count();
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("colorRangeDialog");
            QVERIFY(dialog);
            auto close = qScopeGuard([&] { dialog->reject(); });
            auto source = dialog->findChild<QWidget *>("colorRangeSource");
            auto buttons = dialog->findChild<QDialogButtonBox *>();
            auto ok = buttons->button(QDialogButtonBox::Ok);
            auto inverse = dialog->findChild<QCheckBox *>("colorRangeInvert");
            const int side = std::min(source->width(), source->height());
            const auto pick = [&](bool blue, Qt::KeyboardModifiers mods) {
                QTest::mouseClick(source, Qt::LeftButton, mods,
                                  QPoint(source->width()/2 + (blue ? side/4 : -side/4),
                                         source->height()/2));
                QTRY_VERIFY(ok->isEnabled());
            };
            pick(false, {});
            pick(true, Qt::ShiftModifier);
            QCOMPARE(p->session.selection.constScanLine(20)[20], uchar(255));
            QCOMPARE(p->session.selection.constScanLine(20)[40], uchar(255));
            pick(false, Qt::AltModifier);
            QCOMPARE(p->session.selection.constScanLine(20)[20], uchar(0));
            QCOMPARE(p->session.selection.constScanLine(20)[40], uchar(255));
            inverse->setChecked(true);
            QTRY_VERIFY(ok->isEnabled());
            QCOMPARE(p->session.selection.constScanLine(20)[20], uchar(255));
            QCOMPARE(p->session.selection.constScanLine(20)[40], uchar(0));
            dialog->findChild<QPushButton *>("colorRangeClear")->click();
            QCOMPARE(p->session.selection, original);
            QVERIFY(!ok->isEnabled());
            // Closing with another request pending must never deliver a stale selection.
            QTest::mouseClick(source, Qt::LeftButton, {},
                              QPoint(source->width()/2-side/4, source->height()/2));
            QVERIFY(!ok->isEnabled());
        });
        action(w, "Color Range…")->trigger();
        QTest::qWait(100);
        QCOMPARE(p->session.selection, original);
        QCOMPARE(p->history.count(), count);
    }
    void selectionDialogLanguages_data() {
        QTest::addColumn<QString>("language");
        QTest::newRow("English") << QString("en");
        QTest::newRow("Chinese") << QString("zh_CN");
        QTest::newRow("Japanese") << QString("ja_JP");
    }
    void selectionDialogLanguages() {
        QFETCH(QString, language);
        UiLanguage::instance().setLanguage(language, false);
        EditorWindow w;
        auto p = page(w);
        p->document = smallDocument();
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                p->document.active()->image.setPixelColor(x, y, Qt::blue);
        p->canvas->refresh();
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("colorRangeDialog");
            QVERIFY(dialog);
            auto close = qScopeGuard([&] { dialog->reject(); });
            QCOMPARE(dialog->windowTitle(), uiText("Color Range"));
            QCOMPARE(dialog->findChild<QCheckBox *>("colorRangeInvert")->text(), uiText("Invert"));
            auto source = dialog->findChild<QWidget *>("colorRangeSource");
            const int side = std::min(source->width(), source->height());
            QTest::mouseClick(source, Qt::LeftButton, {},
                              QPoint(source->width()/2-side/4, source->height()/2));
            QTRY_VERIFY(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
            if (qEnvironmentVariableIsSet("COMPOSITOR_SELECTION_SCREENSHOTS")) {
                dialog->grab().save("artifacts/s1-s3-color-range-" + language + ".png");
                tool(w, Tool::Lasso);
                w.grab().save("artifacts/s1-s3-editor-" + language + ".png");
            }
        });
        action(w, "Color Range…")->trigger();
    }
    void resizeSelectionDialogCancel() {
        EditorWindow w;
        auto p = page(w);
        p->document = Document::create({1024, 1024});
        p->canvas->refresh();
        p->canvas->selectAll();
        const auto original = p->session.selection;
        const auto count = p->history.count();
        QTimer::singleShot(0, &w, [&] {
            auto input = w.findChild<QInputDialog *>();
            QVERIFY(input);
            input->setIntValue(200);
            QTimer::singleShot(0, &w, [&] {
                auto progress = w.findChild<QProgressDialog *>("resizeSelectionProgress");
                QVERIFY(progress);
                progress->cancel();
                progress->reject();
            });
            input->accept();
        });
        action(w, "Contract…")->trigger();
        QTest::qWait(50);
        QCOMPARE(p->session.selection, original);
        QCOMPARE(p->history.count(), count);
        QVERIFY(!p->isModified());
    }
    void folderMaskFromSelection() {
        EditorWindow w;
        auto p = page(w);
        p->document = smallDocument();
        const auto child = p->document.activeId();
        const auto folder = p->document.addGroup("Folder");
        p->document.find(child)->metadata["parentID"] = folder;
        p->document.metadata["activeLayerID"] = folder;
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x)
                mask.scanLine(y)[x] = 255;
        p->session.selection = mask;
        action(w, "Mask from Selection")->trigger();
        auto rendered = renderDocument(p->document);
        QCOMPARE(rendered.pixelColor(20, 20).alpha(), 255);
        QCOMPARE(rendered.pixelColor(40, 20).alpha(), 0);
        p->history.undo();
        QVERIFY(p->document.find(folder)->mask.isNull());
    }
    void brushKeysAndPalette() {
        EditorPage p(smallDocument());
        ready(p);
        p.canvas->setTool(Tool::Brush);
        QTest::keyClick(p.canvas, Qt::Key_BracketRight);
        QCOMPARE(p.session.brushSize, 37.0);
        QTest::keyClick(p.canvas, Qt::Key_BracketLeft, Qt::ShiftModifier);
        QVERIFY(std::abs(p.session.hardness - .7) < .001);
        QTest::keyClick(p.canvas, Qt::Key_2);
        QTest::keyClick(p.canvas, Qt::Key_5);
        QCOMPARE(p.session.brushOpacity, .25);
        QTest::keyClick(p.canvas, Qt::Key_D);
        QCOMPARE(p.session.foreground, QColor(Qt::black));
        QTest::keyClick(p.canvas, Qt::Key_X);
        QCOMPARE(p.session.foreground, QColor(Qt::white));
        QTest::keyClick(p.canvas, Qt::Key_Tab);
        QCOMPARE(p.session.tool, Tool::Erase);
        QVERIFY(!p.isModified());
    }
    void nudgeUndoAndBlend() {
        EditorPage p(smallDocument());
        ready(p);
        auto before = p.document.active()->metadata;
        QTest::keyClick(p.canvas, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(p.document.active()->transform()["origin"].toArray()[0].toDouble(), 10.0);
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        QCOMPARE(p.document.active()->metadata, before);
        QTest::keyClick(p.canvas, Qt::Key_Equal, Qt::ShiftModifier);
        QVERIFY(p.document.active()->blend() != "Normal");
        p.history.undo();
        QCOMPARE(p.document.active()->blend(), QString("Normal"));
    }
    void moveSelectionAndPixels() {
        auto d = smallDocument();
        d.active()->image.fill(Qt::transparent);
        d.active()->image.setPixelColor(20, 20, Qt::red);
        EditorPage p(d);
        ready(p);
        p.session.selection = QImage(64, 64, QImage::Format_Grayscale8);
        p.session.selection.fill(0);
        p.session.selection.scanLine(20)[20] = 255;
        auto selected = p.session.selection;
        p.canvas->setTool(Tool::RectangleSelect);
        QTest::keyClick(p.canvas, Qt::Key_Right);
        QCOMPARE(p.session.selection.constScanLine(20)[21], uchar(255));
        QCOMPARE(p.document.active()->image, d.active()->image);
        p.history.undo();
        QCOMPARE(p.session.selection, selected);
        QTest::keyClick(p.canvas, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(p.document.active()->image.pixelColor(21, 20), QColor(Qt::red));
        QCOMPARE(p.document.active()->image.pixelColor(20, 20).alpha(), 0);
        QCOMPARE(p.session.selection.constScanLine(20)[21], uchar(255));
        p.history.undo();
        QCOMPARE(p.document.active()->image, d.active()->image);
        QCOMPARE(p.session.selection, selected);
    }
    void enterCommitsAndEscapeRollsBack() {
        EditorPage p(smallDocument());
        ready(p);
        p.session.view.snap = false;
        p.session.view.transformControls = false;
        auto before = p.document.active()->metadata;
        auto center = at(p, 32, 32);
        mouse(p.canvas, QEvent::MouseButtonPress, center);
        mouse(p.canvas, QEvent::MouseMove, center + QPoint(13, 7));
        QVERIFY(p.interacting());
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.active()->metadata, before);
        QCOMPARE(p.history.count(), 0);
        mouse(p.canvas, QEvent::MouseButtonPress, center);
        mouse(p.canvas, QEvent::MouseMove, center + QPoint(13, 7));
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QVERIFY(!p.interacting());
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.document.active()->transform()["origin"].toArray()[0].toDouble(), 13.0);
    }
    void rulerGuidesUndoLockAndSave() {
        EditorPage p(smallDocument());
        ready(p);
        p.session.view.rulers = true;
        p.session.view.snap = false;
        mouse(p.canvas, QEvent::MouseButtonPress, QPoint(200, 12));
        mouse(p.canvas, QEvent::MouseMove, at(p, 30, 40));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 30, 40));
        auto guides = p.document.metadata["guides"].toArray();
        QCOMPARE(guides.size(), 1);
        QCOMPARE(guides[0].toObject()["axis"].toString(), QString("horizontal"));
        QCOMPARE(guides[0].toObject()["position"].toDouble(), 40.0);
        QTemporaryDir dir;
        saveProject(p.document, dir.path() + "/Guides.comp");
        QCOMPARE(loadProject(dir.path() + "/Guides.comp").metadata["guides"],
                 p.document.metadata["guides"]);
        QCOMPARE(CurrentVersion, 11);
        p.session.view.lockGuides = true;
        p.canvas->clearGuides();
        QCOMPARE(p.document.metadata["guides"].toArray().size(), 1);
        p.session.view.lockGuides = false;
        p.history.undo();
        QCOMPARE(p.document.metadata["guides"].toArray().size(), 0);
        mouse(p.canvas, QEvent::MouseButtonPress, QPoint(12, 200));
        mouse(p.canvas, QEvent::MouseMove, at(p, 25, 40));
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.metadata["guides"].toArray().size(), 0);
    }
    void deleteGuideByDraggingOffCanvas() {
        EditorPage p(smallDocument());
        ready(p);
        p.session.view.snap = false;
        p.canvas->addGuide(false, 25);
        auto old = p.document.metadata["guides"];
        auto start = at(p, 25, 30);
        mouse(p.canvas, QEvent::MouseButtonPress, start);
        mouse(p.canvas, QEvent::MouseMove, at(p, -20, 30));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, -20, 30));
        QCOMPARE(p.document.metadata["guides"].toArray().size(), 0);
        p.history.undo();
        QCOMPARE(p.document.metadata["guides"], old);
    }
    void snappingTargetsAndScreenTolerance() {
        auto d = smallDocument();
        CanvasViewOptions o;
        o.snapLayers = false;
        o.snapCanvas = false;
        d.metadata["guides"] = QJsonArray{QJsonObject{{"axis", "vertical"}, {"position", 23}}};
        QCOMPARE(snapCoordinate(d, o, 26, false, 1), 23.0);
        QCOMPARE(snapCoordinate(d, o, 26, false, 4), 26.0);
        o.guides = false;
        QCOMPARE(snapCoordinate(d, o, 26, false, 1), 26.0);
        o.grid = true;
        o.snapGrid = true;
        QCOMPARE(snapCoordinate(d, o, 14, false, 1), 16.0);
        o.snap = false;
        QCOMPARE(snapCoordinate(d, o, 14, false, 1), 14.0);
        EditorPage p(d);
        ready(p);
        p.session.view = o;
        p.session.view.snap = true;
        QCOMPARE(p.canvas->snapValue(14, false, Qt::ControlModifier), 14.0);
    }
    void transformHandlesPreserveSourceAndCancel() {
        EditorPage p(smallDocument());
        ready(p);
        p.session.view.snap = false;
        auto image = p.document.active()->image;
        auto before = p.document.active()->metadata;
        auto corner = at(p, 64, 64);
        mouse(p.canvas, QEvent::MouseButtonPress, corner);
        mouse(p.canvas, QEvent::MouseMove, corner + QPoint(20, 12));
        mouse(p.canvas, QEvent::MouseButtonRelease, corner + QPoint(20, 12));
        QCOMPARE(p.document.active()->transform()["size"].toArray()[0].toDouble(), 84.0);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        QCOMPARE(p.document.active()->metadata, before);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 64, 64));
        mouse(p.canvas, QEvent::MouseMove, at(p, 80, 80));
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.active()->metadata, before);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 32, -24));
        mouse(p.canvas, QEvent::MouseMove, at(p, 88, 32), Qt::ShiftModifier);
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 88, 32), Qt::ShiftModifier);
        QCOMPARE(p.document.active()->transform()["rotation"].toDouble(), 90.0);
        QCOMPARE(p.document.active()->image, image);
        p.history.undo();
        QCOMPARE(p.document.active()->metadata, before);
    }
    void relevantToolOptionsAndTabState() {
        EditorWindow w;
        w.show();
        QTest::qWait(25);
        tool(w, Tool::Clone);
        auto clone = w.findChild<QCheckBox *>("cloneAligned");
        QTRY_VERIFY(clone->isVisible());
        clone->setChecked(false);
        QVERIFY(!page(w)->session.cloneAligned);
        tool(w, Tool::Gradient);
        QTRY_VERIFY(!clone->isVisible());
        auto type = w.findChild<QComboBox *>("gradientType");
        QTRY_VERIFY(type->isVisible());
        type->setCurrentIndex(1);
        QCOMPARE(page(w)->session.gradientKind, 1);
        auto tabs = w.findChild<QTabWidget *>();
        auto first = page(w);
        action(w, "New…");
        QTemporaryDir dir;
        auto d = smallDocument();
        saveProject(d, dir.path() + "/Second.comp");
        w.openPath(dir.path() + "/Second.comp");
        tool(w, Tool::Gradient);
        QCOMPARE(page(w)->session.gradientKind, 0);
        tabs->setCurrentWidget(first);
        QCOMPARE(type->currentIndex(), 1);
        tool(w, Tool::Move);
        QVERIFY(!type->isVisible());
    }
    void maskTransformAndCenteredResize() {
        auto d = smallDocument();
        auto l = d.active();
        l->mask = QImage(64, 64, QImage::Format_Grayscale8);
        l->mask.fill(255);
        l->metadata["maskFile"] = l->id() + ".mask.png";
        EditorPage p(d);
        ready(p);
        p.session.target = EditTarget::Mask;
        p.session.view.snap = false;
        auto image = p.document.active()->image, mask = p.document.active()->mask;
        auto before = p.document.active()->transform();
        auto corner = at(p, 64, 64), end = at(p, 80, 72);
        mouse(p.canvas, QEvent::MouseButtonPress, corner);
        mouse(p.canvas, QEvent::MouseMove, end, Qt::AltModifier | Qt::ShiftModifier);
        mouse(p.canvas, QEvent::MouseButtonRelease, end, Qt::AltModifier | Qt::ShiftModifier);
        auto transform = p.document.active()->metadata["maskPlacement"].toObject();
        QCOMPARE(transform["origin"].toArray()[0].toDouble(), -16.0);
        QCOMPARE(transform["size"].toArray()[0].toDouble(), 96.0);
        QCOMPARE(transform["size"].toArray()[1].toDouble(), 96.0);
        QCOMPARE(p.document.active()->transform(), before);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.document.active()->mask, mask);
        p.history.undo();
        QVERIFY(!p.document.active()->metadata.contains("maskPlacement"));
        p.session.view.transformControls = false;
        auto center = at(p, 32, 32);
        mouse(p.canvas, QEvent::MouseButtonPress, center);
        mouse(p.canvas, QEvent::MouseMove, center + QPoint(10, 5));
        mouse(p.canvas, QEvent::MouseButtonRelease, center + QPoint(10, 5));
        QCOMPARE(p.document.active()
                     ->metadata["maskPlacement"]
                     .toObject()["origin"]
                     .toArray()[0]
                     .toDouble(),
                 10.0);
        QCOMPARE(p.document.active()->transform(), before);
        QTest::keyClick(p.canvas, Qt::Key_Right);
        QCOMPARE(p.document.active()
                     ->metadata["maskPlacement"]
                     .toObject()["origin"]
                     .toArray()[0]
                     .toDouble(),
                 11.0);
    }
    void smoothingEndsAtPointerAndCloneSampleModes() {
        auto d = smallDocument();
        d.active()->image.fill(Qt::transparent);
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 4;
        p.session.hardness = 1;
        p.session.brushSmoothing = 20;
        p.session.foreground = Qt::green;
        auto start = at(p, 12, 32), end = at(p, 48, 32);
        mouse(p.canvas, QEvent::MouseButtonPress, start);
        mouse(p.canvas, QEvent::MouseMove, start + QPoint(10, 0));
        QCOMPARE(p.document.active()->image.pixelColor(22, 32).alpha(), 0);
        mouse(p.canvas, QEvent::MouseMove, end);
        QCOMPARE(p.document.active()->image.pixelColor(48, 32).alpha(), 0);
        mouse(p.canvas, QEvent::MouseButtonRelease, end);
        QCOMPARE(p.document.active()->image.pixelColor(48, 32), QColor(Qt::green));
        QCOMPARE(p.history.count(), 1);
        QTest::mouseClick(p.canvas, Qt::LeftButton, Qt::ShiftModifier, at(p, 48, 48));
        QCOMPARE(p.document.active()->image.pixelColor(48, 40), QColor(Qt::green));
        p.history.undo();
        p.history.undo();
        QCOMPARE(p.document.active()->image, d.active()->image);
        for (bool aligned : {false, true}) {
            auto cloneDoc = smallDocument();
            cloneDoc.active()->image.fill(Qt::red);
            QPainter draw(&cloneDoc.active()->image);
            draw.fillRect(12, 12, 8, 8, Qt::green);
            draw.fillRect(28, 12, 8, 8, Qt::yellow);
            draw.end();
            EditorPage clone(cloneDoc);
            ready(clone);
            clone.canvas->setTool(Tool::Clone);
            clone.session.cloneAligned = aligned;
            clone.session.brushSize = 4;
            clone.session.hardness = 1;
            QTest::mouseClick(clone.canvas, Qt::LeftButton, Qt::AltModifier, at(clone, 16, 16));
            QTest::mouseClick(clone.canvas, Qt::LeftButton, {}, at(clone, 32, 32));
            QTest::mouseClick(clone.canvas, Qt::LeftButton, {}, at(clone, 48, 32));
            QCOMPARE(clone.document.active()->image.pixelColor(48, 32),
                     QColor(aligned ? Qt::yellow : Qt::green));
        }
        auto merged = smallDocument();
        auto target = merged.activeId();
        QImage blue(8, 8, QImage::Format_RGBA8888_Premultiplied);
        blue.fill(Qt::blue);
        auto upper = merged.addImage("Upper", blue);
        merged.find(upper)->move({12, 12});
        merged.metadata["activeLayerID"] = target;
        EditorPage clone(merged);
        ready(clone);
        clone.canvas->setTool(Tool::Clone);
        clone.session.cloneMerged = true;
        clone.session.brushSize = 4;
        clone.session.hardness = 1;
        QTest::mouseClick(clone.canvas, Qt::LeftButton, Qt::AltModifier, at(clone, 16, 16));
        QTest::mouseClick(clone.canvas, Qt::LeftButton, {}, at(clone, 40, 40));
        QCOMPARE(clone.document.active()->image.pixelColor(40, 40), QColor(Qt::blue));
    }
    void menuFillDeleteAndSelectTool() {
        EditorWindow w;
        QTemporaryDir dir;
        auto d = smallDocument();
        d.active()->mask = QImage(64, 64, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        saveProject(d, dir.path() + "/Fill.comp");
        w.openPath(dir.path() + "/Fill.comp");
        auto p = page(w);
        p->session.foreground = Qt::green;
        p->session.background = Qt::blue;
        action(w, "Fill with Background")->trigger();
        QCOMPARE(p->document.active()->image.pixelColor(32, 32), QColor(Qt::blue));
        p->history.undo();
        p->session.target = EditTarget::Mask;
        action(w, "Clear Pixels")->trigger();
        QCOMPARE(p->document.active()->mask.constScanLine(32)[32], uchar(0));
        QCOMPARE(p->document.active()->image, d.active()->image);
        p->history.undo();
        p->session.target = EditTarget::Pixels;
        tool(w, Tool::Select);
        w.show();
        QTest::qWait(20);
        p->canvas->zoomTo(1);
        QTest::mouseClick(p->canvas, Qt::LeftButton, {}, at(*p, 32, 32));
        QCOMPARE(p->session.selectedLayerIDs, QSet<QString>{d.activeId()});
        action(w, "Delete Selection / Layer")->trigger();
        QCOMPARE(p->document.layers.size(), 0);
        p->history.undo();
        QCOMPARE(p->document.layers.size(), 1);
    }
    void wandLayerSamplingAndHealingModes() {
        auto d = smallDocument();
        auto active = d.activeId();
        QImage patch(16, 16, QImage::Format_RGBA8888_Premultiplied);
        patch.fill(Qt::blue);
        auto upper = d.addImage("Patch", patch);
        d.find(upper)->move({24, 24});
        d.metadata["activeLayerID"] = active;
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Wand);
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 30, 30));
        QCOMPARE(p.session.selection.constScanLine(10)[10], uchar(0));
        p.session.wandMerged = false;
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 30, 30));
        QCOMPARE(p.session.selection.constScanLine(10)[10], uchar(255));
        for (int mode = 0; mode < 3; ++mode) {
            auto source = smallDocument();
            auto &image = source.active()->image;
            image.fill(Qt::white);
            QPainter painter(&image);
            painter.fillRect(27, 27, 10, 10, Qt::black);
            painter.end();
            EditorPage heal(source);
            ready(heal);
            heal.canvas->setTool(Tool::Heal);
            heal.session.healingMode = mode;
            heal.session.brushSize = 16;
            heal.session.hardness = 1;
            QTest::mouseClick(heal.canvas, Qt::LeftButton, {}, at(heal, 32, 32));
            QCOMPARE(heal.history.count(), 1);
            QVERIFY(heal.document.active()->image.pixelColor(32, 32).red() > 0);
            heal.history.undo();
            QCOMPARE(heal.document.active()->image, image);
        }
    }
    void paintPaddingPreservesTransformedPixelsAndMasks() {
        for (double angle : {0.0, 37.0, 90.0})
            for (bool flip : {false, true}) {
                auto d = patchDocument();
                auto l = d.active();
                auto t = l->transform();
                t["rotation"] = angle;
                t["flipX"] = flip;
                t["size"] = QJsonArray{16, 12};
                l->metadata["transform"] = t;
                l->mask = QImage(8, 8, QImage::Format_Grayscale8);
                l->mask.fill(255);
                l->mask.scanLine(2)[3] = 30;
                l->metadata["maskFile"] = l->id() + ".mask.png";
                const auto image = l->image, mask = l->mask;
                const auto old = l->placement(image.size());
                const auto offset = growPaintSurface(*l, false, {-4, -3, 20, 18});
                QCOMPARE(offset, QPoint(4, 3));
                auto next = l->placement(l->image.size());
                for (QPointF point : {QPointF(), QPointF(3.5, 2.5), QPointF(8, 8)})
                    QVERIFY(QLineF(old.map(point), next.map(point + offset)).length() < .00001);
                QCOMPARE(l->image.copy(QRect(offset, image.size())), image);
                QCOMPARE(l->mask.copy(QRect(offset, mask.size())), mask);
                QCOMPARE(l->mask.constScanLine(0)[0], uchar(255));
                d.validateAssets();
                const auto layerTransform = l->transform();
                l->metadata["maskPlacement"] = t;
                auto placed = paintTarget(*l, true).placement(l->mask.size());
                const auto moved = growPaintSurface(*l, true, {-2, -2, 24, 22});
                QCOMPARE(l->transform(), layerTransform);
                QVERIFY(QLineF(placed.map(QPointF(4, 4)),
                               paintTarget(*l, true).placement(l->mask.size()).map(
                                   QPointF(4, 4) + moved)).length() < .00001);
            }
    }
    void brushGrowsEveryEdgeAndRestoresUndoCancelAndSave() {
        auto d = patchDocument();
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 6;
        p.session.hardness = 1;
        p.session.foreground = Qt::green;
        for (QPoint point : {QPoint(20, 32), QPoint(44, 32), QPoint(32, 20), QPoint(32, 44)}) {
            QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, point.x(), point.y()));
            QCOMPARE(renderDocument(p.document).pixelColor(point), QColor(Qt::green));
            QCOMPARE(renderDocument(p.document).pixelColor(30, 30), QColor(Qt::red));
        }
        QCOMPARE(p.history.count(), 4);
        const auto grown = p.document;
        if (qEnvironmentVariableIsSet("COMPOSITOR_PAINT_SCREENSHOTS")) {
            p.canvas->zoomTo(4);
            QTest::qWait(30);
            p.canvas->waitForRendering();
            QTest::qWait(30);
            p.canvas->grab().save("artifacts/p1-p3-growing-brush.png");
            p.canvas->zoomTo(1);
        }
        QTemporaryDir dir;
        saveProject(p.document, dir.path() + "/Paint.comp");
        auto loaded = loadProject(dir.path() + "/Paint.comp");
        QCOMPARE(renderDocument(loaded), renderDocument(grown));
        for (int i = 0; i < 4; ++i)
            p.history.undo();
        QCOMPARE(p.document.active()->image, d.active()->image);
        QCOMPARE(p.document.active()->metadata, d.active()->metadata);
        for (int i = 0; i < 4; ++i)
            p.history.redo();
        QCOMPARE(p.document.active()->image, grown.active()->image);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 8, 8));
        mouse(p.canvas, QEvent::MouseMove, at(p, 56, 8));
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.active()->image, grown.active()->image);
        QCOMPARE(p.document.active()->metadata, grown.active()->metadata);
        QCOMPARE(p.history.count(), 4);
    }
    void growingStrokeKeepsOpacitySelectionAndReleaseEndpoint() {
        auto d = patchDocument();
        d.active()->image.fill(Qt::transparent);
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 4;
        p.session.hardness = 1;
        p.session.brushOpacity = .5;
        p.session.foreground = Qt::green;
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 10, 32));
        mouse(p.canvas, QEvent::MouseMove, at(p, 30, 32));
        mouse(p.canvas, QEvent::MouseMove, at(p, 50, 32));
        mouse(p.canvas, QEvent::MouseMove, at(p, 10, 32));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 54, 32));
        const auto result = renderDocument(p.document);
        for (int x = 10; x <= 54; ++x)
            QVERIFY(std::abs(result.pixelColor(x, 32).alpha() - 128) <= 1);
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        p.session.selection = QImage(64, 64, QImage::Format_Grayscale8);
        p.session.selection.fill(0);
        p.session.selection.scanLine(16)[16] = 128;
        p.session.brushOpacity = 1;
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 16, 16));
        auto selected = renderDocument(p.document);
        QCOMPARE(selected.pixelColor(16, 16).alpha(), 128);
        QCOMPARE(selected.pixelColor(17, 16).alpha(), 0);
    }
    void independentAndFolderMasksGrowWithoutMovingLayer() {
        for (bool group : {false, true}) {
            auto d = patchDocument();
            const auto id = group ? d.addGroup("Folder") : d.activeId();
            auto l = d.find(id);
            l->mask = QImage(8, 8, QImage::Format_Grayscale8);
            l->mask.fill(0);
            l->metadata["maskFile"] = id + ".mask.png";
            l->metadata["maskPlacement"] = d.layers.front().transform();
            l->metadata["maskLinked"] = false;
            if (group)
                d.layers.front().metadata["parentID"] = id;
            d.metadata["activeLayerID"] = id;
            EditorPage p(d);
            ready(p);
            p.canvas->setTool(Tool::Brush);
            p.session.target = EditTarget::Mask;
            p.session.brushSize = 6;
            p.session.hardness = 1;
            p.session.foreground = Qt::white;
            QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 16, 16));
            auto painted = paintTarget(*p.document.active(), true);
            auto pixel = painted.placement(painted.image.size()).inverted().map(QPointF(16, 16));
            QCOMPARE(painted.image.constScanLine(int(pixel.y()))[int(pixel.x())], uchar(255));
            QCOMPARE(p.document.active()->transform(), d.active()->transform());
            QCOMPARE(p.document.active()->image, d.active()->image);
            QCOMPARE(p.history.count(), 1);
            p.document.validateAssets();
            p.history.undo();
            QCOMPARE(p.document.active()->metadata, d.active()->metadata);
            QCOMPARE(p.document.active()->mask, d.active()->mask);
        }
    }
    void implicitMaskGrowthKeepsOldCoverageAndRevealsNewPaint() {
        auto d = patchDocument();
        d.active()->mask = QImage(8, 8, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        d.active()->mask.scanLine(2)[2] = 0;
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 4;
        p.session.hardness = 1;
        p.session.foreground = Qt::blue;
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 16, 16));
        auto result = renderDocument(p.document);
        QCOMPARE(result.pixelColor(16, 16), QColor(Qt::blue));
        QCOMPARE(result.pixelColor(30, 30).alpha(), 0);
        QCOMPARE(result.pixelColor(32, 32), QColor(Qt::red));
        p.history.undo();
        QCOMPARE(p.document.active()->mask, d.active()->mask);
        QCOMPARE(p.document.active()->metadata, d.active()->metadata);
    }
    void cloneGrowthUsesFrozenSourceCoordinatesAndCancelAlignment() {
        auto d = patchDocument();
        d.active()->image.setPixelColor(2, 2, Qt::green);
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Clone);
        p.session.brushSize = 2;
        p.session.hardness = 1;
        QTest::mouseClick(p.canvas, Qt::LeftButton, Qt::AltModifier, at(p, 30, 30));
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 10, 10));
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.history.count(), 0);
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 50, 50));
        QCOMPARE(renderDocument(p.document).pixelColor(50, 50), QColor(Qt::green));
        p.history.undo();
        QCOMPARE(p.document.active()->image, d.active()->image);
        p.session.cloneAligned = false;
        QTest::mouseClick(p.canvas, Qt::LeftButton, Qt::AltModifier, at(p, 27, 27));
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 10, 10));
        QCOMPARE(p.history.index(), 0);
        QCOMPARE(p.document.active()->metadata, d.active()->metadata);
    }
    void paintOutsideSelectionOrCanvasDoesNotDirtyDocument() {
        EditorPage p(patchDocument());
        ready(p);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 4;
        p.session.selection = QImage(64, 64, QImage::Format_Grayscale8);
        p.session.selection.fill(0);
        p.session.selection.scanLine(8)[8] = 255;
        p.session.selection.scanLine(56)[56] = 255;
        const auto before = p.document;
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 16, 16));
        QCOMPARE(p.history.count(), 0);
        QVERIFY(!p.isModified());
        QCOMPARE(p.document.active()->metadata, before.active()->metadata);
        QCOMPARE(p.document.active()->image, before.active()->image);
        p.session.selection = {};
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, -20, -20));
        QCOMPARE(p.history.count(), 0);
        QVERIFY(!p.isModified());
    }
    void radialGradientUsesDocumentCoordinatesAndGrows() {
        auto d = patchDocument();
        auto t = d.active()->transform();
        t["size"] = QJsonArray{16, 8};
        d.active()->metadata["transform"] = t;
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Gradient);
        p.session.gradientKind = 1;
        p.session.gradientBackground = true;
        p.session.foreground = Qt::white;
        p.session.background = Qt::black;
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 32, 32));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 52, 32));
        const auto result = renderDocument(p.document);
        QVERIFY(std::abs(result.pixelColor(42, 32).red() - result.pixelColor(32, 42).red()) < 12);
        QVERIFY(result.pixelColor(8, 8).alpha() > 240);
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        QCOMPARE(p.document.active()->metadata, d.active()->metadata);
        QCOMPARE(p.document.active()->image, d.active()->image);
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 32, 32));
        QCOMPARE(p.history.index(), 0);
        QVERIFY(!p.isModified());
    }
    void linearGradientReverseTransparencySelectionAndMask() {
        auto d = patchDocument();
        d.active()->image.fill(Qt::transparent);
        d.active()->mask = QImage(8, 8, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Gradient);
        p.session.foreground = Qt::white;
        p.session.gradientReverse = true;
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 16, 32));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 48, 32));
        const auto result = renderDocument(p.document);
        QVERIFY(result.pixelColor(48, 32).alpha() > 240);
        QVERIFY(result.pixelColor(16, 32).alpha() < 20);
        p.history.undo();
        p.session.target = EditTarget::Mask;
        p.session.gradientBackground = true;
        p.session.background = Qt::black;
        p.session.gradientReverse = false;
        p.session.selection = QImage(64, 64, QImage::Format_Grayscale8);
        p.session.selection.fill(0);
        for (int y = 8; y < 24; ++y)
            std::fill_n(p.session.selection.scanLine(y) + 8, 16, uchar(255));
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 8, 16));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 24, 16));
        auto mask = paintTarget(*p.document.active(), true);
        auto inv = mask.placement(mask.image.size()).inverted();
        auto left = inv.map(QPointF(9, 16)), right = inv.map(QPointF(22, 16));
        QVERIFY(mask.image.constScanLine(int(left.y()))[int(left.x())] >
                mask.image.constScanLine(int(right.y()))[int(right.x())]);
        QCOMPARE(p.document.active()->image, d.active()->image);
        p.document.validateAssets();
        p.history.undo();
        QCOMPARE(p.document.active()->mask, d.active()->mask);
    }
    void transformedBrushExpansionAndOnePixelMask() {
        auto d = patchDocument();
        auto t = d.active()->transform();
        t["rotation"] = 37;
        t["flipY"] = true;
        t["size"] = QJsonArray{12, 16};
        d.active()->metadata["transform"] = t;
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 8;
        p.session.hardness = 1;
        p.session.foreground = Qt::blue;
        auto old = p.document.active()->placement({8, 8});
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 16, 16));
        QCOMPARE(renderDocument(p.document).pixelColor(16, 16), QColor(Qt::blue));
        auto center = p.document.active()->placement(p.document.active()->image.size()).inverted()
                          .map(old.map(QPointF(4, 4)));
        QCOMPARE(p.document.active()->image.pixelColor(int(center.x()), int(center.y())), QColor(Qt::red));
        p.history.undo();
        p.document.active()->mask = QImage(1, 1, QImage::Format_Grayscale8);
        p.document.active()->mask.fill(255);
        p.document.active()->metadata["maskFile"] = p.document.activeId() + ".mask.png";
        p.session.target = EditTarget::Mask;
        p.session.foreground = Qt::black;
        p.session.brushSize = 3;
        const auto before = *p.document.active();
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 34, 36));
        QVERIFY(p.document.active()->mask.size() != QSize(1, 1));
        auto mask = paintTarget(*p.document.active(), true);
        auto q = mask.placement(mask.image.size()).inverted().map(QPointF(34, 36));
        QCOMPARE(mask.image.constScanLine(int(q.y()))[int(q.x())], uchar(0));
        QVERIFY(mask.image.constScanLine(0)[0] > 0);
        p.history.undo();
        QCOMPARE(p.document.active()->mask, before.mask);
        QCOMPARE(p.document.active()->metadata, before.metadata);
    }
    void transparentCloneUsesSourceOverAndMergedSourceCanGrow() {
        auto d = smallDocument();
        d.active()->image.setPixelColor(16, 16, QColor(0, 255, 0, 128));
        d.active()->image.setPixelColor(17, 16, Qt::transparent);
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Clone);
        p.session.brushSize = 4;
        p.session.hardness = 1;
        p.session.cloneAligned = false;
        QTest::mouseClick(p.canvas, Qt::LeftButton, Qt::AltModifier, at(p, 16, 16));
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 32, 32));
        auto color = p.document.active()->image.pixelColor(32, 32);
        QCOMPARE(color.alpha(), 255);
        QVERIFY(std::abs(color.green() - 128) <= 1 && std::abs(color.red() - 127) <= 1);
        QCOMPARE(p.document.active()->image.pixelColor(33, 32), QColor(Qt::red));
        auto merged = patchDocument();
        const auto target = merged.activeId();
        QImage blue(8, 8, QImage::Format_RGBA8888_Premultiplied);
        blue.fill(Qt::blue);
        const auto upper = merged.addImage("Source", blue);
        merged.find(upper)->setBounds({8, 8, 8, 8});
        merged.metadata["activeLayerID"] = target;
        EditorPage clone(merged);
        ready(clone);
        clone.canvas->setTool(Tool::Clone);
        clone.session.cloneMerged = true;
        clone.session.brushSize = 4;
        clone.session.hardness = 1;
        QTest::mouseClick(clone.canvas, Qt::LeftButton, Qt::AltModifier, at(clone, 12, 12));
        QTest::mouseClick(clone.canvas, Qt::LeftButton, {}, at(clone, 48, 48));
        QCOMPARE(renderDocument(clone.document).pixelColor(48, 48), QColor(Qt::blue));
        clone.history.undo();
        QCOMPARE(clone.document.active()->image, merged.active()->image);
    }
    void fillsGrowToCanvasOrSelectionAndRespectPlacedMasks() {
        EditorWindow w;
        QTemporaryDir dir;
        auto d = patchDocument();
        d.active()->mask = QImage(8, 8, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        saveProject(d, dir.path() + "/Patch.comp");
        w.openPath(dir.path() + "/Patch.comp");
        auto p = page(w);
        p->session.background = Qt::blue;
        action(w, "Fill with Background")->trigger();
        QCOMPARE(renderDocument(p->document).pixelColor(8, 8), QColor(Qt::blue));
        QCOMPARE(renderDocument(p->document).pixelColor(56, 56), QColor(Qt::blue));
        QCOMPARE(p->history.count(), 1);
        p->history.undo();
        QCOMPARE(p->document.active()->metadata, d.active()->metadata);
        QCOMPARE(p->document.active()->mask, d.active()->mask);
        p->session.selection = QImage(64, 64, QImage::Format_Grayscale8);
        p->session.selection.fill(0);
        for (int y = 8; y < 16; ++y)
            std::fill_n(p->session.selection.scanLine(y) + 8, 8, uchar(128));
        p->session.foreground = Qt::green;
        action(w, "Fill with Foreground")->trigger();
        auto result = renderDocument(p->document);
        QCOMPARE(result.pixelColor(10, 10).alpha(), 128);
        QCOMPARE(result.pixelColor(17, 10).alpha(), 0);
        QCOMPARE(result.pixelColor(32, 32), QColor(Qt::red));
        p->history.undo();
        p->session.target = EditTarget::Mask;
        // The reveal-all mask grows in its edge tone, white (as on the Mac): filling black at the
        // selection's half coverage leaves it half way.
        p->session.foreground = Qt::black;
        action(w, "Fill with Foreground")->trigger();
        auto mask = paintTarget(*p->document.active(), true);
        auto q = mask.placement(mask.image.size()).inverted().map(QPointF(10, 10));
        QVERIFY(std::abs(int(mask.image.constScanLine(int(q.y()))[int(q.x())]) - 127) <= 1);
        QCOMPARE(p->document.active()->image, d.active()->image);
        p->document.validateAssets();
        p->history.undo();
        QCOMPARE(p->document.active()->mask, d.active()->mask);
    }
    void surfaceBudgetFailureLeavesOriginalIntact() {
        auto d = patchDocument();
        const auto before = *d.active();
        QVERIFY_EXCEPTION_THROWN(growPaintSurface(*d.active(), false, {0, 0, MaxSide + 1, 8}), Error);
        QVERIFY_EXCEPTION_THROWN(growPaintSurface(*d.active(), false, {0, 0, 20000, 20000}), Error);
        QCOMPARE(d.active()->image, before.image);
        QCOMPARE(d.active()->metadata, before.metadata);
        auto tiny = *d.active();
        tiny.image = QImage(30000, 1, QImage::Format_RGBA8888_Premultiplied);
        tiny.setBounds({0, 0, 1, 1});
        QVERIFY_EXCEPTION_THROWN(paintSurfaceBounds(tiny, QRectF(0, 0, 30000, 1)), Error);
    }
    void viewPreferencesCancelAndLanguageScreenshots() {
        EditorWindow w;
        w.show();
        QTest::qWait(30);
        QTemporaryDir dir;
        saveProject(createDemoDocument(), dir.path() + "/Canvas.comp");
        w.openPath(dir.path() + "/Canvas.comp");
        action(w, "Show Rulers")->setChecked(true);
        QVERIFY(page(w)->session.view.rulers);
        QVERIFY(loadCanvasViewOptions().rulers);
        action(w, "Show Transform Controls")->setChecked(false);
        QVERIFY(!w.findChild<QCheckBox *>("transformControls")->isChecked());
        action(w, "Show Transform Controls")->setChecked(true);
        action(w, "Show Grid")->setChecked(true);
        const auto before = loadCanvasViewOptions();
        QTimer::singleShot(30, [] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            if (dialog)
                dialog->reject();
        });
        action(w, "Grid and Guide Settings…")->trigger();
        QCOMPARE(loadCanvasViewOptions().gridSpacing, before.gridSpacing);
        UiLanguage::instance().setLanguage("zh_CN", false);
        QCOMPARE(action(w, "Show Rulers")->text(), QString("显示标尺"));
        tool(w, Tool::Move);
        QTest::qWait(30);
        page(w)->canvas->waitForRendering();
        if (qEnvironmentVariableIsSet("COMPOSITOR_CANVAS_SCREENSHOTS"))
            w.grab().save("artifacts/c1-c5-zh-options.png");
        UiLanguage::instance().setLanguage("ja_JP", false);
        tool(w, Tool::Text);
        QTest::qWait(30);
        QCOMPARE(w.findChild<QComboBox *>("textAlignment")->itemText(1), QString("中央"));
        if (qEnvironmentVariableIsSet("COMPOSITOR_CANVAS_SCREENSHOTS"))
            w.grab().save("artifacts/c1-c5-ja-options.png");
        QTimer::singleShot(30, [] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            if (dialog) {
                if (qEnvironmentVariableIsSet("COMPOSITOR_CANVAS_SCREENSHOTS"))
                    dialog->grab().save("artifacts/c1-c5-ja-shortcuts.png");
                dialog->reject();
            }
        });
        Shortcuts::instance().showDialog(&w);
        const auto count = Shortcuts::instance().entries().size();
        EditorWindow second;
        QCOMPARE(Shortcuts::instance().entries().size(), count);
    }
    void pickerAverageAndGradientOptions() {
        auto d = smallDocument();
        d.active()->image.fill(Qt::blue);
        for (int y = 29; y <= 31; ++y)
            d.active()->image.setPixelColor(30, y, Qt::red);
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Eyedropper);
        p.session.pickerSize = 3;
        QSignalSpy picked(p.canvas, &Canvas::colorPicked);
        QTest::mouseClick(p.canvas, Qt::LeftButton, {}, at(p, 30, 30));
        QCOMPARE(picked.count(), 1);
        auto color = picked[0][0].value<QColor>();
        QVERIFY(std::abs(color.red() - 85) <= 1);
        QVERIFY(std::abs(color.blue() - 170) <= 1);
        p.canvas->setTool(Tool::Gradient);
        p.session.gradientKind = 1;
        p.session.gradientBackground = true;
        p.session.foreground = Qt::white;
        p.session.background = Qt::black;
        auto from = at(p, 32, 32), to = at(p, 48, 32);
        mouse(p.canvas, QEvent::MouseButtonPress, from);
        mouse(p.canvas, QEvent::MouseMove, to);
        mouse(p.canvas, QEvent::MouseButtonRelease, to);
        QVERIFY(p.document.active()->image.pixelColor(32, 32).red() >
                p.document.active()->image.pixelColor(48, 32).red());
        p.history.undo();
        QCOMPARE(p.document.active()->image, d.active()->image);
    }
    void shortcutValidationRemapAndPhysicalRelease() {
        EditorWindow w;
        auto &shortcuts = Shortcuts::instance();
        auto values = assignments();
        QVERIFY2(shortcuts.validate(values).isEmpty(), qPrintable(shortcuts.validate(values)));
        values["canvas/swap"] = QKeySequence("D");
        QVERIFY(!shortcuts.validate(values).isEmpty());
        values = assignments();
        values["canvas/pan"] = QKeySequence("F8");
        values["canvas/reset"] = QKeySequence("F9");
        QVERIFY(shortcuts.save(values));
        EditorPage p(smallDocument());
        ready(p);
        p.session.foreground = Qt::red;
        QTest::keyClick(p.canvas, Qt::Key_D);
        QCOMPARE(p.session.foreground, QColor(Qt::red));
        QTest::keyClick(p.canvas, Qt::Key_F9);
        QCOMPARE(p.session.foreground, QColor(Qt::black));
        QTest::keyPress(p.canvas, Qt::Key_F8);
        QCOMPARE(p.canvas->cursor().shape(), Qt::OpenHandCursor);
        QTest::keyRelease(p.canvas, Qt::Key_F8);
        QCOMPARE(p.canvas->cursor().shape(), Qt::CrossCursor);
        QCOMPARE(QSettings().value("shortcuts/v1/canvas/pan").toString(), QString("F8"));
    }
    void shortcutDialogCancelAndTyping() {
        EditorWindow w;
        w.show();
        QTest::qWait(25);
        auto values = assignments();
        QTimer::singleShot(30, [] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            if (dialog)
                dialog->reject();
        });
        Shortcuts::instance().showDialog(&w);
        QCOMPARE(assignments(), values);
        auto edit = new QLineEdit(&w);
        edit->setGeometry(100, 150, 250, 40);
        edit->show();
        edit->setFocus();
        tool(w, Tool::Move);
        edit->setFocus();
        QTest::keyClicks(edit, "brushx");
        QCOMPARE(edit->text(), QString("brushx"));
        QCOMPARE(page(w)->session.tool, Tool::Move);
    }
    void overlaysAreViewOnly() {
        EditorPage p(smallDocument());
        ready(p);
        auto manifest = p.document.manifest();
        p.session.view.rulers = true;
        p.session.view.grid = true;
        p.canvas->selectAll();
        p.canvas->setTool(Tool::Brush);
        QTest::mouseMove(p.canvas, at(p, 32, 32));
        QImage first(p.canvas->size(), QImage::Format_ARGB32_Premultiplied);
        p.canvas->render(&first);
        QTest::qWait(130);
        QImage second(p.canvas->size(), QImage::Format_ARGB32_Premultiplied);
        p.canvas->render(&second);
        QVERIFY(first != second);
        QCOMPARE(p.document.manifest(), manifest);
        QVERIFY(!p.isModified());
        if (qEnvironmentVariableIsSet("COMPOSITOR_CANVAS_SCREENSHOTS"))
            second.save("artifacts/c1-c5-overlays.png");
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("CanvasTests");
    CanvasTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "canvas_tests.moc"

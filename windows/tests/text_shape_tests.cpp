// SPDX-License-Identifier: MIT
#include "canvas_text_editor.h"
#include "distort.h"
#include "editable_layers.h"
#include "editor.h"
#include "language.h"
#include "render.h"
#include "text_fonts.h"
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <cmath>
using namespace compositor;
namespace {
QJsonObject textStyle(QString content = "Hello 中文") {
    return {{"content", content},
            {"fontName", "Segoe UI"},
            {"fontSize", 24},
            {"red", 0.1},
            {"green", 0.2},
            {"blue", 0.3},
            {"boxSize", QJsonArray{180, 100}},
            {"alignment", "Left"}};
}
Document textDocument(QJsonObject style = textStyle()) {
    auto d = Document::create({320, 240});
    auto id = d.addImage("Text", renderText(style));
    d.find(id)->metadata["text"] = style;
    d.find(id)->setBounds({30, 70, 180, 100});
    return d;
}
Document shapeDocument(QString kind = "Rectangle") {
    auto d = Document::create({320, 240});
    QJsonObject style{{"kind", kind},
                      {"red", 1},
                      {"green", 0},
                      {"blue", 0},
                      {"cornerRadius", 12},
                      {"lineWidth", 4},
                      {"start", QJsonArray{.1, .5}},
                      {"end", QJsonArray{.9, .5}}};
    auto id = d.addImage("Shape", renderShape(style, {80, 60}));
    d.find(id)->metadata["shape"] = style;
    d.find(id)->setBounds({30, 70, 80, 60});
    return d;
}
void ready(EditorPage &p) {
    p.resize(480, 360);
    p.show();
    QTest::qWait(20);
    p.canvas->zoomTo(1);
    p.canvas->waitForRendering();
    p.session.view.snap = false;
}
QPoint at(EditorPage &p, QPointF point) {
    const double inset = p.session.view.rulers ? 24 : 0;
    return {qRound((p.canvas->width() + inset - p.document.size().width() * p.canvas->zoom) / 2 +
                   point.x() * p.canvas->zoom),
            qRound((p.canvas->height() + inset - p.document.size().height() * p.canvas->zoom) / 2 +
                   point.y() * p.canvas->zoom)};
}
void mouse(QWidget *widget, QEvent::Type type, QPoint point, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, QPointF(point), QPointF(widget->mapToGlobal(point)),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(widget, &event);
}
CanvasTextEditor *inlineEditor(EditorPage &p) {
    for (auto view : p.canvas->findChildren<QGraphicsView *>())
        if (view->isVisible())
            return static_cast<CanvasTextEditor *>(view);
    return nullptr;
}
void commit(QTextEdit *text, QString value) {
    QInputMethodEvent event;
    event.setCommitString(value);
    QApplication::sendEvent(text, &event);
}
void dragFrame(EditorPage &p, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}) {
    p.canvas->repaint();
    auto view = inlineEditor(p);
    mouse(view->viewport(), QEvent::MouseButtonPress, at(p, from), mods);
    mouse(view->viewport(), QEvent::MouseMove, at(p, to), mods);
    mouse(view->viewport(), QEvent::MouseButtonRelease, at(p, to), mods);
}
QAction *action(EditorWindow &w, QString title) {
    for (auto a : w.findChildren<QAction *>())
        if (a->property("layerAction") == title || a->property("_uiSource_text") == title)
            return a;
    return nullptr;
}
} // namespace
class TextShapeTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        // Qt's offscreen Windows plugin does not enumerate the system font directory.
        const auto fonts = qEnvironmentVariable("WINDIR") + "/Fonts/";
        for (auto file : {"segoeui.ttf", "consola.ttf", "msyh.ttc"})
            QVERIFY(QFontDatabase::addApplicationFont(fonts + file) >= 0);
    }
    void init() {
        QSettings().clear();
        UiLanguage::instance().setLanguage("en", false);
    }
    void chineseCompositionAndUndo() {
        EditorPage p(textDocument());
        ready(p);
        const auto before = p.document.active()->image;
        p.canvas->beginTextEditing(p.document.activeId());
        auto input = inlineEditor(p)->text();
        QInputMethodEvent preedit("zhongwen", {});
        QApplication::sendEvent(input, &preedit);
        QCOMPARE(p.document.active()->image, before);
        QVERIFY(p.canvas->textEditing());
        QCOMPARE(p.history.count(), 0);
        commit(input, "输入法");
        QVERIFY(p.document.active()->metadata["text"].toObject()["content"].toString().endsWith(
            "输入法"));
        QInputMethodEvent replacement;
        replacement.setCommitString("替换", -3, 3);
        QApplication::sendEvent(input, &replacement);
        QVERIFY(input->toPlainText().endsWith("替换"));
        QVERIFY(!input->toPlainText().endsWith("输入法"));
        QTest::keyClick(input, Qt::Key_Return);
        commit(input, "第二行🙂");
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(!p.canvas->textEditing());
        QCOMPARE(p.history.count(), 1);
        const auto result = p.document.active()->image;
        p.history.undo();
        QCOMPARE(p.document.active()->image, before);
        p.history.redo();
        QCOMPARE(p.document.active()->image, result);
    }
    void escapeCompositionThenCancel() {
        EditorPage p(textDocument());
        ready(p);
        const auto before = p.document.manifest();
        p.canvas->beginTextEditing(p.document.activeId());
        auto input = inlineEditor(p)->text();
        commit(input, "修改");
        QInputMethodEvent preedit("nihao", {});
        QApplication::sendEvent(input, &preedit);
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(p.canvas->textEditing());
        QVERIFY(!input->toPlainText().contains("nihao"));
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(!p.canvas->textEditing());
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.history.count(), 0);
    }
    void newParagraphAndEmptyCancel() {
        EditorPage p(Document::create({320, 240}));
        ready(p);
        p.canvas->setTool(Tool::Text);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, {20, 60}));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, {200, 160}));
        QVERIFY(p.canvas->textEditing());
        QCOMPARE(p.document.active()->image.size(), QSize(180, 100));
        commit(inlineEditor(p)->text(), "新建段落");
        p.canvas->finishTextEditing(true);
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        QVERIFY(p.document.layers.isEmpty());
        p.history.redo();
        p.canvas->beginTextEditing({}, {230, 180, 80, 40});
        p.canvas->finishTextEditing(true);
        QCOMPARE(p.document.layers.size(), 1);
        QCOMPARE(p.history.count(), 1);
    }
    void nativeTypingAndLocalUndo() {
        EditorWindow w;
        auto p = qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
        p->document = textDocument();
        p->changed();
        w.show();
        QTest::qWait(30);
        p->canvas->beginTextEditing(p->document.activeId());
        auto input = inlineEditor(*p)->text();
        const auto original = input->toPlainText();
        QTest::keyClicks(input, " bt[]012");
        QCOMPARE(p->session.tool, Tool::Text);
        QCOMPARE(p->history.count(), 0);
        QVERIFY(input->toPlainText().endsWith(" bt[]012"));
        QTest::keyClick(input, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(input->toPlainText(), original);
        QVERIFY(p->canvas->textEditing());
        QTest::keyClick(input, Qt::Key_Y, Qt::ControlModifier);
        QVERIFY(input->toPlainText().endsWith("012"));
        p->canvas->finishTextEditing(false);
    }
    void commitOnToolHideAndOtherEdit() {
        EditorPage p(textDocument());
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        commit(inlineEditor(p)->text(), "工具");
        p.canvas->setTool(Tool::Move);
        QCOMPARE(p.history.count(), 1);
        p.canvas->beginTextEditing(p.document.activeId());
        commit(inlineEditor(p)->text(), "隐藏");
        p.hide();
        QCOMPARE(p.history.count(), 2);
        p.show();
        p.canvas->beginTextEditing(p.document.activeId());
        commit(inlineEditor(p)->text(), "编辑");
        p.edit("Opacity", [](Document &d) { d.active()->metadata["opacity"] = .5; });
        QCOMPARE(p.history.count(), 4);
        QVERIFY(!p.canvas->textEditing());
        p.history.undo();
        QCOMPARE(p.document.active()->opacity(), 1.0);
        QVERIFY(p.document.active()->metadata["text"].toObject()["content"].toString().endsWith(
            "编辑"));
    }
    void typographyAndSelectedFontRuns() {
        EditorPage p(textDocument(textStyle("A🙂中文Z")));
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        auto view = inlineEditor(p);
        p.canvas->findChild<QDoubleSpinBox *>("inlineTextSize")->setValue(32);
        p.canvas->findChild<QDoubleSpinBox *>("inlineTextTracking")->setValue(2);
        p.canvas->findChild<QDoubleSpinBox *>("inlineTextLeading")->setValue(45);
        p.canvas->findChild<QComboBox *>("inlineTextAlignment")->setCurrentIndex(1);
        auto cursor = view->text()->textCursor();
        cursor.setPosition(1);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        view->text()->setTextCursor(cursor);
        p.canvas->findChild<QFontComboBox *>("inlineTextFont")->setCurrentFont(QFont("Consolas"));
        auto style = p.document.active()->metadata["text"].toObject();
        QCOMPARE(style["fontSize"].toDouble(), 32.0);
        QCOMPARE(style["tracking"].toDouble(), 2.0);
        QCOMPARE(style["leading"].toDouble(), 45.0);
        QCOMPARE(style["alignment"].toString(), QString("Center"));
        const auto run = style["fontRuns"].toArray().first().toObject();
        QCOMPARE(run["location"].toInt(), 1);
        QCOMPARE(run["length"].toInt(), 4);
        QCOMPARE(style["fontName"].toString(), QString("Segoe UI"));
        p.canvas->finishTextEditing(true);
        QCOMPARE(p.history.count(), 1);
    }
    void selectedColorAndTextEntryPoints() {
        EditorPage p(textDocument(textStyle("A🙂中文Z")));
        ready(p);
        p.canvas->setTool(Tool::Text);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, {100, 120}));
        QVERIFY(p.canvas->textEditing());
        auto input = inlineEditor(p)->text();
        auto cursor = input->textCursor();
        cursor.setPosition(1);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        input->setTextCursor(cursor);
        QTimer::singleShot(0, [] {
            auto dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->setCurrentColor(Qt::red);
            dialog->accept();
        });
        p.canvas->findChild<QAction *>("inlineTextColor")->trigger();
        const auto run = p.document.active()
                             ->metadata["text"]
                             .toObject()["colorRuns"]
                             .toArray()
                             .first()
                             .toObject();
        QCOMPARE(run["location"].toInt(), 1);
        QCOMPARE(run["length"].toInt(), 4);
        QCOMPARE(run["red"].toDouble(), 1.0);
        QVERIFY(p.canvas->textEditing());
        p.canvas->finishTextEditing(true);
        p.canvas->setTool(Tool::Move);
        QTest::mouseDClick(p.canvas, Qt::LeftButton, {}, at(p, {100, 120}));
        QVERIFY(p.canvas->textEditing());
        p.canvas->finishTextEditing(false);
        QTemporaryDir dir;
        const auto path = dir.filePath("color.comp");
        saveProject(p.document, path);
        QCOMPARE(loadProject(path).active()->metadata["text"],
                 p.document.active()->metadata["text"]);
    }
    void destroyActiveEditor() {
        for (int i = 0; i < 3; ++i) {
            EditorPage p(textDocument());
            ready(p);
            p.canvas->beginTextEditing(p.document.activeId());
            commit(inlineEditor(p)->text(), "关闭编辑页");
        }
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void fontMappingPreservesDocumentNames() {
        auto style = textStyle("Mac 中文🙂");
        style["fontName"] = "Missing Mac Font";
        style["fontRuns"] =
            QJsonArray{QJsonObject{{"location", 4}, {"length", 2}, {"fontName", "PingFang SC"}}};
        setTextFontMappings({{"Missing Mac Font", "Consolas"}});
        QCOMPARE(resolvedTextFont("Missing Mac Font"), QString("Consolas"));
        QTextDocument document;
        loadTextDocument(document, style);
        const auto result = textStyleFromDocument(document, style);
        QCOMPARE(result["fontName"], style["fontName"]);
        QCOMPARE(result["fontRuns"], style["fontRuns"]);
        auto cursor = QTextCursor(&document);
        cursor.movePosition(QTextCursor::End);
        cursor.insertText("新增");
        QCOMPARE(textStyleFromDocument(document, style)["fontName"], style["fontName"]);
        setTextFontMappings({{"Missing Mac Font", "Not installed"}});
        QVERIFY(resolvedTextFont("Missing Mac Font") != "Not installed");
    }
    void mappingDialogApplyResetCancel() {
        auto style = textStyle();
        style["fontName"] = "Unavailable font";
        EditorPage p(textDocument(style));
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        auto mapping = p.canvas->findChild<QAction *>("inlineFontMapping");
        QVERIFY(mapping);
        auto respond = [&](bool accept, bool reset = false) {
            QTimer::singleShot(0, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto buttons = dialog->findChild<QDialogButtonBox *>();
                if (reset)
                    buttons->button(QDialogButtonBox::Reset)->click();
                else
                    dialog->findChild<QFontComboBox *>()->setCurrentFont(QFont("Consolas"));
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            });
            mapping->trigger();
        };
        respond(false);
        QVERIFY(textFontMappings().isEmpty());
        respond(true);
        QCOMPARE(textFontMappings().value("Unavailable font"), QString("Consolas"));
        QCOMPARE(inlineEditor(p)->text()->textCursor().position(),
                 inlineEditor(p)->text()->toPlainText().size());
        QCOMPARE(p.canvas->findChild<QFontComboBox *>("inlineTextFont")->currentFont().family(),
                 QString("Consolas"));
        QCOMPARE(p.document.active()->metadata["text"].toObject()["fontName"].toString(),
                 QString("Unavailable font"));
        respond(true, true);
        QVERIFY(textFontMappings().isEmpty());
        p.canvas->finishTextEditing(true);
    }
    void frameResizeReflowsAndMoves() {
        EditorPage p(
            textDocument(textStyle("A paragraph that wraps onto multiple lines 中文段落")));
        ready(p);
        const auto before = p.document.active()->image;
        p.canvas->beginTextEditing(p.document.activeId());
        dragFrame(p, {210, 120}, {290, 120});
        QCOMPARE(p.document.active()->image.size(), QSize(260, 100));
        QCOMPARE(
            p.document.active()->metadata["text"].toObject()["boxSize"].toArray()[0].toDouble(),
            260.0);
        QVERIFY(p.document.active()->image != before);
        dragFrame(p, {100, 70}, {120, 82}, Qt::ShiftModifier);
        QCOMPARE(p.document.active()->transform()["origin"].toArray(), (QJsonArray{50, 70}));
        p.canvas->finishTextEditing(true);
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        QCOMPARE(p.document.active()->image, before);
    }
    void frameShiftAndReturnToStart() {
        EditorPage p(textDocument());
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        auto view = inlineEditor(p);
        mouse(view->viewport(), QEvent::MouseButtonPress, at(p, {210, 170}));
        mouse(view->viewport(), QEvent::MouseMove, at(p, {300, 190}), Qt::ShiftModifier);
        const auto size = p.document.active()->metadata["text"].toObject()["boxSize"].toArray();
        QVERIFY(std::abs(size[0].toDouble() / size[1].toDouble() - 1.8) < .01);
        mouse(view->viewport(), QEvent::MouseButtonRelease, at(p, {210, 170}));
        QCOMPARE(p.document.active()->image.size(), QSize(180, 100));
        QCOMPARE(p.document.active()->metadata["text"].toObject()["boxSize"].toArray(),
                 (QJsonArray{180, 100}));
        p.canvas->finishTextEditing(false);
    }
    void rotatedFlippedFrameKeepsAnchor() {
        auto d = textDocument();
        auto transform = d.active()->transform();
        transform["rotation"] = 25;
        transform["flipX"] = true;
        d.active()->metadata["transform"] = transform;
        EditorPage p(d);
        ready(p);
        const auto placement = p.document.active()->placement({180, 100});
        const auto anchor = placement.map(QPointF());
        p.canvas->beginTextEditing(p.document.activeId());
        dragFrame(p, placement.map(QPointF(180, 100)), placement.map(QPointF(220, 130)));
        auto layer = p.document.active();
        QVERIFY(QLineF(layer->placement(layer->image.size()).map(QPointF()), anchor).length() <
                1.5);
        QCOMPARE(layer->transform()["rotation"].toDouble(), 25.0);
        QVERIFY(layer->transform()["flipX"].toBool());
        p.canvas->zoomTo(.75);
        p.canvas->repaint();
        QVERIFY(p.canvas->textEditing());
        p.canvas->finishTextEditing(true);
    }
    void pointTextMovesWithoutParagraphConversion() {
        auto style = textStyle("Point");
        style.remove("boxSize");
        auto d = textDocument(style);
        auto layer = d.active();
        layer->setBounds(QRectF({30, 70}, layer->image.size()));
        EditorPage p(d);
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        dragFrame(p, {55, 70}, {65, 80});
        QVERIFY(!p.document.active()->metadata["text"].toObject().contains("boxSize"));
        p.canvas->finishTextEditing(true);
    }
    void saveAndLayerSwitchCommit() {
        QTemporaryDir directory;
        auto d = textDocument();
        const auto textID = d.activeId();
        const auto blankID = d.addBlank("Other");
        d.metadata["activeLayerID"] = textID;
        const auto path = directory.filePath("text.comp");
        saveProject(d, path);
        EditorWindow w;
        w.openPath(path);
        w.show();
        QTest::qWait(30);
        auto tabs = w.findChild<QTabWidget *>();
        auto p = qobject_cast<EditorPage *>(tabs->currentWidget());
        p->canvas->beginTextEditing(textID);
        commit(inlineEditor(*p)->text(), "保存");
        auto save = action(w, "Save Project");
        QVERIFY(save);
        save->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!p->saving, 10000);
        QVERIFY(!p->canvas->textEditing());
        QVERIFY(loadProject(path)
                    .find(textID)
                    ->metadata["text"]
                    .toObject()["content"]
                    .toString()
                    .endsWith("保存"));
        p->canvas->beginTextEditing(textID);
        commit(inlineEditor(*p)->text(), "切换");
        auto tree = w.findChild<QTreeWidget *>("layerTree");
        auto items = tree->findItems("Other", Qt::MatchExactly);
        QVERIFY(!items.isEmpty());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {},
                          tree->visualItemRect(items.first()).center());
        QVERIFY(!p->canvas->textEditing());
        QCOMPARE(p->document.activeId(), blankID);
        QVERIFY(
            p->document.find(textID)->metadata["text"].toObject()["content"].toString().endsWith(
                "切换"));
        p->markSaved(p->contentState);
    }
    void noChangePreservesImportedRaster() {
        auto d = textDocument();
        d.active()->image.fill(Qt::magenta);
        EditorPage p(d);
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        p.canvas->finishTextEditing(true);
        QCOMPARE(p.document.active()->image, d.active()->image);
        QCOMPARE(p.history.count(), 0);
    }
    void overflowingParagraphKeepsSourceGrid() {
        EditorPage p(textDocument());
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        auto input = inlineEditor(p)->text();
        commit(input, "\n中文一\n中文二\n中文三\n中文四\n中文五");
        QTest::qWait(20);
        QCOMPARE(input->verticalScrollBar()->value(), 0);
        QCOMPARE(input->horizontalScrollBar()->value(), 0);
        QCOMPARE(p.document.active()->image.size(), QSize(180, 100));
        p.canvas->finishTextEditing(true);
    }
    void roundTripAndInvalidParagraph() {
        EditorPage p(textDocument());
        ready(p);
        p.canvas->beginTextEditing(p.document.activeId());
        commit(inlineEditor(p)->text(), "🙂保存");
        p.canvas->finishTextEditing(true);
        QTemporaryDir dir;
        auto path = dir.filePath("rich.comp");
        saveProject(p.document, path);
        auto loaded = loadProject(path);
        QCOMPARE(loaded.active()->metadata, p.document.active()->metadata);
        QCOMPARE(loaded.active()->image, p.document.active()->image);
        QCOMPARE(CurrentVersion, 11);
        auto invalid = textStyle();
        invalid["boxSize"] = QJsonArray{1e100, 20};
        QVERIFY_EXCEPTION_THROWN(renderText(invalid), Error);
        p.canvas->beginTextEditing(p.document.activeId());
        dragFrame(p, {210, 170}, {60000, 40000});
        QVERIFY(!p.canvas->textEditing());
        QCOMPARE(p.document.active()->image, loaded.active()->image);
        auto style = p.document.active()->metadata["text"].toObject();
        style["fontSize"] = 3001;
        p.document.active()->metadata["text"] = style;
        QSignalSpy errors(&p, &EditorPage::error);
        p.canvas->setTool(Tool::Move);
        QTest::mouseDClick(p.canvas, Qt::LeftButton, {}, at(p, {100, 120}));
        QCOMPARE(errors.count(), 1);
        QVERIFY(!p.canvas->textEditing());
        QCOMPARE(p.document.active()->image, loaded.active()->image);
    }
    void vectorResizeCornerRadiusAndStroke() {
        auto d = shapeDocument();
        const auto shape = d.active()->metadata["shape"];
        auto next = d.active()->transform();
        next["size"] = QJsonArray{240, 90};
        retransformLayer(*d.active(), next);
        QCOMPARE(d.active()->image.size(), QSize(240, 90));
        QCOMPARE(d.active()->metadata["shape"], shape);
        QCOMPARE(d.active()->image.pixelColor(5, 5).alpha(), 255);
        QCOMPARE(d.active()->image.pixelColor(0, 0).alpha(), 0);
        auto line = shapeDocument("Line");
        next = line.active()->transform();
        next["size"] = QJsonArray{240, 120};
        retransformLayer(*line.active(), next);
        QCOMPARE(line.active()->image.pixelColor(120, 60).alpha(), 255);
        QCOMPARE(line.active()->image.pixelColor(120, 65).alpha(), 0);
        auto ellipse = shapeDocument("Ellipse");
        next = ellipse.active()->transform();
        next["size"] = QJsonArray{250, 70};
        retransformLayer(*ellipse.active(), next);
        QCOMPARE(ellipse.active()->image.pixelColor(125, 35).alpha(), 255);
        QCOMPARE(ellipse.active()->image.pixelColor(0, 0).alpha(), 0);
    }
    void vectorHandlesShiftUndoMasksAndRoundTrip() {
        auto d = shapeDocument();
        d.active()->mask = QImage(80, 60, QImage::Format_Grayscale8);
        d.active()->mask.fill(128);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        EditorPage p(d);
        ready(p);
        p.canvas->setTool(Tool::Move);
        p.session.view.transformControls = true;
        QSignalSpy errors(&p, &EditorPage::error);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, {110, 130}));
        QVERIFY(p.interacting());
        mouse(p.canvas, QEvent::MouseMove, at(p, {190, 160}), Qt::ShiftModifier);
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, {190, 160}), Qt::ShiftModifier);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QCOMPARE(p.history.count(), 1);
        const auto resized = *p.document.active();
        QVERIFY(resized.image.width() > 80);
        QVERIFY(std::abs(double(resized.image.width()) / resized.image.height() - 80.0 / 60) < .02);
        QVERIFY(resized.metadata["maskPlacement"].isObject());
        QCOMPARE(resized.mask, d.active()->mask);
        p.history.undo();
        QCOMPARE(p.document.active()->image, d.active()->image);
        p.history.redo();
        QCOMPARE(p.document.active()->image, resized.image);
        QTemporaryDir dir;
        auto path = dir.filePath("vector.comp");
        saveProject(p.document, path);
        auto loaded = loadProject(path);
        QCOMPARE(loaded.active()->metadata, resized.metadata);
        QCOMPARE(loaded.active()->image, resized.image);
        QCOMPARE(loaded.active()->mask, resized.mask);
    }
    void independentMaskAndVectorLimits() {
        auto d = shapeDocument();
        auto layer = d.active();
        layer->mask = QImage(20, 20, QImage::Format_Grayscale8);
        layer->mask.fill(255);
        layer->metadata["maskFile"] = layer->id() + ".mask.png";
        const auto placed = makeTransform({50, 80, 20, 20});
        layer->metadata["maskPlacement"] = placed;
        layer->metadata["maskLinked"] = false;
        auto next = layer->transform();
        next["size"] = QJsonArray{150, 80};
        retransformLayer(*layer, next);
        QCOMPARE(layer->metadata["maskPlacement"].toObject(), placed);
        const auto before = *layer;
        next["size"] = QJsonArray{50000, 100};
        QVERIFY_EXCEPTION_THROWN(retransformLayer(*layer, next), Error);
        QCOMPARE(layer->metadata, before.metadata);
        QCOMPARE(layer->image, before.image);
    }
    void shiftedCreationAndTranslations() {
        EditorPage p(Document::create({320, 240}));
        ready(p);
        p.canvas->setTool(Tool::Rectangle);
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, {20, 60}));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, {90, 100}), Qt::ShiftModifier);
        const auto size = p.document.active()->transform()["size"].toArray();
        QCOMPARE(size[0], size[1]);
        p.session.textSize = 24;
        p.canvas->beginTextEditing({}, {110, 60, 180, 100});
        commit(inlineEditor(p)->text(), "中文🙂\nCanvas text");
        UiLanguage::instance().setLanguage("zh_CN", false);
        QCOMPARE(p.canvas->findChild<QAction *>("inlineFontMapping")->text(), QString("字体映射…"));
        QCOMPARE(p.canvas->findChild<QComboBox *>("inlineTextAlignment")->itemText(1),
                 QString("居中"));
        p.canvas->findChild<QComboBox *>("inlineTextAlignment")->setCurrentIndex(2);
        QCOMPARE(p.document.active()->metadata["text"].toObject()["alignment"].toString(),
                 QString("Right"));
        QTest::qWait(30);
        p.canvas->waitForRendering();
        QTest::qWait(30);
        if (qEnvironmentVariableIsSet("COMPOSITOR_TEXT_SCREENSHOTS"))
            QVERIFY(p.canvas->grab().save("artifacts/x1-x2-inline-text.png"));
        p.canvas->finishTextEditing(false);
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("TextShapeTests");
    TextShapeTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "text_shape_tests.moc"

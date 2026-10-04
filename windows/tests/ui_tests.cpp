#include "editable_layers.h"
#include "editor.h"
#include "image_operations.h"
#include "image_scope.h"
#include "language.h"
#include "raw_dialog.h"
#include "raw_fixture.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <numeric>
using namespace compositor;
class UiTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void cleanup() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void scopeCountsCoverageAndIgnoresTransparentPixels() {
        QImage pixels(3, 1, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::transparent);
        pixels.setPixelColor(0, 0, Qt::black);
        pixels.setPixelColor(1, 0, QColor(255, 0, 0, 128));
        ImageScope scope;
        scope.setImage(pixels);
        QVERIFY(std::abs(scope.histograms[0][255] - 128.0 / 255) < 1e-6);
        QCOMPARE(scope.histograms[0][0], 1.0);
        double sum = std::accumulate(scope.vectors.begin(), scope.vectors.end(), 0.0);
        QVERIFY(std::abs(sum - 128.0 / 255) < 1e-6);
    }
    void cameraRawAutoWhiteBalanceCompletes() {
        QTemporaryDir dir;
        auto d = Document::create({24, 24});
        QImage image(24, 24, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(120, 110, 100));
        d.addImage("Photo", image);
        auto path = dir.filePath("WhiteBalance.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        QAction *filter = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Camera Raw…")
                filter = action;
        QVERIFY(filter);
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto autoBalance = dialog->findChild<QPushButton *>("autoWhiteBalance");
            QVERIFY(autoBalance);
            autoBalance->click();
            QTRY_VERIFY_WITH_TIMEOUT(autoBalance->isEnabled(), 3000);
            auto temperature = dialog->findChild<QDoubleSpinBox *>("temperatureControl");
            QVERIFY(temperature && temperature->value() < 0);
            visited = true;
            dialog->accept();
        });
        filter->trigger();
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 1);
        auto neutral = page->document.active()->image.pixelColor(12, 12);
        QVERIFY(std::abs(neutral.red() - neutral.green()) <= 1 &&
                std::abs(neutral.green() - neutral.blue()) <= 1);
        page->history.undo();
        QCOMPARE(page->document.active()->image, image);
    }
    void cameraRawSamplingAndGuidesInJapanese() {
        QTemporaryDir dir;
        auto d = Document::create({32, 32});
        QImage pixels(32, 32, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                pixels.setPixelColor(x, y, x < 16 ? Qt::red : Qt::blue);
        d.addImage("Pixels", pixels);
        auto path = dir.filePath("Sampling.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        window.show();
        UiLanguage::instance().setLanguage("ja_JP", false);
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        QAction *filter = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->property("_uiSource_text").toString() == "Camera Raw…")
                filter = action;
        QVERIFY(filter);
        bool sampled = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto tool = dialog->findChild<QComboBox *>("cameraPreviewTool");
            auto preview = dialog->findChild<QLabel *>("filterPreview");
            auto point = dialog->findChild<QComboBox *>("pointColorList");
            QVERIFY(tool && preview && point);
            selectComboValue(tool, "Sample point color");
            QCOMPARE(comboValue(tool), QString("Sample point color"));
            QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier,
                              preview->rect().center() - QPoint(60, 0));
            QCOMPARE(point->count(), 1);
            auto saturation = dialog->findChild<QDoubleSpinBox *>("pointSaturationShiftControl");
            QVERIFY(saturation && saturation->isEnabled());
            saturation->setValue(-100);
            sampled = true;
            dialog->accept();
        });
        filter->trigger();
        QVERIFY(sampled);
        QCOMPARE(page->history.count(), 1);
        auto color = page->document.active()->image.pixelColor(8, 16);
        QVERIFY(color.hslSaturationF() < .01);
        QCOMPARE(page->document.active()->image.pixelColor(24, 16), QColor(Qt::blue));
        auto afterColor = page->document.active()->image;
        bool guided = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto tool = dialog->findChild<QComboBox *>("cameraPreviewTool");
            auto preview = dialog->findChild<QLabel *>("filterPreview");
            selectComboValue(tool, "Draw geometry guides");
            auto center = preview->rect().center();
            QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, center - QPoint(60, 15));
            QTest::mouseMove(preview, center + QPoint(60, 15));
            QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, center + QPoint(60, 15));
            auto upright = dialog->findChild<QCheckBox *>("geometryUprightControl");
            QVERIFY(upright && upright->isChecked());
            guided = true;
            dialog->accept();
        });
        filter->trigger();
        QVERIFY(guided);
        QCOMPARE(page->history.count(), 2);
        QVERIFY(page->document.active()->image != afterColor);
        QCOMPARE(page->document.active()->image.pixelColor(0, 0).alpha(), 0);
        page->history.undo();
        QCOMPARE(page->document.active()->image, afterColor);
        page->history.undo();
        QCOMPARE(page->document.active()->image, pixels);
    }
    void liveLanguagesPreserveDocumentAndCanonicalValues() {
        EditorWindow window;
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        page->edit("Pixels", [](Document &d) { d.addBlank("Language"); });
        auto before = page->document.manifest();
        int history = page->history.count();
        window.show();
        UiLanguage::instance().setLanguage("zh_CN", false);
        QVERIFY(window.menuBar()->actions().first()->text().startsWith("文件"));
        auto blend = window.findChild<QComboBox *>("layerBlendMode");
        QVERIFY(blend);
        QCOMPARE(comboValue(blend), QString("Normal"));
        QCOMPARE(blend->currentText(), QString("正常"));
        QCOMPARE(page->document.manifest(), before);
        QCOMPARE(page->history.count(), history);
        UiLanguage::instance().setLanguage("ja_JP", false);
        QVERIFY(window.menuBar()->actions().first()->text().startsWith("ファイル"));
        QCOMPARE(blend->currentText(), QString("通常"));
        QCOMPARE(page->document.active()->name(), QString("Language"));
        selectComboValue(blend, "Multiply");
        QCOMPARE(blend->currentText(), QString("乗算"));
        QCOMPARE(page->document.active()->blend(), QString("Multiply"));
        auto action = window.findChild<QAction *>("language_ja_JP");
        QVERIFY(action && action->isChecked());
        QDialog dialog;
        QVBoxLayout layout(&dialog);
        QLabel label("Exposure");
        layout.addWidget(&label);
        dialog.show();
        QTest::qWait(10);
        QCOMPARE(label.text(), QString("露光量"));
        UiLanguage::instance().setLanguage("en", false);
        QCOMPARE(label.text(), QString("Exposure"));
        QCOMPARE(blend->currentText(), QString("Multiply"));
        QCOMPARE(window.menuBar()->actions().first()->text(), QString("&File"));
        page->history.undo();
        QCOMPARE(page->document.active()->blend(), QString("Normal"));
    }
    void languagePreferencePersists() {
        QTemporaryDir dir;
        auto previous = QSettings::defaultFormat();
        auto organization = QCoreApplication::organizationName();
        auto restore = qScopeGuard([previous, organization] {
            QSettings::setDefaultFormat(previous);
            QCoreApplication::setOrganizationName(organization);
        });
        QCoreApplication::setOrganizationName("CompositorTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        UiLanguage::instance().setLanguage("ja_JP");
        QSettings saved;
        saved.sync();
        QCOMPARE(saved.status(), QSettings::NoError);
        QVERIFY(QFileInfo::exists(saved.fileName()));
        QCOMPARE(QSettings().value("ui/language").toString(), QString("ja_JP"));
        UiLanguage::instance().setLanguage("en", false);
        UiLanguage::instance().initialize();
        QCOMPARE(UiLanguage::instance().code(), QString("ja_JP"));
        QSettings::setDefaultFormat(previous);
    }
    void switchingToolCancelsStroke() {
        auto d = Document::create({64, 64});
        d.addBlank("Pixels");
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->setTool(Tool::Brush);
        c->session().foreground = Qt::red;
        auto before = page.document.active()->image;
        QSignalSpy errors(&page, &EditorPage::error);
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, c->rect().center());
        QVERIFY(page.document.active()->image != before);
        c->setTool(Tool::Move);
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, c->rect().center());
        QCOMPARE(errors.count(), 0);
        QCOMPARE(page.document.active()->image, before);
        QCOMPARE(page.history.count(), 0);
        c->setTool(Tool::Smudge);
        QTest::mouseClick(c, Qt::LeftButton, Qt::NoModifier, c->rect().center());
        QCOMPARE(page.history.count(), 0);
    }
    void ditherAndAdvancedFiltersCommitUndo() {
        auto d = Document::create({24, 24});
        QImage image(24, 24, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(120, 120, 120));
        d.addImage("Pixels", image);
        QTemporaryDir dir;
        auto path = dir.filePath("Filters.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        for (auto kind : {"Dither", "Vignette", "Tonal Contrast", "Camera Raw"}) {
            QAction *action = nullptr;
            for (auto a : window.findChildren<QAction *>())
                if (a->text() == QString(kind) + "…")
                    action = a;
            QVERIFY(action);
            bool visited = false;
            QTimer::singleShot(20, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                visited = true;
                if (QString(kind) == "Camera Raw") {
                    auto spin = dialog->findChild<QDoubleSpinBox *>("exposureControl");
                    QVERIFY(spin);
                    spin->setValue(1);
                }
                dialog->accept();
            });
            auto before = page->document.active()->image;
            int count = page->history.count();
            action->trigger();
            QVERIFY(visited);
            if (QString(kind) != "Tonal Contrast")
                QVERIFY(page->document.active()->image != before);
            QCOMPARE(page->history.count(), count + 1);
            page->history.undo();
            QCOMPARE(page->document.active()->image, before);
            page->history.redo();
        }
    }
    void warpBrushesUndoSelectionAndAlpha_data() {
        QTest::addColumn<int>("tool");
        QTest::newRow("Smudge") << int(Tool::Smudge);
        QTest::newRow("Liquify") << int(Tool::Liquify);
        QTest::newRow("Blur") << int(Tool::Blur);
    }
    void warpBrushesUndoSelectionAndAlpha() {
        QFETCH(int, tool);
        auto d = Document::create({64, 64});
        QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x)
                image.setPixelColor(x, y, QColor(255, 0, 0, 128));
        d.addImage("Pixels", image);
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->session().tool = Tool(tool);
        c->session().brushSize = 20;
        c->session().hardness = 1;
        c->session().brushOpacity = 1;
        c->session().selection = QImage(64, 64, QImage::Format_Grayscale8);
        c->session().selection.fill(0);
        for (int y = 20; y < 44; ++y)
            std::fill_n(c->session().selection.scanLine(y) + 20, 24, uchar(255));
        QSignalSpy errors(&page, &EditorPage::error);
        auto center = c->rect().center();
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, center - QPoint(7, 0));
        QTest::mouseMove(c, center + QPoint(7, 0));
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, center + QPoint(7, 0));
        QCOMPARE(errors.count(), 0);
        QCOMPARE(page.history.count(), 1);
        auto after = page.document.active()->image;
        QVERIFY(after != image);
        QCOMPARE(after.pixelColor(50, 32), image.pixelColor(50, 32));
        QCOMPARE(after.pixelColor(30, 10), image.pixelColor(30, 10));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                auto p = after.constScanLine(y) + x * 4;
                QVERIFY(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3]);
            }
        page.history.undo();
        QCOMPARE(page.document.active()->image, image);
        page.history.redo();
        QCOMPARE(page.document.active()->image, after);
    }
    void blurFolderMask() {
        auto d = Document::create({64, 64});
        d.addGroup("Folder");
        d.active()->mask = QImage(64, 64, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        for (int y = 0; y < 64; ++y)
            std::fill_n(d.active()->mask.scanLine(y), 32, uchar(0));
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->session().tool = Tool::Blur;
        c->session().target = EditTarget::Mask;
        c->session().brushSize = 20;
        c->session().blurRadius = 4;
        QSignalSpy errors(&page, &EditorPage::error);
        QTest::mouseClick(c, Qt::LeftButton, Qt::NoModifier, c->rect().center());
        QCOMPARE(errors.count(), 0);
        QVERIFY(page.document.active()->mask.constScanLine(32)[31] > 0);
        QCOMPARE(page.document.active()->mask.constScanLine(0)[60], uchar(255));
        page.history.undo();
        QCOMPARE(page.document.active()->mask.constScanLine(32)[31], uchar(0));
    }
    void rawDevelopmentImportAndCancel() {
        QTemporaryDir dir;
        auto path = dir.filePath("照片.dng");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(dngFixture());
        f.close();
        auto source = RawSource::open(path);
        auto settings = source->asShot();
        settings.exposure = -1.5;
        auto expected = source->develop(settings);
        RawDevelopDialog dialog(path);
        dialog.show();
        auto buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Ok)->isEnabled(), 10000);
        dialog.findChild<QDoubleSpinBox *>("exposureControl")->setValue(2);
        dialog.findChild<QDoubleSpinBox *>("temperatureControl")->setValue(9000);
        buttons->button(QDialogButtonBox::Reset)->click();
        QCOMPARE(dialog.findChild<QDoubleSpinBox *>("exposureControl")->value(), 0.0);
        QCOMPARE(dialog.findChild<QDoubleSpinBox *>("temperatureControl")->value(), 5000.0);
        dialog.findChild<QDoubleSpinBox *>("exposureControl")->setValue(-1.5);
        buttons->button(QDialogButtonBox::Ok)->click();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 10000);
        QCOMPARE(dialog.importedImage(), expected);
        RawDevelopDialog cancel(path);
        cancel.show();
        cancel.reject();
        QVERIFY(cancel.importedImage().isNull());
    }
    void adjustmentChannelEditUndo() {
        QTemporaryDir temp;
        auto d = Document::create({16, 16});
        d.addBlank("Pixels");
        auto path = temp.filePath("Adjustment.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        QAction *create = nullptr, *edit = nullptr;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->text() == "Levels…" && !create)
                create = action;
            if (action->text() == "Edit Adjustment…")
                edit = action;
        }
        QVERIFY(create && edit);
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                visited = true;
                auto spins = dialog->findChildren<QDoubleSpinBox *>();
                if (!spins.isEmpty())
                    spins.first()->setValue(20);
                dialog->accept();
            }
        });
        create->trigger();
        QVERIFY(visited);
        QCOMPARE(page->document.layers.size(), 2);
        auto settings = page->document.active()
                            ->metadata.value("adjustment")
                            .toObject()
                            .value("levels")
                            .toObject();
        QCOMPARE(settings.value("ranges").toArray()[0].toObject().value("black").toInt(), 20);
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                auto combo = dialog->findChild<QComboBox *>();
                auto spins = dialog->findChildren<QDoubleSpinBox *>();
                if (combo && !spins.isEmpty()) {
                    combo->setCurrentText("Blue");
                    spins.first()->setValue(40);
                }
                dialog->accept();
            }
        });
        edit->trigger();
        settings = page->document.active()
                       ->metadata.value("adjustment")
                       .toObject()
                       .value("levels")
                       .toObject();
        QCOMPARE(settings.value("ranges").toArray()[3].toObject().value("black").toInt(), 40);
        QCOMPARE(settings.value("ranges").toArray()[0].toObject().value("black").toInt(), 20);
        page->history.undo();
        settings = page->document.active()
                       ->metadata.value("adjustment")
                       .toObject()
                       .value("levels")
                       .toObject();
        QCOMPARE(settings.value("ranges").toArray()[3].toObject().value("black").toInt(), 0);
        page->history.undo();
        QCOMPARE(page->document.layers.size(), 1);
    }
    void layerEffectEditUndo() {
        QTemporaryDir temp;
        auto d = Document::create({16, 16});
        d.addBlank("Pixels");
        auto path = temp.filePath("Effect.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        QAction *stroke = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Stroke…")
                stroke = action;
        QVERIFY(stroke);
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                auto spins = dialog->findChildren<QDoubleSpinBox *>();
                if (spins.size() > 1)
                    spins[1]->setValue(2);
                dialog->accept();
            }
        });
        stroke->trigger();
        QCOMPARE(page->document.active()
                     ->metadata.value("effects")
                     .toObject()
                     .value("stroke")
                     .toObject()
                     .value("size")
                     .toDouble(),
                 2.0);
        page->history.undo();
        QVERIFY(!page->document.active()->metadata.contains("effects"));
    }
    void richTextUtf16RoundTrip() {
        QJsonObject style{
            {"content", QString::fromUtf8("A😀中文")},
            {"fontName", "Segoe UI"},
            {"fontSize", 24},
            {"red", 0},
            {"green", 0},
            {"blue", 0},
            {"colorRuns",
             QJsonArray{QJsonObject{
                 {"location", 1}, {"length", 2}, {"red", 1}, {"green", 0}, {"blue", 0}}}},
            {"fontRuns",
             QJsonArray{QJsonObject{{"location", 3}, {"length", 2}, {"fontName", "Arial"}}}}};
        QTextDocument document;
        loadTextDocument(document, style);
        auto out = textStyleFromDocument(document, style);
        QCOMPARE(out.value("content"), style.value("content"));
        auto runs = out.value("colorRuns").toArray();
        QCOMPARE(runs.size(), 1);
        QCOMPARE(runs[0].toObject().value("location").toInt(), 1);
        QCOMPARE(runs[0].toObject().value("length").toInt(), 2);
        QVERIFY(!renderText(out).isNull());
    }
    void textEditingUndo() {
        QTemporaryDir temp;
        auto d = Document::create({128, 64});
        QJsonObject style{{"content", "Before"}, {"fontName", "Segoe UI"},
                          {"fontSize", 20},      {"red", 1},
                          {"green", 0},          {"blue", 0}};
        d.addImage("Text", renderText(style));
        d.active()->metadata["text"] = style;
        auto path = temp.filePath("Text.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto page = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        QAction *edit = nullptr;
        for (auto a : window.findChildren<QAction *>())
            if (a->text() == "Edit Text…")
                edit = a;
        QVERIFY(edit);
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                auto text = dialog->findChild<QTextEdit *>();
                if (text) {
                    text->selectAll();
                    text->insertPlainText("After");
                }
                dialog->accept();
            }
        });
        edit->trigger();
        QCOMPARE(
            page->document.active()->metadata.value("text").toObject().value("content").toString(),
            QString("After"));
        page->history.undo();
        QCOMPARE(
            page->document.active()->metadata.value("text").toObject().value("content").toString(),
            QString("Before"));
    }
    void windowLayerSelectionAndOpacity() {
        QTemporaryDir temp;
        auto d = Document::create({32, 32});
        QImage image(32, 32, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        auto first = d.addImage("First", image);
        d.addImage("Second", image);
        auto path = temp.filePath("Window.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        window.show();
        QTest::qWait(30);
        auto tabs = window.findChild<QTabWidget *>();
        auto page = qobject_cast<EditorPage *>(tabs->currentWidget());
        auto tree = window.findChild<QTreeWidget *>();
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 2);
        auto item = tree->topLevelItem(1);
        auto target = tree->visualItemRect(item).center();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        QCOMPARE(page->document.activeId(), first);
        QDoubleSpinBox *opacity = nullptr;
        for (auto spin : window.findChildren<QDoubleSpinBox *>())
            if (spin->suffix().contains("opacity"))
                opacity = spin;
        QVERIFY(opacity);
        opacity->setValue(50);
        QMetaObject::invokeMethod(opacity, "editingFinished", Qt::DirectConnection);
        QCOMPARE(page->document.active()->opacity(), 0.5);
        page->history.undo();
        QCOMPARE(page->document.active()->opacity(), 1.0);
    }
    void filterPreviewCancelPreservesDocument() {
        QTemporaryDir temp;
        auto d = Document::create({32, 32});
        QImage image(32, 32, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(100, 120, 140));
        d.addImage("Image", image);
        auto path = temp.filePath("Filter.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto tabs = window.findChild<QTabWidget *>();
        auto page = qobject_cast<EditorPage *>(tabs->currentWidget());
        auto before = page->document.active()->image;
        QAction *exposure = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text().startsWith("Exposure"))
                exposure = action;
        QVERIFY(exposure);
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                visited = true;
                auto spins = dialog->findChildren<QDoubleSpinBox *>();
                if (!spins.isEmpty())
                    spins.first()->setValue(2);
                QTimer::singleShot(150, dialog, &QDialog::reject);
            }
        });
        exposure->trigger();
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 0);
        QCOMPARE(page->document.active()->image, before);
    }
    void brushStrokeUndoRedo() {
        auto document = Document::create({64, 64});
        document.addBlank("Paint");
        EditorPage page(document);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto canvas = page.canvas;
        canvas->zoom = 1;
        canvas->session().tool = Tool::Brush;
        canvas->session().brushSize = 10;
        canvas->session().hardness = 1;
        canvas->session().foreground = Qt::red;
        QSignalSpy errors(&page, &EditorPage::error);
        auto center = canvas->rect().center();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseMove(canvas, center + QPoint(10, 0));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, center + QPoint(10, 0));
        QCOMPARE(errors.count(), 0);
        QCOMPARE(page.history.count(), 1);
        QVERIFY(page.document.active()->image.pixelColor(32, 32).alpha() > 0);
        page.history.undo();
        QCOMPARE(page.document.active()->image.pixelColor(32, 32).alpha(), 0);
        page.history.redo();
        QVERIFY(page.document.active()->image.pixelColor(32, 32).alpha() > 0);
    }
    void eraseAndMaskStroke() {
        auto d = Document::create({64, 64});
        QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        d.addImage("Image", image);
        d.active()->mask = QImage(64, 64, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->session().tool = Tool::Erase;
        c->session().target = EditTarget::Mask;
        c->session().brushSize = 20;
        c->session().hardness = 1;
        auto center = c->rect().center();
        QTest::mouseClick(c, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(page.document.active()->mask.constScanLine(32)[32], uchar(0));
        QCOMPARE(page.document.active()->image.pixelColor(32, 32), QColor(Qt::red));
        page.history.undo();
        QCOMPARE(page.document.active()->mask.constScanLine(32)[32], uchar(255));
    }
    void selectionInDocumentCoordinates() {
        auto d = Document::create({64, 64});
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 2;
        c->session().tool = Tool::RectangleSelect;
        auto center = c->rect().center();
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, center - QPoint(20, 20));
        QTest::mouseMove(c, center + QPoint(20, 20));
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, center + QPoint(20, 20));
        QVERIFY(!c->session().selection.isNull());
        QVERIFY(c->session().selection.constScanLine(32)[32] > 0);
        QCOMPARE(c->session().selection.constScanLine(0)[0], uchar(0));
        QCOMPARE(page.history.count(), 1);
        auto selected = page.session.selection;
        page.history.undo();
        QVERIFY(page.session.selection.isNull());
        page.history.redo();
        QCOMPARE(page.session.selection, selected);
    }
    void selectionHistoryDoesNotDirtySavedPixels() {
        auto d = Document::create({32, 32});
        d.addBlank("Pixels");
        EditorPage page(d);
        auto c = page.canvas;
        c->selectAll();
        QCOMPARE(page.history.count(), 1);
        QVERIFY(!page.isModified());
        auto full = page.session.selection;
        c->invertSelection();
        auto empty = page.session.selection;
        QCOMPARE(empty.constScanLine(0)[0], uchar(0));
        page.history.undo();
        QCOMPARE(page.session.selection, full);
        page.history.redo();
        QCOMPARE(page.session.selection, empty);
        c->clearSelection();
        QVERIFY(page.session.selection.isNull());
        page.history.undo();
        QCOMPARE(page.session.selection, empty);
        page.history.undo();
        QCOMPARE(page.session.selection, full);
        page.edit("Fill", [](Document &doc) { doc.active()->image.fill(Qt::red); });
        QVERIFY(page.isModified());
        auto saved = page.contentState;
        page.markSaved(saved);
        QVERIFY(!page.isModified());
        c->clearSelection();
        QVERIFY(!page.isModified());
        page.history.undo();
        QVERIFY(!page.isModified());
        page.history.undo();
        QVERIFY(page.isModified());
        page.history.redo();
        QVERIFY(!page.isModified());
    }
    void featherAndResizeUndoRestoreSelection() {
        EditorPage page(Document::create({32, 32}));
        QImage mask(32, 32, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 8; y < 24; ++y)
            std::fill_n(mask.scanLine(y) + 8, 16, uchar(255));
        page.canvas->replaceSelection(mask, "Select");
        page.canvas->featherSelection(2);
        auto feathered = page.session.selection;
        QVERIFY(feathered != mask);
        page.history.undo();
        QCOMPARE(page.session.selection, mask);
        page.history.redo();
        QCOMPARE(page.session.selection, feathered);
        page.edit("Resize", [](Document &d) { d.metadata["width"] = 16; });
        QVERIFY(page.session.selection.isNull());
        page.history.undo();
        QCOMPARE(page.document.size(), QSize(32, 32));
        QCOMPARE(page.session.selection, feathered);
        page.history.redo();
        QCOMPARE(page.document.size(), QSize(16, 32));
        QVERIFY(page.session.selection.isNull());
    }
    void backgroundSaveTracksContentRatherThanSelection() {
        QTemporaryDir dir;
        auto d = Document::create({32, 32});
        d.addBlank("Pixels");
        auto path = dir.filePath("后台保存.comp");
        saveProject(d, path);
        EditorWindow window;
        window.openPath(path);
        auto p = qobject_cast<EditorPage *>(window.findChild<QTabWidget *>()->currentWidget());
        QAction *save = nullptr;
        for (auto a : window.findChildren<QAction *>())
            if (a->text() == "Save Project")
                save = a;
        QVERIFY(save);
        p->edit("Red", [](Document &doc) { doc.active()->image.fill(Qt::red); });
        save->trigger();
        QVERIFY(p->saving);
        p->canvas->selectAll();
        QTRY_VERIFY_WITH_TIMEOUT(!p->saving, 5000);
        QVERIFY(!p->isModified());
        QCOMPARE(loadProject(path).active()->image.pixelColor(0, 0), QColor(Qt::red));
        p->edit("Green", [](Document &doc) { doc.active()->image.fill(Qt::green); });
        save->trigger();
        QVERIFY(p->saving);
        p->edit("Blue", [](Document &doc) { doc.active()->image.fill(Qt::blue); });
        QTRY_VERIFY_WITH_TIMEOUT(!p->saving, 5000);
        QVERIFY(p->isModified());
        QCOMPARE(loadProject(path).active()->image.pixelColor(0, 0), QColor(Qt::green));
        p->history.undo();
        QVERIFY(!p->isModified());
        QCOMPARE(p->document.active()->image.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(!p->document.manifest().contains("selection"));
    }
    void temporaryPanReturnsToBrush() {
        auto d = Document::create({64, 64});
        d.addBlank("Pixels");
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->setTool(Tool::Brush);
        auto center = c->rect().center();
        QTest::keyPress(c, Qt::Key_Space);
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseMove(c, center + QPoint(20, 0));
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, center + QPoint(20, 0));
        QTest::keyRelease(c, Qt::Key_Space);
        QCOMPARE(page.history.count(), 0);
        QVERIFY(!page.interacting());
        QCOMPARE(page.session.tool, Tool::Brush);
        QTest::mouseClick(c, Qt::LeftButton, Qt::NoModifier, center + QPoint(20, 0));
        QCOMPARE(page.history.count(), 1);
        QVERIFY(page.document.active()->image.pixelColor(32, 32).alpha() > 0);
    }
    void escapeCancelsStrokeAndSelection() {
        auto d = Document::create({64, 64});
        d.addBlank("Pixels");
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->setTool(Tool::Brush);
        auto image = page.document.active()->image;
        auto center = c->rect().center();
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, center);
        QVERIFY(page.interacting());
        QTest::keyClick(c, Qt::Key_Escape);
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(page.document.active()->image, image);
        QCOMPARE(page.history.count(), 0);
        QVERIFY(!page.interacting());
        c->setTool(Tool::RectangleSelect);
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, center - QPoint(10, 10));
        QTest::mouseMove(c, center + QPoint(10, 10));
        QTest::keyClick(c, Qt::Key_Escape);
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, center + QPoint(10, 10));
        QVERIFY(page.session.selection.isNull());
        QCOMPARE(page.history.count(), 0);
    }
    void perProjectSessionSurvivesTabSwitching() {
        EditorWindow window;
        auto tabs = window.findChild<QTabWidget *>();
        auto first = qobject_cast<EditorPage *>(tabs->currentWidget());
        first->session.foreground = Qt::red;
        first->session.background = Qt::green;
        first->session.brushSize = 77;
        first->session.target = EditTarget::Mask;
        first->canvas->selectAll();
        QAction *demo = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Open Demo")
                demo = action;
        QVERIFY(demo);
        demo->trigger();
        auto second = qobject_cast<EditorPage *>(tabs->currentWidget());
        QVERIFY(second && second != first);
        QVERIFY(second->session.selection.isNull());
        QCOMPARE(second->session.target, EditTarget::Pixels);
        second->session.foreground = Qt::blue;
        tabs->setCurrentWidget(first);
        QCOMPARE(first->session.foreground, QColor(Qt::red));
        QCOMPARE(first->session.background, QColor(Qt::green));
        QCOMPARE(first->session.brushSize, 77.0);
        QCOMPARE(first->session.target, EditTarget::Mask);
        QVERIFY(!first->session.selection.isNull());
        QTest::keyClick(first->canvas, Qt::Key_X);
        QCOMPARE(first->session.foreground, QColor(Qt::green));
        QCOMPARE(first->session.background, QColor(Qt::red));
        QTest::keyClick(first->canvas, Qt::Key_D);
        QCOMPARE(first->session.foreground, QColor(Qt::black));
        QCOMPARE(first->session.background, QColor(Qt::white));
        QCOMPARE(second->session.foreground, QColor(Qt::blue));
    }
    void failedEditRollsBack() {
        auto d = Document::create({4, 4});
        EditorPage page(d);
        auto before = page.document.manifest();
        QSignalSpy errors(&page, &EditorPage::error);
        page.edit("Bad edit", [](Document &document) { document.metadata["width"] = 0; });
        QCOMPARE(errors.count(), 1);
        QCOMPARE(page.document.manifest(), before);
        QCOMPARE(page.history.count(), 0);
    }
    // One-pixel vertical stripes: anything but full resolution blurs them together.
    static QImage stripes(QSize size, QColor even, QColor odd) {
        QImage image(size, QImage::Format_RGBA8888_Premultiplied),
            row(size.width(), 1, image.format());
        for (int x = 0; x < size.width(); ++x)
            row.setPixelColor(x, 0, x % 2 ? odd : even);
        for (int y = 0; y < size.height(); ++y)
            std::memcpy(image.scanLine(y), row.constScanLine(0), size_t(row.bytesPerLine()));
        return image;
    }
    // Where document pixel `x`, `y` is on a canvas at its current zoom, with no pan.
    static QPoint onCanvas(const Canvas *c, QSize document, int x, int y) {
        return {
            int(std::floor((c->width() - document.width() * c->zoom) / 2 + (x + .5) * c->zoom)),
            int(std::floor((c->height() - document.height() * c->zoom) / 2 + (y + .5) * c->zoom))};
    }
    // The canvas once its background rendering has caught up.
    static QImage rendered(Canvas *c) {
        // Pixel comparisons exclude pointer and transform overlays; CanvasTests covers them.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(c, &leave);
        const bool controls = c->session().view.transformControls;
        c->session().view.transformControls = false;
        auto shot = c->grab().toImage();
        for (int i = 0; i < 20 && c->rendering(); ++i) {
            c->waitForRendering();
            shot = c->grab().toImage();
        }
        c->session().view.transformControls = controls;
        return shot;
    }
    void backgroundRenderingDiscardsOutdatedTiles() {
        const QSize size(3000, 2000);
        auto d = Document::create(size);
        d.addImage("Stripes", stripes(size, Qt::black, Qt::white));
        EditorPage page(d);
        page.resize(400, 300);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        rendered(c);
        c->zoom = 1;
        c->grab();
        QVERIFY(c->rendering());
        // Changed before those tiles are taken in, so they show the old pixels.
        page.document.active()->image.fill(Qt::red);
        c->refresh();
        QCOMPARE(rendered(c).pixelColor(onCanvas(c, size, 1500, 1000)), QColor(Qt::red));
    }
    void canvasShowsLargeDocumentsAtFullResolution() {
        const QSize size(4000, 3000);
        auto d = Document::create(size);
        d.addImage("Stripes", stripes(size, Qt::black, Qt::white));
        EditorPage page(d);
        page.resize(400, 300);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        auto shot = rendered(c);
        for (int x : {1999, 2000, 2001})
            QCOMPARE(shot.pixelColor(onCanvas(c, size, x, 1500)),
                     QColor(x % 2 ? Qt::white : Qt::black));
    }
    void canvasRedrawsWhatABrushTouches() {
        auto d = Document::create({64, 64});
        d.addBlank("Paint");
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->session().tool = Tool::Brush;
        c->session().brushSize = 10;
        c->session().hardness = 1;
        c->session().foreground = Qt::red;
        const auto point = onCanvas(c, {64, 64}, 32, 32), outside = onCanvas(c, {64, 64}, 5, 5);
        const auto before = rendered(c);
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(c, point + QPoint(6, 0));
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, point + QPoint(6, 0));
        auto painted = rendered(c);
        QCOMPARE(painted.pixelColor(point), QColor(Qt::red));
        QCOMPARE(painted.pixelColor(point + QPoint(6, 0)), QColor(Qt::red));
        QCOMPARE(painted.pixelColor(outside), before.pixelColor(outside));
        page.history.undo();
        QCOMPARE(rendered(c).pixelColor(point), before.pixelColor(point));
        page.history.redo();
        QCOMPARE(rendered(c).pixelColor(point), QColor(Qt::red));
    }
    void canvasRedrawsAMovedLayer() {
        auto d = Document::create({64, 64});
        QImage square(10, 10, QImage::Format_RGBA8888_Premultiplied);
        square.fill(Qt::red);
        d.addImage("Square", square);
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = 1;
        c->session().tool = Tool::Move;
        const auto from = onCanvas(c, {64, 64}, 5, 5), to = onCanvas(c, {64, 64}, 35, 35);
        const auto empty = rendered(c).pixelColor(to);
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(c, from + QPoint(15, 15));
        QTest::mouseMove(c, to);
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, to);
        auto moved = rendered(c);
        QCOMPARE(moved.pixelColor(to), QColor(Qt::red));
        QCOMPARE(moved.pixelColor(from), empty);
        page.history.undo();
        auto undone = rendered(c);
        QCOMPARE(undone.pixelColor(from), QColor(Qt::red));
        QCOMPARE(undone.pixelColor(to), empty);
    }
    void strokeMatchesFreshRender_data() {
        QTest::addColumn<bool>("mask");
        QTest::addColumn<double>("zoom");
        QTest::addColumn<bool>("effects");
        QTest::newRow("pixels at 25%") << false << .25 << false;
        QTest::newRow("mask at 25%") << true << .25 << false;
        QTest::newRow("pixels with effects") << false << 1.0 << true;
        QTest::newRow("mask with effects") << true << 1.0 << true;
        QTest::newRow("pixels with effects at 25%") << false << .25 << true;
    }
    // A stroke updates the halved images, mask and effects it draws from, and redraws every tile
    // they reach (a shadow falls past the brush), so the result matches a fresh drawing.
    void strokeMatchesFreshRender() {
        QFETCH(bool, mask);
        QFETCH(double, zoom);
        QFETCH(bool, effects);
        auto d = Document::create({1200, 1200});
        d.addImage("Stripes", stripes({1200, 1200}, Qt::darkCyan, Qt::yellow));
        d.active()->mask = QImage(1200, 1200, QImage::Format_Grayscale8);
        d.active()->mask.fill(255);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        if (effects) {
            QImage disc(1200, 1200, QImage::Format_RGBA8888_Premultiplied);
            disc.fill(Qt::transparent);
            QPainter p(&disc);
            p.setBrush(Qt::darkCyan);
            p.drawEllipse(QPoint(600, 600), 150, 150);
            p.end();
            d.active()->image = disc;
            d.active()->metadata["effects"] =
                QJsonObject{{"shadow", QJsonObject{{"distance", 60}, {"angle", 0}, {"blur", 6}}},
                            {"outerGlow", QJsonObject{{"size", 8}}}};
        }
        EditorPage page(d);
        page.resize(400, 400);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = zoom;
        c->session().tool = mask ? Tool::Erase : Tool::Brush;
        c->session().target = mask ? EditTarget::Mask : EditTarget::Pixels;
        c->session().brushSize = 60;
        c->session().hardness = 1;
        c->session().foreground = Qt::red;
        rendered(c);
        // Within the tile that starts at 512; the shadow, 60 pixels to the left, falls in the one
        // before.
        const auto from = onCanvas(c, {1200, 1200}, 545, 600);
        QTest::mousePress(c, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 4; ++step) {
            QTest::mouseMove(c, from + QPoint(step * 8, 0));
            c->grab();
        }
        QTest::mouseRelease(c, Qt::LeftButton, Qt::NoModifier, from + QPoint(32, 0));
        const auto painted = rendered(c);
        auto copy = page.document;
        copy.active()->image = copy.active()->image.copy();
        copy.active()->mask = copy.active()->mask.copy();
        EditorPage fresh(copy);
        fresh.resize(400, 400);
        fresh.show();
        QTest::qWait(30);
        fresh.canvas->zoom = zoom;
        QCOMPARE(rendered(fresh.canvas), painted);
    }
    void eyedropperReadsFullResolution() {
        const QSize size(4000, 3000);
        auto d = Document::create(size);
        d.addImage("Stripes", stripes(size, Qt::red, Qt::blue));
        EditorPage page(d);
        page.resize(400, 300);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = .05;
        c->session().tool = Tool::Eyedropper;
        QSignalSpy picked(c, &Canvas::colorPicked);
        QTest::mouseClick(c, Qt::LeftButton, Qt::NoModifier, onCanvas(c, size, 2001, 1500));
        QCOMPARE(picked.count(), 1);
        const auto color = picked.first().first().value<QColor>();
        QVERIFY(color == QColor(Qt::red) || color == QColor(Qt::blue));
    }
    // Whole-canvas edits (flip, crop, canvas size) change what every tile shows, not one layer's.
    void canvasFollowsCanvasOperations() {
        const QSize size(600, 400);
        auto d = Document::create(size);
        QImage halves(size, QImage::Format_RGBA8888_Premultiplied);
        halves.fill(Qt::blue);
        QPainter(&halves).fillRect(0, 0, size.width() / 2, size.height(), Qt::red);
        d.addImage("Halves", halves);
        EditorPage page(d);
        page.resize(400, 300);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        c->zoom = .5;
        auto at = [&](int x, int y) {
            return rendered(c).pixelColor(onCanvas(c, page.document.size(), x, y));
        };
        QCOMPARE(at(100, 200), QColor(Qt::red));
        QCOMPARE(at(500, 200), QColor(Qt::blue));
        page.edit("Flip", [](Document &document) { flipCanvas(document, true); });
        QCOMPARE(at(100, 200), QColor(Qt::blue));
        QCOMPARE(at(500, 200), QColor(Qt::red));
        page.edit("Crop",
                  [](Document &document) { cropCanvas(document, QRect(300, 0, 300, 400)); });
        QCOMPARE(page.document.size(), QSize(300, 400));
        QCOMPARE(at(10, 200), QColor(Qt::red));
        QCOMPARE(at(290, 200), QColor(Qt::red));
        page.edit("Canvas size", [](Document &document) {
            resizeCanvas(document, {600, 400}, 4, QColor(Qt::green));
        });
        QCOMPARE(page.document.size(), QSize(600, 400));
        // Cropping keeps the layer's pixels outside the canvas, so its flipped blue half shows
        // again on the left; only beyond the layer is there extension fill.
        QCOMPARE(at(10, 200), QColor(Qt::blue));
        QCOMPARE(at(300, 200), QColor(Qt::red));
        QCOMPARE(at(590, 200), QColor(Qt::green));
        page.history.undo();
        QCOMPARE(page.document.size(), QSize(300, 400));
        QCOMPARE(at(150, 200), QColor(Qt::red));
    }
    // Destroying a modified page clears its undo stack, which must not tell the window that owns
    // it, already tearing down, to refresh its panels.
    void destroyingAModifiedPageDoesNotNotifyItsWindow() {
        auto d = Document::create({8, 8});
        d.addBlank("Layer");
        auto page = new EditorPage(d);
        page->edit("Rename", [](Document &document) { document.active()->metadata["name"] = "Renamed"; });
        QVERIFY(page->isModified());
        int notified = 0;
        connect(page, &EditorPage::documentChanged, this, [&] { ++notified; });
        delete page;
        QCOMPARE(notified, 0);
    }
    void pixelGridAppearsAt800Percent() {
        auto d = Document::create({16, 16});
        QImage white(16, 16, QImage::Format_RGBA8888_Premultiplied);
        white.fill(Qt::white);
        d.addImage("White", white);
        EditorPage page(d);
        page.resize(320, 320);
        page.show();
        QTest::qWait(30);
        auto c = page.canvas;
        auto darkest = [&](double zoom) {
            c->zoom = zoom;
            auto shot = rendered(c);
            // Pixels 7 and 8 meet at the middle of the canvas.
            const int edge = int(std::lround((c->width() - 16 * zoom) / 2 + 8 * zoom));
            int value = 255;
            for (int x = edge - 1; x <= edge; ++x)
                value = std::min(value, shot.pixelColor(x, c->height() / 2 + 3).red());
            return value;
        };
        QVERIFY(darkest(10) < 255);
        QCOMPARE(darkest(4), 255);
    }
};
QTEST_MAIN(UiTests)
#include "ui_tests.moc"

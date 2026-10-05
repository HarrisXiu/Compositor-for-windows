// SPDX-License-Identifier: MIT
#include "adjustment_panels.h"
#include "adjustment_tools.h"
#include "curve_editor.h"
#include "editor.h"
#include "filters.h"
#include "language.h"
#include "live_dialog.h"
#include "parameter_control.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
using namespace compositor;
namespace {
QImage filled(QSize size, QColor color) {
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(color);
    return image;
}
// Gray values spread from 50 to 200 across the width.
QImage ramp() {
    QImage image(151, 4, QImage::Format_RGBA8888_Premultiplied);
    for (int x = 0; x < 151; ++x)
        for (int y = 0; y < 4; ++y)
            image.setPixelColor(x, y, QColor(50 + x, 50 + x, 50 + x));
    return image;
}
double level(const QJsonArray &ranges, int c, const char *key) {
    return ranges[c].toObject().value(QLatin1String(key)).toDouble();
}
EditorWindow *openWindow(QTemporaryDir &dir, const QImage &image) {
    auto d = Document::create(image.size());
    d.addImage("Photo", image);
    const auto path = dir.filePath("Panels.comp");
    saveProject(d, path);
    auto window = new EditorWindow;
    window->openPath(path);
    window->show();
    return window;
}
EditorPage *currentPage(EditorWindow &w) {
    return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
}
QAction *imageAdjustment(EditorWindow &w, const QString &title) {
    QSet<QAction *> layerEntries;
    for (auto menu : w.findChildren<QMenu *>())
        if (menu->title() == "New Adjustment Layer")
            for (auto a : menu->actions())
                layerEntries.insert(a);
    for (auto a : w.findChildren<QAction *>())
        if (a->text() == title && !layerEntries.contains(a))
            return a;
    return nullptr;
}
// Where document point (x, y) is on the canvas widget.
QPoint onCanvas(EditorPage &p, double x, double y) {
    const double inset = p.session.view.rulers ? 24 : 0;
    const auto size = p.document.size();
    return QPoint(qRound((p.canvas->width() + inset - size.width() * p.canvas->zoom) / 2 + x * p.canvas->zoom),
                  qRound((p.canvas->height() + inset - size.height() * p.canvas->zoom) / 2 + y * p.canvas->zoom));
}
// The pointer moving over a widget with no button down, delivered to it directly.
void hover(QWidget *widget, QPoint position) {
    QMouseEvent e(QEvent::MouseMove, QPointF(position), QPointF(widget->mapToGlobal(position)),
                  Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &e);
}
// Runs `check` inside the dialog `action` opens, then closes the dialog with OK or Cancel.
void withDialog(QAction *action, bool accept, const std::function<void(QDialog *)> &check, bool *visited) {
    QTimer::singleShot(20, [=] {
        auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
        QVERIFY(dialog);
        auto close = qScopeGuard([dialog, accept] {
            if (dialog->isVisible())
                accept ? dialog->accept() : dialog->reject();
        });
        check(dialog);
        *visited = true;
    });
    action->trigger();
}
} // namespace
class AdjustmentPanelTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void histogramsWeighByAlphaAndSelection() {
        auto image = filled({4, 1}, Qt::transparent);
        image.setPixelColor(0, 0, QColor(255, 0, 0));
        image.setPixelColor(1, 0, QColor(0, 255, 0));
        image.setPixelColor(2, 0, QColor(255, 255, 255, 128)); // Half opaque: half the weight.
        const auto h = levelsHistogram(image);
        const double half = 128 / 255.0;
        QCOMPARE(h[1][255], 1 + half);
        QCOMPARE(h[1][0], 1.0);
        QCOMPARE(h[2][255], 1 + half);
        // RGB is the mean of the three channels.
        QVERIFY(std::abs(h[0][255] - (1 + 1 + 3 * half) / 3.0) < 1e-9);
        QImage coverage(4, 1, QImage::Format_Grayscale8);
        coverage.fill(0);
        coverage.scanLine(0)[0] = 255;
        const auto selected = levelsHistogram(image, coverage);
        QCOMPARE(selected[1][255], 1.0);
        QCOMPARE(selected[2][255], 0.0);
    }
    void aSpikeDoesNotFlattenTheRest() {
        std::array<double, 256> bins{};
        for (int i = 1; i < 255; ++i)
            bins[i] = 10;
        bins[0] = 100000;
        QCOMPARE(histogramScale(bins), 40.0);
        bins.fill(0);
        QCOMPARE(histogramScale(bins), 0.0);
    }
    void autoLevelsStretchTheUsedRange() {
        const auto h = levelsHistogram(ramp());
        const auto contrast = autoLevels(h, AutoLevels::Contrast);
        QCOMPARE(level(contrast, 0, "black"), 50.0);
        QCOMPARE(level(contrast, 0, "white"), 200.0);
        QCOMPARE(level(contrast, 1, "black"), 0.0);
        const auto color = autoLevels(h, AutoLevels::Color);
        for (int c = 1; c <= 3; ++c) {
            QCOMPARE(level(color, c, "black"), 50.0);
            QCOMPARE(level(color, c, "white"), 200.0);
        }
        QCOMPARE(level(color, 0, "white"), 255.0);
        // A dark picture: neutral midtones lifts its mean to the middle.
        QImage dark(100, 1, QImage::Format_RGBA8888_Premultiplied);
        for (int x = 0; x < 100; ++x)
            dark.setPixelColor(x, 0, x < 90 ? QColor(40, 40, 40) : QColor(220, 220, 220));
        const auto neutral = autoLevels(levelsHistogram(dark), AutoLevels::NeutralMidtones);
        QVERIFY(level(neutral, 1, "gamma") > 1.2);
    }
    void eyedroppersMapTheSampledColor() {
        QJsonArray ranges{identityLevels(), identityLevels(), identityLevels(), identityLevels()};
        auto black = sampleLevels(ranges, QColor(30, 40, 50), LevelsSample::Black);
        QCOMPARE(level(black, 1, "black"), 30.0);
        QCOMPARE(level(black, 3, "black"), 50.0);
        auto white = sampleLevels(black, QColor(200, 210, 220), LevelsSample::White);
        QCOMPARE(level(white, 2, "white"), 210.0);
        QCOMPARE(level(white, 1, "black"), 30.0);
        // Gray makes the sampled color come out neutral.
        auto gray = sampleLevels(white, QColor(120, 100, 140), LevelsSample::Gray);
        for (int c = 1; c <= 3; ++c) {
            const double sampled = (c == 1 ? 120 : c == 2 ? 100 : 140) / 255.0;
            QVERIFY(std::abs(levelsMap(sampled, gray[c].toObject()) - .5) < .01);
        }
        auto filtered = applyFilter(filled({1, 1}, QColor(120, 100, 140)), "Levels",
                                    {{"ranges", gray}});
        const auto out = filtered.pixelColor(0, 0);
        QVERIFY(std::abs(out.red() - out.green()) <= 2 && std::abs(out.green() - out.blue()) <= 2);
    }
    void hueBandsKeepTheirHandlesInOrder() {
        const auto reds = defaultHueBand("Reds");
        QCOMPARE(reds.value("falloffStart").toDouble(), 315.0);
        QCOMPARE(reds.value("rangeStart").toDouble(), 345.0);
        QCOMPARE(reds.value("rangeEnd").toDouble(), 15.0);
        QCOMPARE(reds.value("falloffEnd").toDouble(), 45.0);
        QCOMPARE(nearestHueRange(110), QString("Greens"));
        QCOMPARE(nearestHueRange(350), QString("Reds"));
        // A handle can't cross its neighbor.
        auto moved = moveHueHandle(reds, 1, 40);
        QCOMPARE(moved.value("rangeStart").toDouble(), 15.0);
        moved = moveHueHandle(reds, 3, 90);
        QCOMPARE(moved.value("falloffEnd").toDouble(), 90.0);
        auto wide = widenHueBand(reds, 30);
        QCOMPARE(wide.value("rangeEnd").toDouble(), 30.0);
        QCOMPARE(wide.value("falloffEnd").toDouble(), 60.0);
        auto narrow = narrowHueBand(reds, 10);
        QCOMPARE(narrow.value("rangeEnd").toDouble(), 9.0);
        QCOMPARE(narrow.value("falloffEnd").toDouble(), 10.0);
    }
    void aBandLimitsWhichColorsChange() {
        QImage image(2, 1, QImage::Format_RGBA8888_Premultiplied);
        image.setPixelColor(0, 0, QColor(220, 30, 30));
        image.setPixelColor(1, 0, QColor(30, 200, 30));
        QJsonObject settings{{"range", "Greens"},
                             {"adjustments", QJsonObject{{"Greens", QJsonObject{{"saturation", -100}}}}},
                             {"bands", QJsonObject{{"Greens", hueBandAround(120)}}}};
        auto out = applyFilter(image, "Hue/Saturation", settings);
        QCOMPARE(out.pixelColor(0, 0), QColor(220, 30, 30));
        QVERIFY(out.pixelColor(1, 0).hslSaturationF() < .05);
        settings["invertRange"] = true;
        out = applyFilter(image, "Hue/Saturation", settings);
        QVERIFY(out.pixelColor(0, 0).hslSaturationF() < .05);
        QVERIFY(out.pixelColor(1, 0).hslSaturationF() > .5);
    }
    void levelsGraphHandlesReportValues() {
        LevelsGraph graph;
        graph.resize(380, 214);
        QString key;
        double value = -1;
        graph.changed = [&](const QString &k, double v) {
            key = k;
            value = v;
            if (k == "inputBlack")
                graph.black = v;
        };
        graph.show();
        const int y = int(graph.inputStrip().center().y());
        QTest::mousePress(&graph, Qt::LeftButton, {}, QPoint(int(graph.xOf(0)), y));
        QTest::mouseMove(&graph, QPoint(int(graph.xOf(64)), y));
        QTest::mouseRelease(&graph, Qt::LeftButton, {}, QPoint(int(graph.xOf(64)), y));
        QCOMPARE(key, QString("inputBlack"));
        QVERIFY(std::abs(value - 64) <= 1);
        // The middle handle sets gamma: dragged left of the middle it brightens (gamma > 1).
        graph.black = 0;
        graph.white = 255;
        QTest::mousePress(&graph, Qt::LeftButton, {}, QPoint(int(graph.xOf(graph.gammaPosition())), y));
        QTest::mouseMove(&graph, QPoint(int(graph.xOf(64)), y));
        QCOMPARE(key, QString("gamma"));
        QVERIFY(value > 1.5 && value < 2.5);
        QTest::mouseRelease(&graph, Qt::LeftButton, {}, QPoint(int(graph.xOf(64)), y));
        const int output = int(graph.outputStrip().bottom() - 6);
        QTest::mousePress(&graph, Qt::LeftButton, {}, QPoint(int(graph.xOf(255)), output));
        QTest::mouseMove(&graph, QPoint(int(graph.xOf(200)), output));
        QCOMPARE(key, QString("outputWhite"));
        QVERIFY(std::abs(value - 200) <= 1);
    }

    void levelsDialogShowsTheHistogramAndAutoAndEyedroppersWork() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, ramp()));
        auto page = currentPage(*window);
        bool visited = false;
        withDialog(imageAdjustment(*window, "Levels…"), true, [&](QDialog *dialog) {
            auto graph = static_cast<LevelsGraph *>(dialog->findChild<QWidget *>("levelsGraph"));
            QVERIFY(graph);
            QTRY_VERIFY_WITH_TIMEOUT(graph->histogramReady, 5000);
            QVERIFY(graph->histogram[0][50] > 0 && graph->histogram[0][20] == 0);
            auto contrast = dialog->findChild<QPushButton *>("levelsAutoContrast");
            QVERIFY(contrast && contrast->isEnabled());
            contrast->click();
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("inputBlackControl")->value(), 50.0);
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("inputWhiteControl")->value(), 200.0);
            QCOMPARE(graph->black, 50.0);
            // The white eyedropper, clicked on the canvas, makes that pixel white.
            dialog->findChild<QPushButton *>("levelsSampleWhite")->click();
            QTest::mouseClick(page->canvas, Qt::LeftButton, {}, onCanvas(*page, 100.5, 2.5));
            auto channel = dialog->findChild<QComboBox *>();
            QVERIFY(channel);
            channel->setCurrentIndex(1);
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("inputWhiteControl")->value(), 150.0);
            QCOMPARE(graph->channel, 1);
            QCOMPARE(page->history.count(), 0);
        }, &visited);
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 1);
        // Gray 150 is now white; 200 stays white.
        QCOMPARE(page->document.active()->image.pixelColor(100, 2), QColor(255, 255, 255));
        // The eyedropper no longer takes the canvas.
        page->canvas->setTool(Tool::Brush);
        QTest::mouseClick(page->canvas, Qt::LeftButton, {}, onCanvas(*page, 10.5, 2.5));
        QCOMPARE(page->history.count(), 2);
    }
    void colorBalanceBlackWhiteAndGradientMapShowTheirColors() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, ramp()));
        auto page = currentPage(*window);
        const auto track = [](QDialog *dialog, const QString &key) {
            auto parameter = dialog->findChild<QWidget *>(key + "ControlParameter");
            return parameter ? static_cast<ParameterControl *>(parameter)->slider()->styleSheet() : QString();
        };
        bool visited = false;
        // Color Balance: three titled tonal ranges, each axis colored from one color to its opposite.
        withDialog(imageAdjustment(*window, "Color Balance…"), false, [&](QDialog *dialog) {
            QStringList titles;
            for (auto label : dialog->findChildren<QLabel *>())
                titles << label->text();
            for (auto heading : {"Shadows", "Midtones", "Highlights"})
                QVERIFY2(titles.contains(heading), heading);
            for (auto range : {"shadow", "mid", "highlight"})
                for (auto axis : {"CyanRed", "MagentaGreen", "YellowBlue"})
                    QVERIFY(track(dialog, QString(range) + axis).contains("qlineargradient"));
            QVERIFY(track(dialog, "midCyanRed").contains(QColor::fromRgbF(0.86f, 0.18f, 0.20f).name()));
        }, &visited);
        QVERIFY(visited);
        // Black & White: each family runs dark to light in its hue; Tint's controls show only while on.
        visited = false;
        withDialog(imageAdjustment(*window, "Black & White…"), false, [&](QDialog *dialog) {
            for (auto key : {"reds", "yellows", "greens", "cyans", "blues", "magentas"})
                QVERIFY(track(dialog, key).contains("qlineargradient"));
            auto hue = dialog->findChild<QWidget *>("tintHueControlParameter");
            auto tint = dialog->findChild<QCheckBox *>("tintControl");
            QVERIFY(hue && tint && !tint->isChecked());
            QVERIFY(!hue->isVisibleTo(dialog));
            tint->setChecked(true);
            QVERIFY(hue->isVisibleTo(dialog));
            const auto before = track(dialog, "tintSaturation");
            dialog->findChild<QDoubleSpinBox *>("tintHueControl")->setValue(200);
            QVERIFY(track(dialog, "tintSaturation") != before);
        }, &visited);
        QVERIFY(visited);
        // Gradient Map: a bar from the shadow end to the highlight end, swapped by Reverse.
        visited = false;
        withDialog(imageAdjustment(*window, "Gradient Map…"), false, [&](QDialog *dialog) {
            auto bar = dialog->findChild<QFrame *>("gradientMapBar");
            QVERIFY(bar);
            QVERIFY(bar->styleSheet().contains("stop:0 #000000,stop:1 #ffffff"));
            auto swatch = dialog->findChild<QPushButton *>("shadowsSwatch");
            QVERIFY(swatch && !swatch->icon().isNull());
            dialog->findChild<QCheckBox *>("reversedControl")->setChecked(true);
            QVERIFY(bar->styleSheet().contains("stop:0 #ffffff,stop:1 #000000"));
        }, &visited);
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 0);
    }
    void curvesShowTheHistogramAndEditPoints() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, ramp()));
        auto page = currentPage(*window);
        bool visited = false;
        withDialog(imageAdjustment(*window, "Curves…"), false, [&](QDialog *dialog) {
            auto curve = static_cast<CurveEditor *>(dialog->findChild<QWidget *>("curveEditor"));
            QVERIFY(curve);
            QTRY_VERIFY_WITH_TIMEOUT(curve->showHistogram, 5000);
            QVERIFY(curve->histogram[100] > 0);
            // Click the middle of the graph: a point is added and selected.
            QTest::mouseClick(curve, Qt::LeftButton, {}, curve->rect().center());
            QCOMPARE(curve->points.size(), 3);
            QCOMPARE(curve->selected, 1);
            auto readout = dialog->findChild<QLabel *>("curvePointReadout");
            QVERIFY(readout->text().contains("Input"));
            auto remove = dialog->findChild<QPushButton *>("curveRemovePoint");
            QVERIFY(remove->isEnabled());
            remove->click();
            QCOMPARE(curve->points.size(), 2);
            QVERIFY(!remove->isEnabled());
            QTest::mouseClick(curve, Qt::LeftButton, {}, curve->rect().center() + QPoint(0, -40));
            QCOMPARE(curve->points.size(), 3);
            dialog->findChild<QPushButton *>("curveReset")->click();
            QCOMPARE(curve->points.size(), 2);
        }, &visited);
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 0);
    }
    void colorWheelsSetHueAndSaturation() {
        ColorWheel wheel;
        wheel.resize(110, 110);
        double hue = -1, saturation = -1;
        wheel.changed = [&](double h, double s) {
            hue = h;
            saturation = s;
        };
        wheel.show();
        // Right of the center is red; straight up is 90°; the rim is full saturation.
        QTest::mouseClick(&wheel, Qt::LeftButton, {}, wheel.pointOf(0, 100).toPoint());
        QVERIFY(hue <= 1 || hue >= 359);
        QVERIFY(saturation >= 97);
        QTest::mouseClick(&wheel, Qt::LeftButton, {}, wheel.pointOf(90, 50).toPoint());
        QVERIFY(std::abs(hue - 90) <= 2);
        QVERIFY(std::abs(saturation - 50) <= 3);
        QTest::mouseDClick(&wheel, Qt::LeftButton, {}, wheel.rect().center());
        QCOMPARE(saturation, 0.0);
    }
    void cameraRawIsASidePanelThatPreviewsOnTheCanvas() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, filled({64, 64}, QColor(90, 100, 110))));
        window->resize(1200, 800);
        auto page = currentPage(*window);
        const auto before = page->document.active()->image;
        bool visited = false;
        withDialog(imageAdjustment(*window, "Camera Raw…"), false, [&](QDialog *dialog) {
            // Down the window's right edge, most of its height.
            const auto frame = window->frameGeometry();
            QVERIFY(dialog->geometry().right() <= frame.right());
            QVERIFY(dialog->geometry().left() > frame.center().x());
            QVERIFY(dialog->height() >= frame.height() - 200);
            dialog->findChild<QDoubleSpinBox *>("exposureControl")->setValue(1);
            QTRY_VERIFY_WITH_TIMEOUT(page->canvas->hasLivePreview(), 5000);
            // The readout shows the previewed color under the pointer.
            auto readout = dialog->findChild<QLabel *>("cameraRawReadout");
            QVERIFY(readout);
            // Hovering the canvas (the window may still be settling into its new size).
            int step = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                [&] {
                    hover(page->canvas, onCanvas(*page, 31.5 + (step++ % 2), 32.5));
                    return !readout->text().contains("—") &&
                           readout->text().split(' ', Qt::SkipEmptyParts).value(1).toInt() > 120;
                }(),
                5000);
            hover(page->canvas, onCanvas(*page, -4, 32.5)); // Beside the layer.
            QVERIFY(readout->text().contains("—"));
            // The color grading wheels drive their fields.
            auto wheel = static_cast<ColorWheel *>(dialog->findChild<QWidget *>("gradeShadowsWheel"));
            QVERIFY(wheel);
            wheel->changed(200, 40);
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("gradeShadowsHueControl")->value(), 200.0);
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("gradeShadowsSaturationControl")->value(), 40.0);
            dialog->findChild<QDoubleSpinBox *>("gradeShadowsHueControl")->setValue(30);
            QCOMPARE(wheel->hue, 30.0);
        }, &visited);
        QVERIFY(visited);
        QVERIFY(!page->canvas->hasLivePreview());
        QCOMPARE(page->history.count(), 0);
        QCOMPARE(page->document.active()->image, before);
    }
    void cameraRawTargetedDragsAndSamplingWorkOnTheCanvas() {
        QImage image(60, 20, QImage::Format_RGBA8888_Premultiplied);
        QPainter painter(&image);
        painter.fillRect(0, 0, 20, 20, QColor(40, 40, 40));
        painter.fillRect(20, 0, 20, 20, QColor(235, 235, 235));
        painter.fillRect(40, 0, 20, 20, QColor(210, 30, 30));
        painter.end();
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, image));
        auto page = currentPage(*window);
        bool visited = false;
        withDialog(imageAdjustment(*window, "Camera Raw…"), true, [&](QDialog *dialog) {
            auto tool = dialog->findChild<QComboBox *>("cameraPreviewTool");
            QVERIFY(tool);
            auto drag = [&](double x, int up) {
                const auto from = onCanvas(*page, x, 10.5);
                QTest::mousePress(page->canvas, Qt::LeftButton, {}, from);
                QTest::mouseMove(page->canvas, from - QPoint(0, up));
                QTest::mouseRelease(page->canvas, Qt::LeftButton, {}, from - QPoint(0, up));
            };
            auto value = [&](const QString &key) {
                return dialog->findChild<QDoubleSpinBox *>(key + "Control")->value();
            };
            selectComboValue(tool, "Targeted: tone curve");
            drag(10.5, 60); // The dark part: shadows up.
            QVERIFY(value("curveShadows") > 15);
            QCOMPARE(value("curveHighlights"), 0.0);
            drag(30.5, -60); // The light part: highlights down.
            QVERIFY(value("curveHighlights") < -15);
            selectComboValue(tool, "Targeted: color mixer");
            auto component = dialog->findChild<QComboBox *>("cameraTargetComponent");
            QVERIFY(component && component->isVisible());
            selectComboValue(component, "Saturation");
            drag(50.5, 60); // Red: reds most, oranges a little, greens not at all.
            QVERIFY(value("mixerRedsSaturation") > 15);
            QVERIFY(value("mixerOrangesSaturation") > 0);
            QVERIFY(value("mixerOrangesSaturation") < value("mixerRedsSaturation"));
            QCOMPARE(value("mixerGreensSaturation"), 0.0);
            // Point color sampling on the canvas.
            selectComboValue(tool, "Sample point color");
            QTest::mouseClick(page->canvas, Qt::LeftButton, {}, onCanvas(*page, 50.5, 10.5));
            QCOMPARE(dialog->findChild<QComboBox *>("pointColorList")->count(), 1);
            // Back to Preview: the canvas is the dialog's no longer.
            selectComboValue(tool, "Preview");
            QCOMPARE(page->history.count(), 0);
        }, &visited);
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 1);
        // Shadows lifted: the dark part is lighter now.
        QVERIFY(page->document.active()->image.pixelColor(10, 10).red() > 40);
    }
    void hueSaturationBandsEyedroppersAndTargetedDrag() {
        QImage image(40, 10, QImage::Format_RGBA8888_Premultiplied);
        QPainter painter(&image);
        painter.fillRect(0, 0, 20, 10, QColor(220, 30, 30));
        painter.fillRect(20, 0, 20, 10, QColor(30, 200, 30));
        painter.end();
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, image));
        auto page = currentPage(*window);
        bool visited = false;
        withDialog(imageAdjustment(*window, "Hue/Saturation…"), true, [&](QDialog *dialog) {
            auto spectrum = static_cast<HueSpectrum *>(dialog->findChild<QWidget *>("hueSpectrum"));
            auto range = dialog->findChild<QComboBox *>();
            QVERIFY(spectrum && range);
            QVERIFY(!spectrum->isVisible()); // Master has no band.
            selectComboValue(range, "Greens");
            QVERIFY(spectrum->isVisible());
            QCOMPARE(spectrum->band.value("rangeStart").toDouble(), 105.0);
            // Sampling the green half centers the band on its hue.
            dialog->findChild<QPushButton *>("hueSample")->click();
            QTest::mouseClick(page->canvas, Qt::LeftButton, {}, onCanvas(*page, 30.5, 5.5));
            const double green = QColor(30, 200, 30).hslHueF() * 360;
            QVERIFY(std::abs(spectrum->band.value("rangeStart").toDouble() - (green - 15)) < 1);
            // Targeted: dragging on the red half adjusts the Reds' saturation.
            dialog->findChild<QPushButton *>("hueTargeted")->click();
            QVERIFY(!dialog->findChild<QPushButton *>("hueSample")->isChecked());
            const auto from = onCanvas(*page, 10.5, 5.5);
            QTest::mousePress(page->canvas, Qt::LeftButton, {}, from);
            QTest::mouseMove(page->canvas, from - QPoint(120, 0));
            QTest::mouseRelease(page->canvas, Qt::LeftButton, {}, from - QPoint(120, 0));
            QCOMPARE(comboValue(range), QString("Reds"));
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("saturationControl")->value(), -60.0);
            // Colorize hides the bands and starts at 25% saturation.
            dialog->findChild<QCheckBox *>("colorizeControl")->setChecked(true);
            QVERIFY(!spectrum->isVisible());
            QVERIFY(!range->isEnabled());
            QCOMPARE(dialog->findChild<QDoubleSpinBox *>("saturationControl")->value(), 25.0);
            dialog->findChild<QCheckBox *>("colorizeControl")->setChecked(false);
            selectComboValue(range, "Reds");
            dialog->findChild<QDoubleSpinBox *>("saturationControl")->setValue(-100);
        }, &visited);
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 1);
        const auto out = page->document.active()->image;
        QVERIFY(out.pixelColor(5, 5).hslSaturationF() < .05);
        QVERIFY(out.pixelColor(30, 5).hslSaturationF() > .5);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("AdjustmentPanelTests");
    AdjustmentPanelTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "adjustment_panel_tests.moc"

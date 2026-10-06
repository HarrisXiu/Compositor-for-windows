// SPDX-License-Identifier: MIT
#include "editor.h"
#include "filter_preview.h"
#include "language.h"
#include "live_dialog.h"
#include "parameter_control.h"
#include "preview_runner.h"
#include "render.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QTimer>
#include <atomic>
using namespace compositor;
namespace {
Document grayDocument(QSize size = {32, 32}, QColor color = QColor(100, 120, 140)) {
    auto d = Document::create(size);
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(color);
    d.addImage("Photo", image);
    return d;
}
void ready(EditorPage &p, double zoom = 1) {
    p.resize(480, 480);
    p.show();
    QTest::qWait(20);
    p.canvas->zoomTo(zoom);
    p.canvas->waitForRendering();
}
// The canvas as drawn, at a document point.
QColor shown(EditorPage &p, double x, double y) {
    p.canvas->waitForRendering();
    QTest::qWait(10);
    p.canvas->grab();
    p.canvas->waitForRendering();
    const auto image = p.canvas->grab().toImage();
    if (qEnvironmentVariableIsSet("LIVE_PREVIEW_DEBUG"))
        image.save(qEnvironmentVariable("LIVE_PREVIEW_DEBUG"));
    const auto px = QPoint(qRound((p.canvas->width() - p.document.size().width() * p.canvas->zoom) / 2 + x * p.canvas->zoom),
                           qRound((p.canvas->height() - p.document.size().height() * p.canvas->zoom) / 2 + y * p.canvas->zoom));
    return image.pixelColor(px);
}
EditorWindow *openWindow(QTemporaryDir &dir, const Document &d) {
    const auto path = dir.filePath("Live.comp");
    saveProject(d, path);
    auto window = new EditorWindow;
    window->openPath(path);
    window->show();
    return window;
}
EditorPage *currentPage(EditorWindow &w) {
    return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
}
// A menu command by title; "Exposure…" and the like are the Image menu's filters, not the
// entries of the same name under Layer ▸ New Adjustment Layer.
QAction *menuAction(EditorWindow &w, const QString &title) {
    QSet<QAction *> adjustmentLayers;
    for (auto menu : w.findChildren<QMenu *>())
        if (menu->title() == "New Adjustment Layer")
            for (auto a : menu->actions())
                adjustmentLayers.insert(a);
    for (auto a : w.findChildren<QAction *>())
        if ((a->property("layerAction").toString() == title || a->text() == title) &&
            !adjustmentLayers.contains(a))
            return a;
    return nullptr;
}
} // namespace
class LivePreviewTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void sliderFieldAndLabelAgree() {
        ParameterControl control(-100, 100, 1, 0, 0);
        QSignalSpy changes(&control, &ParameterControl::valueChanged);
        control.slider()->setValue(250); // 25.0 at one decimal.
        QCOMPARE(control.spin()->value(), 25.0);
        control.spin()->setValue(-40.5);
        QCOMPARE(control.slider()->value(), -405);
        QCOMPARE(control.value(), -40.5);
        QVERIFY(changes.count() >= 2);
        control.reset();
        QCOMPARE(control.value(), 0.0);
        QCOMPARE(control.slider()->value(), 0);
    }
    void dragginTheLabelScrubsTheValue() {
        QWidget host;
        auto form = new QFormLayout(&host);
        auto control = addParameter(form, "Amount", new ParameterControl(0, 100, 1, 10, 10), "amountControl");
        QCOMPARE(control->spin()->objectName(), QString("amountControl"));
        host.show();
        auto label = host.findChild<ScrubLabel *>();
        QVERIFY(label);
        QSignalSpy finished(control, &ParameterControl::adjustmentFinished);
        const auto start = label->rect().center();
        QTest::mousePress(label, Qt::LeftButton, Qt::NoModifier, start);
        const auto before = control->value();
        QMouseEvent move(QEvent::MouseMove, QPointF(start + QPoint(40, 0)),
                         QPointF(label->mapToGlobal(start + QPoint(40, 0))), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(label, &move);
        // 100 / 400 = .25 per pixel.
        QCOMPARE(control->value(), before + 40 * control->scrubSensitivity());
        QMouseEvent fine(QEvent::MouseMove, QPointF(start + QPoint(40, 0)),
                         QPointF(label->mapToGlobal(start + QPoint(40, 0))), Qt::NoButton,
                         Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(label, &fine);
        QVERIFY(std::abs(control->value() - (before + 4 * control->scrubSensitivity())) < .06);
        QTest::mouseRelease(label, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 0));
        QCOMPARE(finished.count(), 1);
        control->setValue(80);
        QTest::mouseDClick(label, Qt::LeftButton);
        QCOMPARE(control->value(), 10.0); // Back to its default.
    }
    void aSliderRangeOfThousandsStillHasFineSteps() {
        ParameterControl control(1, 2000, 1, 10, 10);
        control.spin()->setValue(1234.5);
        QCOMPARE(control.slider()->value(), 12345);
        ParameterControl wide(0, 100000, 3, 0, 0);
        wide.spin()->setValue(50000);
        QVERIFY(wide.slider()->maximum() <= 200000);
        QVERIFY(wide.slider()->value() > 0);
    }
    void runnerKeepsOnlyTheNewestWaitingJob() {
        PreviewRunner runner;
        QVector<int> results;
        connect(&runner, &PreviewRunner::ready, &runner, [&](const QImage &image) { results << image.width(); });
        std::atomic<bool> release{false};
        auto job = [&release](int width) {
            return [&release, width]() -> QImage {
                while (!release)
                    QThread::msleep(2);
                return QImage(width, 1, QImage::Format_RGBA8888_Premultiplied);
            };
        };
        runner.request(job(1));
        QVERIFY(runner.busy());
        runner.request(job(2)); // Replaced...
        runner.request(job(3)); // ...by this.
        release = true;
        QTRY_VERIFY_WITH_TIMEOUT(!runner.pending(), 3000);
        QCOMPARE(results, (QVector<int>{1, 3}));
    }
    void canceledJobsAreNotShown() {
        PreviewRunner runner;
        int count = 0;
        connect(&runner, &PreviewRunner::ready, &runner, [&](const QImage &) { ++count; });
        runner.request([] { QThread::msleep(60); return QImage(1, 1, QImage::Format_RGBA8888_Premultiplied); });
        runner.request([] { return QImage(2, 1, QImage::Format_RGBA8888_Premultiplied); });
        runner.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!runner.pending(), 3000);
        QTest::qWait(30);
        QCOMPARE(count, 0);
    }
    void failedJobsAreReported() {
        PreviewRunner runner;
        QSignalSpy failures(&runner, &PreviewRunner::failed);
        runner.request([]() -> QImage { throw Error("Out of memory"); });
        QTRY_COMPARE_WITH_TIMEOUT(failures.count(), 1, 3000);
        QCOMPARE(failures.first().first().toString(), QString("Out of memory"));
    }
    void theCanvasDrawsAPreviewWithoutChangingTheDocument() {
        EditorPage p(grayDocument());
        ready(p, 4);
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        const auto original = shown(p, 16, 16);
        QCOMPARE(original, QColor(100, 120, 140));
        const auto id = p.document.activeId();
        QImage red(32, 32, QImage::Format_RGBA8888_Premultiplied);
        red.fill(Qt::red);
        p.canvas->setLivePreview([id, red](Document &d) { d.find(id)->image = red; },
                                 QRectF(0, 0, 32, 32), id);
        QVERIFY(p.canvas->hasLivePreview());
        QCOMPARE(shown(p, 16, 16), QColor(Qt::red));
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.history.count(), 0);
        QVERIFY(!p.isModified());
        p.canvas->clearLivePreview();
        QVERIFY(!p.canvas->hasLivePreview());
        QCOMPARE(shown(p, 16, 16), original);
    }
    void aLockedCanvasOnlyPansAndZooms() {
        auto d = grayDocument();
        EditorPage p(d);
        ready(p, 4);
        p.canvas->setTool(Tool::Brush);
        p.session.brushSize = 8;
        p.session.foreground = Qt::red;
        p.canvas->setInputLocked(true);
        const auto at = QPoint(p.canvas->width() / 2, p.canvas->height() / 2);
        QTest::mouseClick(p.canvas, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(p.history.count(), 0);
        p.canvas->zoomTo(2);
        QCOMPARE(p.canvas->zoom, 2.0);
        p.canvas->setInputLocked(false);
        QTest::mouseClick(p.canvas, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(p.history.count(), 1);
    }
    void aPickHandlerTakesTheMouseWhileLocked() {
        EditorPage p(grayDocument());
        ready(p, 4);
        p.canvas->setInputLocked(true);
        QVector<Canvas::PickEvent> events;
        p.canvas->setPickHandler([&](const Canvas::PickEvent &e) { events << e; });
        const auto at = QPoint(p.canvas->width() / 2, p.canvas->height() / 2);
        QTest::mousePress(p.canvas, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(p.canvas, at + QPoint(8, 0));
        QTest::mouseRelease(p.canvas, Qt::LeftButton, Qt::NoModifier, at + QPoint(8, 0));
        QCOMPARE(events.size(), 3);
        QCOMPARE(int(events[0].phase), int(Canvas::PickEvent::Phase::Press));
        QCOMPARE(int(events[2].phase), int(Canvas::PickEvent::Phase::Release));
        QVERIFY(std::abs(events[0].point.x() - 16) < .6 && std::abs(events[0].point.y() - 16) < .6);
        QVERIFY(events[1].point.x() > events[0].point.x());
        p.canvas->setPickHandler({});
        QCOMPARE(p.history.count(), 0);
    }
    void aFilterPreviewComputesInTheBackgroundAtTheScreensScale() {
        EditorPage p(grayDocument({400, 400}));
        ready(p, .25);
        const auto before = p.document.manifest();
        FilterPreview preview(&p, "Exposure", false, false);
        preview.update({{"exposure", 1.0}});
        QVERIFY(preview.busy());
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > 0, 5000);
        QCOMPARE(preview.scale(), .25); // 100 × 100 pixels for a 100-pixel view.
        QVERIFY(shown(p, 200, 200).red() > 125); // One stop up from 100.
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.history.count(), 0);
        // Zooming in recomputes at full size.
        p.canvas->zoomTo(1);
        QTRY_COMPARE_WITH_TIMEOUT(preview.scale(), 1.0, 5000);
        preview.stop();
        QVERIFY(!p.canvas->hasLivePreview());
        QCOMPARE(shown(p, 200, 200), QColor(100, 120, 140));
    }
    void cameraRawPreviewsOnTheCanvasToo() {
        EditorPage p(grayDocument({64, 64}, QColor(90, 100, 110)));
        ready(p, 4);
        FilterPreview preview(&p, "Camera Raw", false, false);
        preview.update({{"exposure", 0.0}});
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > 0, 5000);
        QCOMPARE(preview.shownColor({32, 32}), QColor(90, 100, 110));
        const int count = preview.shownCount();
        preview.update({{"exposure", 1.0}});
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > count, 5000);
        QVERIFY(preview.shownColor({32, 32}).red() > 120);
        QVERIFY(!preview.shownColor({-5, 5}).isValid());
    }
    void aFilterPreviewStaysInsideTheSelection() {
        EditorPage p(grayDocument({64, 64}));
        ready(p, 4);
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x)
                mask.scanLine(y)[x] = 255;
        p.session.selection = mask;
        FilterPreview preview(&p, "Exposure", false, false);
        preview.update({{"exposure", 2.0}});
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > 0, 5000);
        QVERIFY(shown(p, 8, 32).red() > 160); // Two stops up.
        QCOMPARE(shown(p, 56, 32), QColor(100, 120, 140));
    }
    void aDisabledOrStoppedPreviewShowsTheDocument() {
        EditorPage p(grayDocument());
        ready(p, 4);
        FilterPreview preview(&p, "Exposure", false, false);
        preview.update({{"exposure", 2.0}});
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > 0, 5000);
        preview.setEnabled(false);
        QVERIFY(!p.canvas->hasLivePreview());
        QCOMPARE(shown(p, 16, 16), QColor(100, 120, 140));
        const auto count = preview.shownCount();
        preview.update({{"exposure", 1.0}}); // Not computed while off.
        QTest::qWait(100);
        QCOMPARE(preview.shownCount(), count);
        preview.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > count, 5000);
        QVERIFY(p.canvas->hasLivePreview());
    }
    void anAdjustmentPreviewIsShownImmediately() {
        EditorPage p(grayDocument());
        ready(p, 4);
        const auto before = p.document.manifest();
        FilterPreview preview(&p, "Exposure", true, false);
        preview.update({{"exposure", 1.0}});
        QVERIFY(p.canvas->hasLivePreview());
        QVERIFY(shown(p, 16, 16).red() > 125);
        QCOMPARE(p.document.layers.size(), 1); // The adjustment layer exists only on the canvas.
        QCOMPARE(p.document.manifest(), before);
        preview.update({{"exposure", -1.0}});
        QVERIFY(shown(p, 16, 16).red() < 90);
    }
    void aBlurAdjustmentPreviewFollowsItsRadius() {
        auto d = grayDocument({64, 64});
        QImage edge(64, 64, QImage::Format_RGBA8888_Premultiplied);
        edge.fill(Qt::black);
        QPainter(&edge).fillRect(32, 0, 32, 64, Qt::white);
        d.active()->image = edge;
        EditorPage p(d);
        ready(p, 4);
        FilterPreview preview(&p, "Gaussian Blur", true, false);
        preview.update({{"radius", 0.1}});
        const auto sharp = shown(p, 30, 32).red();
        preview.update({{"radius", 12.0}}); // A reach that differs from before: no stale backdrops.
        const auto soft = shown(p, 30, 32).red();
        QVERIFY(soft > sharp + 20);
    }
    void aFilterDialogLeavesTheWindowOpenAndPreviewsOnTheCanvas() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, grayDocument()));
        auto page = currentPage(*window);
        page->canvas->zoomTo(4);
        page->canvas->waitForRendering();
        const auto before = page->document.active()->image;
        const auto original = shown(*page, 16, 16);
        auto exposure = menuAction(*window, "Exposure…");
        QVERIFY(exposure);
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(dialog);
            // A failed check below must not leave the dialog's event loop running.
            auto closeAtEnd = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->reject();
            });
            QVERIFY(!dialog->isModal());
            QVERIFY(dialog->isVisible());
            QVERIFY(dialog->findChild<QSlider *>()); // Sliders beside the fields.
            auto spin = dialog->findChild<QDoubleSpinBox *>("exposureControl");
            QVERIFY(spin);
            spin->setValue(1);
            QTRY_VERIFY_WITH_TIMEOUT(page->canvas->hasLivePreview(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(shown(*page, 16, 16).red() > original.red() + 15, 5000);
            QCOMPARE(page->document.active()->image, before); // Nothing applied yet.
            QCOMPARE(page->history.count(), 0);
            // The window is locked to what the dialog edits; the view is free.
            QVERIFY(page->canvas->inputLocked());
            QVERIFY(!window->findChild<QTabWidget *>()->tabBar()->isEnabled());
            const double zoomBefore = page->canvas->zoom;
            menuAction(*window, "Zoom Out")->trigger();
            QVERIFY(dialog->isVisible());
            QVERIFY(page->canvas->zoom < zoomBefore);
            // The Preview box shows and hides the result.
            auto toggle = dialog->findChild<QCheckBox *>("livePreviewControl");
            QVERIFY(toggle && toggle->isChecked());
            toggle->setChecked(false);
            QVERIFY(!page->canvas->hasLivePreview());
            toggle->setChecked(true);
            QTRY_VERIFY_WITH_TIMEOUT(page->canvas->hasLivePreview(), 5000);
            visited = true;
            dialog->accept();
        });
        exposure->trigger();
        QVERIFY(visited);
        QVERIFY(!page->canvas->hasLivePreview());
        QVERIFY(!page->canvas->inputLocked());
        QVERIFY(window->findChild<QTabWidget *>()->tabBar()->isEnabled());
        QCOMPARE(page->history.count(), 1);
        QCOMPARE(page->history.undoText(), QString("Exposure"));
        QVERIFY(page->document.active()->image != before);
        page->history.undo();
        QCOMPARE(page->document.active()->image, before);
    }
    void aLayerEffectPreviewsOnTheCanvas() {
        // As on the Mac, an effect's panel stays open beside the window and the effect shows on
        // the canvas as it changes; OK records it as one step, Cancel leaves nothing.
        auto d = Document::create({64, 64});
        QImage red(32, 32, QImage::Format_RGBA8888_Premultiplied);
        red.fill(Qt::red);
        d.addImage("Red", red);
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, d));
        auto page = currentPage(*window);
        page->canvas->zoomTo(4);
        page->canvas->waitForRendering();
        const auto below = shown(*page, 16, 44); // Empty canvas under the layer.
        auto shadow = menuAction(*window, "Drop Shadow…");
        QVERIFY(shadow);
        for (bool accept : {false, true}) {
            bool visited = false;
            QTimer::singleShot(20, [&] {
                auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
                QVERIFY(dialog);
                auto closeAtEnd = qScopeGuard([dialog] {
                    if (dialog->isVisible())
                        dialog->reject();
                });
                QVERIFY(!dialog->isModal());
                QVERIFY(page->canvas->inputLocked());
                dialog->findChild<QDoubleSpinBox *>("distanceControl")->setValue(14);
                dialog->findChild<QDoubleSpinBox *>("blurControl")->setValue(0);
                dialog->findChild<QDoubleSpinBox *>("opacityControl")->setValue(1);
                QTRY_VERIFY_WITH_TIMEOUT(shown(*page, 16, 44) != below, 5000);
                QVERIFY(!page->document.active()->metadata.contains("effects"));
                QCOMPARE(page->history.count(), 0);
                visited = true;
                accept ? dialog->accept() : dialog->reject();
            });
            shadow->trigger();
            QVERIFY(visited);
            QVERIFY(!page->canvas->hasLivePreview());
            QVERIFY(!page->canvas->inputLocked());
            if (!accept) {
                QCOMPARE(page->history.count(), 0);
                QCOMPARE(shown(*page, 16, 44), below);
            }
        }
        QCOMPARE(page->history.count(), 1);
        QCOMPARE(page->history.undoText(), QString("Edit Effect"));
        const auto effect = page->document.active()->metadata.value("effects").toObject().value("shadow").toObject();
        QCOMPARE(effect.value("distance").toDouble(), 14.0);
        QVERIFY(shown(*page, 16, 44) != below);
    }
    void okAppliesInTheBackgroundWithThePreviewStillShown() {
        // Big enough that the full-resolution blur takes a while.
        auto d = Document::create({1200, 1200});
        QImage noise(1200, 1200, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < noise.height(); ++y) {
            auto row = reinterpret_cast<quint32 *>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x)
                row[x] = 0xff000000u | quint32(((x ^ y) & 0xff) * 0x010101);
        }
        d.addImage("Noise", noise);
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, d));
        auto page = currentPage(*window);
        page->canvas->waitForRendering();
        auto blur = menuAction(*window, "Gaussian Blur…");
        QVERIFY(blur);
        // While the result is made, the window keeps running its event loop: the preview is still
        // shown, only the view is free, and other commands are ignored.
        bool applying = false, previewShown = false, locked = false, undoIgnored = true;
        QTimer probe;
        probe.setInterval(0);
        connect(&probe, &QTimer::timeout, [&] {
            if (activeLiveDialog() || !window->statusBar()->currentMessage().startsWith("Applying"))
                return;
            applying = true;
            previewShown = previewShown || page->canvas->hasLivePreview();
            locked = locked || (page->canvas->inputLocked() &&
                                !window->findChild<QTabWidget *>()->tabBar()->isEnabled());
            menuAction(*window, "Undo")->trigger();
            undoIgnored = undoIgnored && page->history.count() == 0;
        });
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(dialog);
            auto closeAtEnd = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->reject();
            });
            dialog->findChild<QDoubleSpinBox *>("radiusControl")->setValue(30);
            QTRY_VERIFY_WITH_TIMEOUT(page->canvas->hasLivePreview(), 10000);
            visited = true;
            probe.start();
            dialog->accept();
        });
        blur->trigger();
        probe.stop();
        QVERIFY(visited);
        QVERIFY(applying);
        QVERIFY(previewShown);
        QVERIFY(locked);
        QVERIFY(undoIgnored);
        QVERIFY(!page->canvas->hasLivePreview());
        QVERIFY(!page->canvas->inputLocked());
        QVERIFY(window->findChild<QTabWidget *>()->tabBar()->isEnabled());
        QCOMPARE(page->history.count(), 1);
        QCOMPARE(page->history.undoText(), QString("Gaussian Blur"));
        // The blur grew the layer past the canvas, as it does when applied directly.
        QVERIFY(page->document.active()->image.width() > 1200);
    }
    void cancelingALiveDialogLeavesNoTrace() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, grayDocument()));
        auto page = currentPage(*window);
        const auto before = page->document.manifest();
        const auto image = page->document.active()->image;
        auto exposure = menuAction(*window, "Exposure…");
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(dialog);
            // A failed check below must not leave the dialog's event loop running.
            auto closeAtEnd = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->reject();
            });
            dialog->findChild<QDoubleSpinBox *>("exposureControl")->setValue(2);
            QTRY_VERIFY_WITH_TIMEOUT(page->canvas->hasLivePreview(), 5000);
            visited = true;
            dialog->reject();
        });
        exposure->trigger();
        QVERIFY(visited);
        QVERIFY(!page->canvas->hasLivePreview());
        QCOMPARE(page->history.count(), 0);
        QCOMPARE(page->document.manifest(), before);
        QCOMPARE(page->document.active()->image, image);
        QVERIFY(!page->isModified());
    }
    void anotherCommandEndsTheDialogFirst() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, grayDocument()));
        auto page = currentPage(*window);
        const auto before = page->document.manifest();
        auto exposure = menuAction(*window, "Exposure…");
        bool visited = false;
        QPointer<QDialog> opened;
        QTimer::singleShot(20, [&] {
            opened = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(opened);
            auto closeAtEnd = qScopeGuard([&] {
                if (opened && opened->isVisible())
                    opened->reject();
            });
            opened->findChild<QDoubleSpinBox *>("exposureControl")->setValue(2);
            QTRY_VERIFY_WITH_TIMEOUT(page->canvas->hasLivePreview(), 5000);
            visited = true;
            menuAction(*window, "New Paint Layer")->trigger();
        });
        exposure->trigger();
        QVERIFY(visited);
        // The dialog was canceled, then the command ran on the untouched document.
        QVERIFY(!page->canvas->hasLivePreview());
        QCOMPARE(page->history.count(), 1);
        QCOMPARE(page->history.undoText(), QString("New Layer"));
        page->history.undo();
        QCOMPARE(page->document.manifest(), before);
    }
    void closingTheWindowEndsTheDialog() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, grayDocument()));
        auto page = currentPage(*window);
        auto exposure = menuAction(*window, "Exposure…");
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(dialog);
            // A failed check below must not leave the dialog's event loop running.
            auto closeAtEnd = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->reject();
            });
            visited = true;
            window->close();
        });
        exposure->trigger();
        QVERIFY(visited);
        QVERIFY(!page->canvas->hasLivePreview());
        QCOMPARE(page->history.count(), 0);
    }
    void anAdjustmentDialogPreviewsAndCreatesOneLayer() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, grayDocument()));
        auto page = currentPage(*window);
        page->canvas->zoomTo(4);
        const auto original = shown(*page, 16, 16);
        // The adjustment-layer entries live under Layer ▸ New Adjustment Layer.
        QAction *create = nullptr;
        for (auto menu : window->findChildren<QMenu *>())
            if (menu->title() == "New Adjustment Layer")
                for (auto a : menu->actions())
                    if (a->text() == "Exposure…")
                        create = a;
        QVERIFY(create);
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(dialog);
            // A failed check below must not leave the dialog's event loop running.
            auto closeAtEnd = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->reject();
            });
            dialog->findChild<QDoubleSpinBox *>("exposureControl")->setValue(1);
            QVERIFY(page->canvas->hasLivePreview());
            // The dialog updates the preview shortly after the last change.
            QTRY_VERIFY_WITH_TIMEOUT(shown(*page, 16, 16).red() > original.red() + 15, 5000);
            QCOMPARE(page->document.layers.size(), 1);
            visited = true;
            dialog->accept();
        });
        create->trigger();
        QVERIFY(visited);
        QCOMPARE(page->document.layers.size(), 2);
        QCOMPARE(page->history.count(), 1);
        QVERIFY(!page->canvas->hasLivePreview());
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("LivePreviewTests");
    LivePreviewTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "live_preview_tests.moc"

// SPDX-License-Identifier: MIT
#include "editor.h"
#include "filter_preview.h"
#include "filters.h"
#include "image_operations.h"
#include "language.h"
#include "live_dialog.h"
#include "parameter_control.h"
#include "render.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
using namespace compositor;
namespace {
QRectF boxOf(const Layer &l) {
    const auto t = l.transform();
    const auto o = t.value("origin").toArray(), s = t.value("size").toArray();
    return {o.at(0).toDouble(), o.at(1).toDouble(), s.at(0).toDouble(), s.at(1).toDouble()};
}
// A 64-pixel canvas with a red 20-pixel square layer at (22, 22).
Document squareDocument() {
    auto d = Document::create({64, 64});
    QImage red(20, 20, QImage::Format_RGBA8888_Premultiplied);
    red.fill(Qt::red);
    const auto id = d.addImage("Square", red);
    d.find(id)->setBounds({22, 22, 20, 20});
    return d;
}
EditorWindow *openWindow(QTemporaryDir &dir, const Document &d) {
    const auto path = dir.filePath("Reach.comp");
    saveProject(d, path);
    auto window = new EditorWindow;
    window->openPath(path);
    window->show();
    return window;
}
EditorPage *currentPage(EditorWindow &w) {
    return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
}
// A Filter or Image menu command, not the adjustment-layer entry of the same name.
QAction *filterAction(EditorWindow &w, const QString &title) {
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
void ready(EditorPage &p, double zoom) {
    p.resize(480, 480);
    p.show();
    QTest::qWait(20);
    p.canvas->zoomTo(zoom);
    p.canvas->waitForRendering();
}
QColor shown(EditorPage &p, double x, double y) {
    p.canvas->waitForRendering();
    QTest::qWait(10);
    p.canvas->grab();
    p.canvas->waitForRendering();
    const auto image = p.canvas->grab().toImage();
    return image.pixelColor(
        qRound((p.canvas->width() - p.document.size().width() * p.canvas->zoom) / 2 + x * p.canvas->zoom),
        qRound((p.canvas->height() - p.document.size().height() * p.canvas->zoom) / 2 + y * p.canvas->zoom));
}
} // namespace
class FilterReachTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void blursGrowTheLayerByTheirReach() {
        QCOMPARE(filterMargin("Gaussian Blur", {{"radius", 4.0}}), 14.0);
        QCOMPARE(filterMargin("Motion Blur", {{"distance", 20.0}}), 12.0);
        QCOMPARE(filterMargin("Bloom / Glow", {{"bloomRadius", 2.0}}), 8.0);
        QCOMPARE(filterMargin("Exposure", {}), 0.0);
        auto d = squareDocument();
        auto layer = *d.active();
        QVERIFY(growForFilter(layer, "Gaussian Blur", {{"radius", 4.0}}));
        QCOMPARE(layer.image.size(), QSize(48, 48));
        QCOMPARE(boxOf(layer), QRectF(8, 8, 48, 48));
        // The pixels kept their place on the document.
        d.layers[0] = layer;
        QCOMPARE(renderDocument(d).pixelColor(22, 22), QColor(Qt::red));
        QCOMPARE(renderDocument(d).pixelColor(21, 22).alpha(), 0);
        auto plain = *squareDocument().active();
        QVERIFY(!growForFilter(plain, "Exposure", {}));
        QCOMPARE(plain.image.size(), QSize(20, 20));
        // Content-Aware Fill grows over the area to fill.
        QVERIFY(growForFilter(plain, "Content-Aware Fill", {}, QRectF(30, 30, 30, 10)));
        QCOMPARE(boxOf(plain), QRectF(22, 22, 38, 20));
    }
    void anAppliedBlurSpreadsPastTheLayerEdge() {
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, squareDocument()));
        auto page = currentPage(*window);
        bool visited = false;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(activeLiveDialog());
            QVERIFY(dialog);
            auto close = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->accept();
            });
            dialog->findChild<QDoubleSpinBox *>("radiusControl")->setValue(3);
            visited = true;
        });
        filterAction(*window, "Gaussian Blur…")->trigger();
        QVERIFY(visited);
        QCOMPARE(page->history.count(), 1);
        const auto &layer = *page->document.active();
        QCOMPARE(layer.image.size(), QSize(42, 42));
        const auto picture = renderDocument(page->document);
        // Two pixels outside the square's old edge, the blur shows.
        const auto outside = picture.pixelColor(20, 32);
        QVERIFY(outside.alpha() > 20);
        QVERIFY(outside.red() > 200 && outside.green() < 30);
        QCOMPARE(picture.pixelColor(2, 2).alpha(), 0);
        page->history.undo();
        QCOMPARE(page->document.active()->image.size(), QSize(20, 20));
    }
    void theBlurPreviewSpreadsToo() {
        EditorPage p(squareDocument());
        ready(p, 4);
        const auto before = shown(p, 19, 32);
        FilterPreview preview(&p, "Gaussian Blur", false, false);
        preview.update({{"radius", 3.0}});
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > 0, 5000);
        const auto spread = shown(p, 19, 32);
        QVERIFY(spread.red() > before.red() + 20);
        QVERIFY(spread.red() > spread.green() + 20);
        QCOMPARE(p.document.active()->image.size(), QSize(20, 20)); // Only the preview grew.
        // A larger radius grows the preview further; a smaller one keeps it.
        preview.update({{"radius", 6.0}});
        QTRY_VERIFY_WITH_TIMEOUT(preview.shownCount() > 1, 5000);
        QVERIFY(shown(p, 12, 32).red() > before.red() + 5);
        preview.stop();
        QCOMPARE(shown(p, 19, 32), before);
    }
    void contentAwareFillReachesPastTheLayer() {
        auto d = Document::create({64, 64});
        QImage gray(32, 32, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                gray.setPixelColor(x, y, QColor(100 + (x * 3) % 40, 110, 120 + (y * 5) % 30));
        d.addImage("Texture", gray);
        QTemporaryDir dir;
        std::unique_ptr<EditorWindow> window(openWindow(dir, d));
        auto page = currentPage(*window);
        QImage mask(64, 64, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 8; y < 24; ++y)
            for (int x = 24; x < 40; ++x)
                mask.scanLine(y)[x] = 255;
        page->session.selection = mask;
        filterAction(*window, "Content-Aware Fill…")->trigger();
        QCOMPARE(page->history.count(), 1);
        const auto &layer = *page->document.active();
        QCOMPARE(boxOf(layer), QRectF(0, 0, 40, 32));
        const auto picture = renderDocument(page->document);
        QCOMPARE(picture.pixelColor(36, 16).alpha(), 255); // Filled past the old edge.
        QCOMPARE(picture.pixelColor(36, 28).alpha(), 0);   // Outside the selection: still empty.
    }
    void jpegsEncodeWithTheirMatteAndQuality() {
        QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        QPainter(&image).fillRect(0, 0, 32, 64, QColor(20, 120, 220));
        const auto black = QImage::fromData(encodeJpeg(image, 90, Qt::black), "JPEG");
        QVERIFY(black.pixelColor(50, 32).value() < 10);
        const auto white = QImage::fromData(encodeJpeg(image, 90, Qt::white), "JPEG");
        QVERIFY(white.pixelColor(50, 32).value() > 245);
        QVERIFY(encodeJpeg(image, 10).size() < encodeJpeg(image, 100).size());
    }
    void theJpegDialogPreviewsWhatIsExported() {
        QTemporaryDir dir;
        auto d = squareDocument();
        std::unique_ptr<EditorWindow> window(openWindow(dir, d));
        const auto path = dir.filePath("Out.jpg");
        bool visited = false;
        QByteArray previewed;
        QTimer::singleShot(20, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog && dialog->objectName() == "jpegExportDialog");
            auto close = qScopeGuard([dialog] {
                if (dialog->isVisible())
                    dialog->reject();
            });
            auto size = dialog->findChild<QLabel *>("jpegSize");
            auto exportButton = dialog->findChild<QPushButton *>("jpegExport");
            QTRY_VERIFY_WITH_TIMEOUT(exportButton->isEnabled(), 5000);
            QVERIFY(size->text().endsWith("KB"));
            QVERIFY(!dialog->findChild<QLabel *>("jpegPreview")->pixmap().isNull());
            dialog->findChild<QDoubleSpinBox *>("jpegQualityControl")->setValue(40);
            QVERIFY(!exportButton->isEnabled()); // Until the new encoding is ready.
            QTRY_VERIFY_WITH_TIMEOUT(exportButton->isEnabled(), 5000);
            previewed = encodeJpeg(renderDocument(currentPage(*window)->document), 40, Qt::white);
            visited = true;
            exportButton->click();
        });
        window->exportTo(path);
        QVERIFY(visited);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto written = file.readAll();
        QCOMPARE(written.size(), previewed.size());
        QCOMPARE(QSettings().value("export/jpegQuality").toInt(), 40);
        // Canceling writes nothing.
        const auto other = dir.filePath("Canceled.jpg");
        QTimer::singleShot(20, [] {
            if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        window->exportTo(other);
        QVERIFY(!QFile::exists(other));
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("FilterReachTests");
    FilterReachTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "filter_reach_tests.moc"

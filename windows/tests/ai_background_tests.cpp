// SPDX-License-Identifier: MIT
#include "ai_background.h"
#include "editor.h"
#include "language.h"
#include "render.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
using namespace compositor;
namespace {
QImage gray(QSize size, int value) {
    QImage image(size, QImage::Format_Grayscale8);
    image.fill(value);
    return image;
}
QAction *action(EditorWindow &w) {
    for (auto a : w.findChildren<QAction *>())
        if (a->property("layerAction").toString() == "Remove Background…")
            return a;
    return nullptr;
}
EditorPage *page(EditorWindow &w) {
    return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
}
QPushButton *ok(QDialog *d) {
    return d->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
}
class Fixture final : public AiSelectionService {
  public:
    std::atomic<int> calls{0}, delay{20};
    bool fail = false, empty = false;
    AiSelectionResult subject(const QImage &image,
                              std::shared_ptr<AiCancellation> cancel) override {
        return matte(image, cancel);
    }
    AiSelectionResult object(const QImage &, const QString &, const QVector<QPointF> &,
                             const QVector<int> &, std::shared_ptr<AiCancellation>) override {
        throw Error("Unused object inference");
    }
    AiSelectionResult matte(const QImage &image, std::shared_ptr<AiCancellation> cancel) override {
        ++calls;
        for (int i = 0; i < delay; ++i) {
            cancel->check();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        cancel->check();
        if (fail)
            throw Error("Fixture failure");
        auto mask = gray(image.size(), 0);
        if (!empty)
            for (int y = 0; y < image.height(); ++y)
                for (int x = image.width() / 3; x < 2 * image.width() / 3; ++x)
                    mask.scanLine(y)[x] = uchar(128 + (x % 3) * 50);
        return {mask, "CPU"};
    }
};
std::shared_ptr<Fixture> prepare(EditorWindow &w, QSize size = {64, 48}) {
    auto p = page(w);
    p->document = Document::create(size);
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(QColor(80, 170, 240, 180));
    p->document.addImage("Original", image);
    p->canvas->refresh();
    auto fixture = std::make_shared<Fixture>();
    p->aiSelection = fixture;
    return fixture;
}
void safety(EditorWindow &w) {
    QTimer::singleShot(15000, &w, [&w] {
        if (auto d = w.findChild<QDialog *>("aiBackgroundDialog"))
            d->reject();
    });
}
std::vector<double> mean(const std::vector<double> &in, int w, int h, int r) {
    std::vector<double> out(in.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double sum = 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    sum +=
                        in[size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1)];
            out[size_t(y) * w + x] = sum / ((2 * r + 1) * (2 * r + 1));
        }
    return out;
}
QImage bruteGuided(const QImage &mask, const QImage &guide, int r) {
    int w = mask.width(), h = mask.height();
    size_t count = size_t(w) * h;
    std::vector<double> p(count), g(count), gg(count), gp(count);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto i = size_t(y) * w + x;
            auto c = guide.pixelColor(x, y);
            g[i] = (.2126 * c.red() + .7152 * c.green() + .0722 * c.blue()) / 255;
            p[i] = mask.constScanLine(y)[x] / 255.;
            gg[i] = g[i] * g[i];
            gp[i] = g[i] * p[i];
        }
    auto mg = mean(g, w, h, r), mp = mean(p, w, h, r), ms = mean(gg, w, h, r),
         mc = mean(gp, w, h, r);
    for (size_t i = 0; i < count; ++i) {
        gg[i] = (mc[i] - mg[i] * mp[i]) / (std::max(0., ms[i] - mg[i] * mg[i]) + 1e-4);
        gp[i] = mp[i] - gg[i] * mg[i];
    }
    auto a = mean(gg, w, h, r), b = mean(gp, w, h, r);
    auto out = gray(mask.size(), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto i = size_t(y) * w + x;
            out.scanLine(y)[x] =
                uchar(std::clamp(std::lround((a[i] * g[i] + b[i]) * 255), 0L, 255L));
        }
    return out;
}
int maxDifference(const QImage &a, const QImage &b) {
    int error = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            error = std::max(error, std::abs(int(a.constScanLine(y)[x]) - b.constScanLine(y)[x]));
    return error;
}
class RecordingService final : public AiSelectionService {
  public:
    std::shared_ptr<AiSelectionService> inner;
    AiSelectionResult result;
    explicit RecordingService(std::shared_ptr<AiSelectionService> s) : inner(std::move(s)) {}
    AiSelectionResult subject(const QImage &i, std::shared_ptr<AiCancellation> c) override {
        return inner->subject(i, c);
    }
    AiSelectionResult matte(const QImage &i, std::shared_ptr<AiCancellation> c) override {
        result = inner->matte(i, c);
        return result;
    }
    AiSelectionResult object(const QImage &i, const QString &m, const QVector<QPointF> &p,
                             const QVector<int> &l, std::shared_ptr<AiCancellation> c) override {
        return inner->object(i, m, p, l, c);
    }
};
} // namespace
class BackgroundTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void softMaskGeometry() {
        QImage image(4, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        image.setPixelColor(3, 0, Qt::transparent);
        const auto raw = AiTensor::floats("logits", {1, 1, 1, 2},
                                          {float(std::log(1. / 3)), float(std::log(3.))});
        const auto soft = aiSubjectMatte(raw, image);
        QCOMPARE(soft.constScanLine(0)[0], uchar(64));
        QCOMPARE(soft.constScanLine(0)[1], uchar(96));
        QCOMPARE(soft.constScanLine(0)[2], uchar(159));
        QCOMPARE(soft.constScanLine(0)[3], uchar(0));
        const auto binary = aiSubjectSelection(raw, image);
        QCOMPARE(binary.constScanLine(0)[1], uchar(0));
        QCOMPARE(binary.constScanLine(0)[2], uchar(255));
        auto cancel = std::make_shared<AiCancellation>();
        cancel->cancel();
        QVERIFY_EXCEPTION_THROWN(aiSubjectMatte(raw, image, cancel), AiCancelled);
    }
    void guidedReference_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("radius");
        QTest::newRow("edges") << QSize(23, 11) << 4;
        QTest::newRow("tile-seams") << QSize(1031, 7) << 3;
        QTest::newRow("single-pixel") << QSize(1, 1) << 40;
    }
    void guidedReference() {
        QFETCH(QSize, size);
        QFETCH(int, radius);
        auto mask = gray(size, 0);
        QImage guide(size, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                mask.scanLine(y)[x] = uchar((x * 37 + y * 13) % 256);
                guide.setPixelColor(
                    x, y,
                    QColor((x * 17 + y * 11) % 256, (x * 3 + y * 41) % 256, (x * 5 + y * 7) % 256));
            }
        auto actual = refineBackgroundMatte(mask, guide, {true, radius, 0, 0});
        auto expected = bruteGuided(mask, guide, radius);
        QVERIFY(maxDifference(actual, expected) <= 1);
        QCOMPARE(refineBackgroundMatte(mask, guide, {false, radius, 100, 10}), mask);
        auto canceled = std::make_shared<AiCancellation>();
        canceled->cancel();
        QVERIFY_EXCEPTION_THROWN(refineBackgroundMatte(mask, guide, {true, radius, 0, 0}, canceled),
                                 AiCancelled);
    }
    void contrastAndShift() {
        auto mask = gray({101, 5}, 0);
        for (int y = 0; y < 5; ++y)
            for (int x = 30; x < 71; ++x)
                mask.scanLine(y)[x] = 255;
        QImage guide(mask.size(), QImage::Format_RGB32);
        guide.fill(Qt::gray);
        auto shrink = refineBackgroundMatte(mask, guide, {true, 0, 0, -6}),
             grow = refineBackgroundMatte(mask, guide, {true, 0, 0, 6});
        int a = 0, b = 0;
        for (int x = 0; x < 101; ++x) {
            a += shrink.constScanLine(2)[x] > 127;
            b += grow.constScanLine(2)[x] > 127;
        }
        QVERIFY(a < 41);
        QVERIFY(b > 41);
        QCOMPARE(shrink.constScanLine(2)[50], uchar(255));
        QCOMPARE(grow.constScanLine(2)[0], uchar(0));
        auto ramp = gray({256, 1}, 0);
        for (int x = 0; x < 256; ++x)
            ramp.scanLine(0)[x] = uchar(x);
        QImage grayGuide(ramp.size(), QImage::Format_RGB32);
        grayGuide.fill(Qt::gray);
        auto hard = refineBackgroundMatte(ramp, grayGuide, {true, 0, 100, 0});
        QVERIFY(hard.constScanLine(0)[126] < ramp.constScanLine(0)[126]);
        QVERIFY(hard.constScanLine(0)[129] > ramp.constScanLine(0)[129]);
        QVERIFY_EXCEPTION_THROWN(refineBackgroundMatte(mask, guide, {true, 41, 0, 0}), Error);
    }
    void existingMasksAndSelection() {
        auto d = Document::create({64, 64});
        QImage image(16, 16, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        d.addImage("Layer", image);
        auto layer = d.active();
        layer->setBounds({16, 16, 32, 32});
        layer->mask = gray({1, 1}, 128);
        auto selection = gray(d.size(), 0);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x)
                selection.scanLine(y)[x] = 255;
        auto matte = gray(image.size(), 0);
        auto out = backgroundLayerMask(d, *layer, matte, selection);
        QCOMPARE(out.constScanLine(8)[2], uchar(0));
        QCOMPARE(out.constScanLine(8)[12], uchar(128));
        selection.fill(128);
        out = backgroundLayerMask(d, *layer, matte, selection);
        QCOMPARE(out.constScanLine(8)[8], uchar(64));
        layer->mask = gray({64, 64}, 255);
        layer->metadata["maskPlacement"] = makeTransform({0, 0, 64, 64});
        layer->metadata["maskLinked"] = false;
        out = backgroundLayerMask(d, *layer, gray(image.size(), 128));
        QVERIFY(maxDifference(out, gray(image.size(), 128)) <= 1);
        const auto original = layer->image;
        installBackgroundMask(*layer, out);
        QCOMPARE(layer->image, original);
        QVERIFY(layer->metadata.value("maskLinked").toBool());
        QVERIFY(!layer->metadata.contains("maskPlacement"));
        auto rendered = renderDocument(d);
        QCOMPARE(rendered.pixelColor(32, 32).alpha(), 128);
        QTemporaryDir temp;
        auto path = temp.filePath("background.comp");
        saveProject(d, path);
        auto loaded = loadProject(path);
        QCOMPARE(loaded.active()->mask, out);
        QCOMPARE(loaded.active()->image, original);
        QCOMPARE(renderDocument(loaded), rendered);
    }
    void linkedMaskFollowsTransform() {
        auto document = Document::create({64, 64});
        QImage image(8, 8, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        document.addImage("Layer", image);
        auto layer = document.active();
        auto matte = gray(image.size(), 0);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 4; ++x)
                matte.scanLine(y)[x] = 255;
        installBackgroundMask(*layer, matte);
        layer->setBounds({4, 4, 8, 8});
        auto rendered = renderDocument(document);
        QCOMPARE(rendered.pixelColor(5, 8).alpha(), 255);
        QCOMPARE(rendered.pixelColor(10, 8).alpha(), 0);
        auto transform = layer->transform();
        transform["rotation"] = 90;
        layer->metadata["transform"] = transform;
        rendered = renderDocument(document);
        QCOMPARE(rendered.pixelColor(8, 5).alpha(), 255);
        QCOMPARE(rendered.pixelColor(8, 10).alpha(), 0);
        layer->setBounds({8, 8, 16, 16});
        rendered = renderDocument(document);
        QCOMPARE(rendered.pixelColor(16, 11).alpha(), 255);
        QCOMPARE(rendered.pixelColor(16, 20).alpha(), 0);
        layer->move({4, 4});
        rendered = renderDocument(document);
        QCOMPARE(rendered.pixelColor(20, 15).alpha(), 255);
        QCOMPARE(rendered.pixelColor(20, 24).alpha(), 0);
        QVERIFY(!layer->metadata.contains("maskPlacement"));
    }
    void previewApplyUndoAndCache() {
        EditorWindow w;
        auto fixture = prepare(w);
        auto p = page(w);
        const auto original = p->document.active()->image;
        const auto count = p->history.count();
        const auto selection = p->session.selection;
        bool verified = false;
        safety(w);
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("aiBackgroundDialog");
            QVERIFY(dialog);
            QTRY_VERIFY(ok(dialog)->isEnabled());
            QVERIFY(p->document.active()->mask.isNull());
            QVERIFY(!p->isModified());
            auto quality = dialog->findChild<QComboBox *>("aiBackgroundQuality");
            quality->setCurrentIndex(1);
            auto refine = dialog->findChild<QSpinBox *>("aiBackgroundRefine");
            refine->setValue(3);
            refine->setValue(4);
            dialog->findChild<QSpinBox *>("aiBackgroundContrast")->setValue(60);
            QTRY_VERIFY(ok(dialog)->isEnabled());
            QCOMPARE(fixture->calls.load(), 1);
            verified = true;
            ok(dialog)->click();
        });
        action(w)->trigger();
        QVERIFY(verified);
        QCOMPARE(p->history.count(), count + 1);
        QVERIFY(p->isModified());
        QCOMPARE(p->document.active()->image, original);
        QCOMPARE(p->session.selection, selection);
        const auto mask = p->document.active()->mask;
        QVERIFY(!mask.isNull());
        QCOMPARE(p->session.target, EditTarget::Mask);
        p->history.undo();
        QVERIFY(p->document.active()->mask.isNull());
        QVERIFY(!p->isModified());
        p->history.redo();
        QCOMPARE(p->document.active()->mask, mask);
    }
    void cancelAndFailure_data() {
        QTest::addColumn<int>("mode");
        for (int i = 0; i < 5; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void cancelAndFailure() {
        QFETCH(int, mode);
        EditorWindow w;
        auto fixture = prepare(w);
        auto p = page(w);
        const int count = p->history.count();
        if (mode == 1)
            fixture->delay = 500;
        if (mode == 2)
            fixture->fail = true;
        if (mode == 3)
            fixture->empty = true;
        bool checked = false;
        safety(w);
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("aiBackgroundDialog");
            QVERIFY(dialog);
            if (mode == 2 || mode == 3) {
                auto status = dialog->findChild<QLabel *>("aiBackgroundStatus");
                QTRY_VERIFY(
                    status->text().contains(mode == 2 ? "Fixture failure" : "No foreground"));
                QVERIFY(!ok(dialog)->isEnabled());
            } else if (mode != 1) {
                QTRY_VERIFY(ok(dialog)->isEnabled());
                if (mode == 4)
                    ok(dialog)->click();
            }
            checked = true;
            dialog->reject();
        });
        action(w)->trigger();
        QVERIFY(checked);
        QCOMPARE(p->history.count(), count);
        QVERIFY(p->document.active()->mask.isNull());
        QVERIFY(!p->isModified());
        QTest::qWait(40);
        QVERIFY(p->document.active()->mask.isNull());
    }
    void externalEditInvalidates() {
        EditorWindow w;
        prepare(w);
        auto p = page(w);
        bool checked = false;
        safety(w);
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("aiBackgroundDialog");
            QVERIFY(dialog);
            QTRY_VERIFY(ok(dialog)->isEnabled());
            checked = true;
            p->edit("External change",
                    [](Document &d) { d.active()->metadata["name"] = "Changed"; });
        });
        action(w)->trigger();
        QVERIFY(checked);
        QCOMPARE(p->history.count(), 1);
        QCOMPARE(p->document.active()->name(), QString("Changed"));
        QVERIFY(p->document.active()->mask.isNull());
    }
    void languages_data() {
        QTest::addColumn<QString>("language");
        for (auto s : {"en", "zh_CN", "ja_JP"})
            QTest::newRow(s) << QString(s);
    }
    void languages() {
        QFETCH(QString, language);
        UiLanguage::instance().setLanguage(language, false);
        EditorWindow w;
        prepare(w);
        bool checked = false;
        safety(w);
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("aiBackgroundDialog");
            QVERIFY(dialog);
            QTRY_VERIFY(ok(dialog)->isEnabled());
            dialog->findChild<QComboBox *>("aiBackgroundQuality")->setCurrentIndex(1);
            QTRY_VERIFY(ok(dialog)->isEnabled());
            QCOMPARE(dialog->windowTitle(), uiText("Remove Background"));
            if (qEnvironmentVariableIsSet("COMPOSITOR_AI_BACKGROUND_SCREENSHOTS"))
                dialog->grab().save("artifacts/ai3-dialog-" + language + ".png");
            checked = true;
            dialog->reject();
        });
        action(w)->trigger();
        QVERIFY(checked);
    }
    void realBackground_data() {
        QTest::addColumn<QString>("filename");
        for (auto s : {"truck.png", "cars.png", "groceries.png"})
            QTest::newRow(s) << QString(s);
    }
    void realBackground() {
        if (!qEnvironmentVariableIsSet("COMPOSITOR_AI_SELECTION_MODEL_ROOT"))
            QSKIP("Opt-in real background removal");
        QFETCH(QString, filename);
        try {
            const QDir references(qEnvironmentVariable("COMPOSITOR_AI_BACKGROUND_REFERENCES"));
            const QDir images(qEnvironmentVariable("COMPOSITOR_AI_SELECTION_REFERENCES"));
            const QImage image(images.filePath(filename));
            QVERIFY(!image.isNull());
            const auto expectedBasic = QImage(references.filePath("basic-" + filename))
                                           .convertToFormat(QImage::Format_Grayscale8);
            const auto expectedAdvanced = QImage(references.filePath("advanced-" + filename))
                                              .convertToFormat(QImage::Format_Grayscale8);
            QVERIFY(!expectedBasic.isNull());
            QVERIFY(!expectedAdvanced.isNull());
            QCOMPARE(expectedBasic.size(), image.size());
            QCOMPARE(expectedAdvanced.size(), image.size());
            AiOptions options;
            options.policy = qEnvironmentVariable("COMPOSITOR_AI_SELECTION_PROVIDER") == "dml"
                                 ? AiProviderPolicy::DirectMLOnly
                                 : AiProviderPolicy::CpuOnly;
            auto recorder = std::make_shared<RecordingService>(createAiSelectionService(
                qEnvironmentVariable("COMPOSITOR_AI_SELECTION_MODEL_ROOT"), options));
            EditorWindow w;
            auto p = page(w);
            p->document = Document::create(image.size());
            p->document.addImage("Original", image);
            p->canvas->refresh();
            p->aiSelection = recorder;
            const auto originalPixels = p->document.active()->image;
            bool checked = false;
            QTimer::singleShot(120000, &w, [&] {
                if (auto d = w.findChild<QDialog *>("aiBackgroundDialog"))
                    d->reject();
            });
            QTimer::singleShot(0, &w, [&] {
                auto dialog = w.findChild<QDialog *>("aiBackgroundDialog");
                QVERIFY(dialog);
                QTRY_VERIFY_WITH_TIMEOUT(ok(dialog)->isEnabled(), 60000);
                dialog->findChild<QComboBox *>("aiBackgroundQuality")->setCurrentIndex(1);
                QTRY_VERIFY_WITH_TIMEOUT(ok(dialog)->isEnabled(), 60000);
                checked = true;
                ok(dialog)->click();
            });
            action(w)->trigger();
            QVERIFY(checked);
            QVERIFY(!p->document.active()->mask.isNull());
            QCOMPARE(p->history.count(), 1);
            QCOMPARE(p->document.active()->image, originalPixels);
            QVERIFY(p->isModified());
            const auto actual = p->document.active()->mask;
            const int basicError = maxDifference(recorder->result.mask, expectedBasic),
                      advancedError = maxDifference(actual, expectedAdvanced);
            QVERIFY2(basicError <= 2, qPrintable(QString("Basic max error %1").arg(basicError)));
            QVERIFY2(advancedError <= 3,
                     qPrintable(QString("Advanced max error %1").arg(advancedError)));
            if (options.policy == AiProviderPolicy::DirectMLOnly)
                QVERIFY(recorder->result.backend.startsWith("DirectML"));
            else
                QCOMPARE(recorder->result.backend, QString("CPU"));
            p->history.undo();
            QVERIFY(p->document.active()->mask.isNull());
            QVERIFY(!p->isModified());
            p->history.redo();
            QCOMPARE(p->document.active()->mask, actual);
            const QString prefix = "artifacts/ai3-" +
                                   qEnvironmentVariable("COMPOSITOR_AI_SELECTION_PROVIDER", "cpu") +
                                   "-" + filename;
            recorder->result.mask.save(prefix + "-basic.png");
            actual.save(prefix + "-advanced.png");
            backgroundCutout(image, actual).save(prefix + "-cutout.png");
            QFile report(prefix + ".json");
            QVERIFY(report.open(QIODevice::WriteOnly));
            report.write(QJsonDocument(QJsonObject{{"image", filename},
                                                   {"backend", recorder->result.backend},
                                                   {"basic_max_byte_error", basicError},
                                                   {"advanced_max_byte_error", advancedError},
                                                   {"undo_redo", true},
                                                   {"pixels_preserved", true}})
                             .toJson());
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("AiBackgroundTests");
    BackgroundTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ai_background_tests.moc"

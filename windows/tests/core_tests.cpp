#include "blend.h"
#include "blend_reference.h"
#include "camera_raw.h"
#include "document.h"
#include "effects.h"
#include "filters.h"
#include "render.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>
using namespace compositor;

class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void cameraRawWhiteBalanceAndClipping() {
        QImage image(3, 1, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        image.setPixelColor(0, 0, QColor(120, 110, 100));
        image.setPixelColor(1, 0, QColor(120, 110, 100, 128));
        auto balance = cameraRawWhiteBalance(image);
        QVERIFY(!balance.isEmpty());
        auto neutral = applyFilter(image, "Camera Raw", balance).pixelColor(0, 0);
        QVERIFY(std::abs(neutral.red() - neutral.green()) <= 1 &&
                std::abs(neutral.green() - neutral.blue()) <= 1);
        QImage gray(1, 1, QImage::Format_RGBA8888_Premultiplied);
        gray.fill(QColor(100, 100, 100));
        QCOMPARE(cameraRawWhiteBalance(gray).value("temperature").toDouble(), 0.0);
        gray.fill(Qt::transparent);
        QVERIFY(cameraRawWhiteBalance(gray).isEmpty());
        image.setPixelColor(0, 0, Qt::white);
        image.setPixelColor(1, 0, QColor(0, 0, 0, 128));
        cameraRawPreviewOverlay(
            image, {{"previewShadowClipping", true}, {"previewHighlightClipping", true}});
        auto highlight = image.pixelColor(0, 0), shadow = image.pixelColor(1, 0);
        QVERIFY(highlight.red() > highlight.green() && highlight.green() == highlight.blue());
        QVERIFY(shadow.blue() > shadow.red() && shadow.red() == shadow.green());
        QCOMPARE(shadow.alpha(), 128);
        QCOMPARE(image.pixelColor(2, 0).alpha(), 0);
    }
    void cameraRawCurveMixerGrading() {
        QImage ramp(256, 1, QImage::Format_RGBA8888_Premultiplied);
        for (int x = 0; x < 256; ++x)
            ramp.setPixelColor(x, 0, QColor(x, x, x));
        auto bent = applyFilter(ramp, "Camera Raw", {{"curveDarks", -51}});
        QCOMPARE(bent.pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(bent.pixelColor(255, 0), QColor(Qt::white));
        QVERIFY(bent.pixelColor(64, 0).red() < 50);
        for (int x = 1; x < 256; ++x)
            QVERIFY(bent.pixelColor(x, 0).red() >= bent.pixelColor(x - 1, 0).red());
        QJsonArray invert{QJsonObject{{"x", 0}, {"y", 255}}, QJsonObject{{"x", 255}, {"y", 0}}};
        auto reversed = applyFilter(ramp, "Camera Raw", {{"curveRed", invert}});
        QCOMPARE(reversed.pixelColor(64, 0), QColor(191, 64, 64));
        QImage colors(3, 1, QImage::Format_RGBA8888_Premultiplied);
        colors.setPixelColor(0, 0, QColor(255, 0, 0, 128));
        colors.setPixelColor(1, 0, QColor(0, 0, 255, 128));
        colors.setPixelColor(2, 0, QColor(100, 100, 100, 128));
        auto mixed = applyFilter(colors, "Camera Raw", {{"mixerRedsSaturation", -100}});
        auto red = mixed.pixelColor(0, 0);
        QVERIFY(red.hslSaturationF() < .3);
        QCOMPARE(mixed.pixelColor(1, 0), colors.pixelColor(1, 0));
        QJsonObject point{
            {"hue", 0}, {"saturation", 1}, {"luminance", .5}, {"saturationShift", -100}};
        auto picked = applyFilter(colors, "Camera Raw", {{"pointColors", QJsonArray{point}}});
        QVERIFY(picked.pixelColor(0, 0).hslSaturationF() < .01);
        QCOMPARE(picked.pixelColor(1, 0), colors.pixelColor(1, 0));
        auto graded = applyFilter(colors, "Camera Raw",
                                  {{"gradeGlobalHue", 120}, {"gradeGlobalSaturation", 70}});
        QVERIFY(graded.pixelColor(2, 0).green() > graded.pixelColor(2, 0).red());
        for (int x = 0; x < 3; ++x) {
            auto p = graded.constScanLine(0) + x * 4;
            QCOMPARE(p[3], uchar(128));
            QVERIFY(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3]);
        }
    }
    void cameraRawGeometry() {
        QImage image(80, 60, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        QCOMPARE(applyFilter(image, "Camera Raw", {{"geometryUpright", true}}), image);
        auto rotated = applyFilter(image, "Camera Raw", {{"geometryRotate", 30}});
        QCOMPARE(rotated.size(), image.size());
        QCOMPARE(rotated.pixelColor(40, 30), QColor(Qt::white));
        QCOMPARE(rotated.pixelColor(0, 0).alpha(), 0);
        auto small = applyFilter(image, "Camera Raw", {{"geometryScale", -50}});
        QCOMPARE(small.pixelColor(5, 5).alpha(), 0);
        auto fitted = applyFilter(image, "Camera Raw",
                                  {{"geometryScale", -50}, {"geometryConstrainCrop", true}});
        QCOMPARE(fitted.pixelColor(5, 5), QColor(Qt::white));
        QJsonObject guide{{"startX", .1}, {"startY", .2}, {"endX", .8}, {"endY", .4}};
        auto guided =
            applyFilter(image, "Camera Raw",
                        {{"geometryUpright", true}, {"geometryGuides", QJsonArray{guide}}});
        QVERIFY(guided != image);
        QCOMPARE(guided, applyFilter(image, "Camera Raw", {{"geometryRotate", -15.9453959}}));
    }
    void additionalFiltersPreserveAlphaAndChangePixels() {
        QImage image(48, 48, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 48; ++x)
                image.setPixelColor(x, y,
                                    QColor(80 + x * 3, 80 + y * 3, 100, (x + y) % 5 ? 128 : 0));
        for (auto kind : {"Vignette", "Bloom / Glow", "Tonal Contrast"}) {
            auto out = applyFilter(image, kind);
            QVERIFY(out != image);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 48; ++x) {
                    auto p = out.constScanLine(y) + x * 4;
                    QCOMPARE(p[3], image.constScanLine(y)[x * 4 + 3]);
                    QVERIFY(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3]);
                }
        }
        auto identity = applyFilter(image, "Camera Raw");
        QCOMPARE(identity, image);
        for (auto key :
             {"temperature", "texture", "clarity", "dehaze", "sharpenAmount", "noiseLuminance",
              "noiseColor", "purpleAmount", "opticsVignetteAmount", "redHue"}) {
            QJsonObject s{{key, 50}};
            auto out = applyFilter(image, "Camera Raw", s);
            QVERIFY2(out != image, key);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 48; ++x) {
                    auto p = out.constScanLine(y) + x * 4;
                    QVERIFY(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3]);
                }
        }
        auto zero = applyFilter(image, "Bloom / Glow", {{"bloomAmount", 0}});
        QCOMPARE(zero, image);
    }
    void vignetteCanFillEmptyLayer() {
        QImage empty(64, 64, QImage::Format_RGBA8888_Premultiplied);
        empty.fill(Qt::transparent);
        auto out =
            applyFilter(empty, "Vignette",
                        {{"vignetteAmount", 100},
                         {"vignetteColor", QJsonObject{{"red", 1}, {"green", 0}, {"blue", 0}}}});
        QVERIFY(out.pixelColor(0, 0).alpha() > 0);
        QVERIFY(out.pixelColor(0, 0).red() > out.pixelColor(0, 0).blue());
        QCOMPARE(out.pixelColor(32, 32).alpha(), 0);
    }
    void duplicateFolderRemapsClippingAndAssets() {
        auto d = Document::create({2, 2});
        auto group = d.addGroup("Folder");
        d.addBlank("Base");
        auto base = d.activeId();
        d.active()->metadata["parentID"] = group;
        d.addBlank("Clipped");
        d.active()->metadata["parentID"] = group;
        d.active()->metadata["maskSourceID"] = base;
        auto next = d.duplicate(group);
        QCOMPARE(d.layers.size(), 6);
        QVERIFY(next != group);
        QCOMPARE(d.layers[4].parent(), next);
        QCOMPARE(d.layers[5].parent(), next);
        QCOMPARE(d.layers[5].metadata.value("maskSourceID").toString(), d.layers[4].id());
        QCOMPARE(d.layers[4].metadata.value("imageFile").toString(), d.layers[4].id() + ".png");
        d.validateAssets();
    }
    void curvesChannelsAndValidation() {
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(64, 128, 192));
        QJsonArray identity{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}};
        QJsonArray inverted{QJsonObject{{"x", 0}, {"y", 255}}, QJsonObject{{"x", 255}, {"y", 0}}};
        auto out = applyFilter(image, "Curves",
                               {{"channels", QJsonArray{identity, inverted, identity, identity}}});
        QCOMPARE(out.pixelColor(0, 0), QColor(191, 128, 192));
        QCOMPARE(applyFilter(image, "Curves"), image);
        QVERIFY_EXCEPTION_THROWN(
            applyFilter(image, "Curves",
                        {{"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}},
                                               QJsonObject{{"x", 0}, {"y", 255}}}}}),
            Error);
    }
    void hueRangesSwiftDictionaryAndColorize() {
        QImage image(3, 1, QImage::Format_RGBA8888_Premultiplied);
        image.setPixelColor(0, 0, Qt::red);
        image.setPixelColor(1, 0, Qt::blue);
        image.setPixelColor(2, 0, QColor(128, 128, 128));
        QJsonObject s{{"range", "Reds"},
                      {"adjustments", QJsonArray{"Reds", QJsonObject{{"saturation", -100}}}}};
        auto out = applyFilter(image, "Hue/Saturation", s);
        QCOMPARE(out.pixelColor(0, 0), QColor(128, 128, 128));
        QCOMPARE(out.pixelColor(1, 0), QColor(Qt::blue));
        QCOMPARE(out.pixelColor(2, 0), QColor(128, 128, 128));
        auto green = applyFilter(image, "Hue/Saturation",
                                 {{"colorize", true}, {"hue", 120}, {"saturation", 100}});
        QCOMPARE(green.pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(applyFilter(image, "Hue/Saturation"), image);
        auto a = makeAdjustment(
            "Hue/Saturation",
            {{"range", "Master"},
             {"adjustments", QJsonObject{{"Master", QJsonObject{{"lightness", 100}}}}}});
        QVERIFY(a.value("hsvSettings").toObject().value("adjustments").isArray());
        QCOMPARE(applyAdjustment(image, a).pixelColor(0, 0), QColor(Qt::white));
    }
    void effectStrokeAndDisabledOverlay() {
        QImage image(3, 3, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        image.setPixelColor(1, 1, Qt::red);
        QJsonObject stroke{{"size", 1}, {"red", 0}, {"green", 0}, {"blue", 1}};
        auto built = renderEffects(image, {{"stroke", stroke}});
        QCOMPARE(built.image.pixelColor(built.inset + 1, built.inset + 1), QColor(Qt::red));
        QCOMPARE(built.image.pixelColor(built.inset, built.inset + 1), QColor(Qt::blue));
        auto disabled = renderEffects(
            image, {{"colorOverlay",
                     QJsonObject{{"enabled", false}, {"red", 0}, {"green", 1}, {"blue", 0}}}});
        QCOMPARE(disabled.image.pixelColor(disabled.inset + 1, disabled.inset + 1),
                 QColor(Qt::red));
        QVERIFY_EXCEPTION_THROWN(renderEffects(image, {{"stroke", QJsonObject{{"size", 501}}}}),
                                 Error);
    }
    void effectsFollowMaskBeforeGrowing() {
        auto d = Document::create({7, 5});
        QImage image(3, 3, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        d.addImage("Shape", image);
        auto l = d.active();
        l->setBounds({2, 1, 3, 3});
        l->mask = QImage(3, 3, QImage::Format_Grayscale8);
        l->mask.fill(0);
        l->mask.scanLine(1)[1] = 255;
        l->metadata["maskFile"] = l->id() + ".mask.png";
        l->metadata["effects"] = QJsonObject{{"stroke", QJsonObject{{"size", 1}, {"blue", 1}}}};
        auto out = renderDocument(d);
        QCOMPARE(out.pixelColor(3, 2), QColor(Qt::red));
        QCOMPARE(out.pixelColor(2, 2), QColor(Qt::blue));
        QCOMPARE(out.pixelColor(1, 2).alpha(), 0);
        QVERIFY(d.previewLimitations().isEmpty());
    }
    void clippedAdjustmentPreservesSoftAlpha() {
        auto d = Document::create({1, 1});
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(100, 120, 140, 128));
        auto id = d.addImage("Base", image);
        auto l = *d.active();
        l.metadata.remove("imageFile");
        l.metadata["id"] = newId();
        l.metadata["name"] = "Invert";
        l.metadata["maskSourceID"] = id;
        l.metadata["adjustment"] = QJsonObject{{"kind", "Invert"}};
        l.image = {};
        d.layers.push_back(l);
        auto out = renderDocument(d).pixelColor(0, 0);
        QCOMPARE(out.alpha(), 128);
        QVERIFY(std::abs(out.red() - 155) <= 2);
        QVERIFY(std::abs(out.green() - 135) <= 2);
        QVERIFY(d.previewLimitations().isEmpty());
    }
    void groupedAdjustmentUsesFolderMaskAndOpacity() {
        auto d = Document::create({2, 1});
        QImage image(2, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::black);
        d.addImage("Background", image);
        auto folder = d.addGroup("Folder");
        d.active()->metadata["opacity"] = .5;
        d.active()->mask = QImage(2, 1, QImage::Format_Grayscale8);
        d.active()->mask.fill(0);
        d.active()->mask.scanLine(0)[0] = 255;
        d.active()->metadata["maskFile"] = folder + ".mask.png";
        Layer l;
        l.metadata = {{"id", newId()},
                      {"name", "Invert"},
                      {"isVisible", true},
                      {"parentID", folder},
                      {"transform", makeTransform({0, 0, 2, 1})},
                      {"adjustment", QJsonObject{{"kind", "Invert"}}}};
        d.layers.push_back(l);
        auto out = renderDocument(d);
        QCOMPARE(out.pixelColor(0, 0), QColor(128, 128, 128));
        QCOMPARE(out.pixelColor(1, 0), QColor(Qt::black));
    }
    void adjustmentBlendKeepsOriginalAlpha() {
        auto d = Document::create({1, 1});
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(128, 128, 128, 128));
        d.addImage("Base", image);
        Layer l;
        l.metadata = {{"id", newId()},
                      {"name", "Multiply"},
                      {"isVisible", true},
                      {"blendMode", "Multiply"},
                      {"transform", makeTransform({0, 0, 1, 1})},
                      {"adjustment", QJsonObject{{"kind", "Invert"}}}};
        d.layers.push_back(l);
        auto out = renderDocument(d).pixelColor(0, 0);
        QCOMPARE(out.alpha(), 128);
        QVERIFY(std::abs(out.red() - 64) <= 2);
    }
    void motionBlurDirection() {
        QImage image(9, 9, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        image.setPixelColor(4, 4, Qt::white);
        auto out = applyFilter(image, "Motion Blur", {{"distance", 4}, {"angle", 0}});
        QVERIFY(out.pixelColor(2, 4).alpha() > 0);
        QCOMPARE(out.pixelColor(4, 3).alpha(), 0);
        QCOMPARE(out.pixelColor(4, 4).alpha(), 51);
    }
    void foundationCoordinates() {
        auto d = Document::create({8, 6});
        QImage image(2, 2, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        auto id = d.addImage("Red", image);
        auto l = d.find(id);
        l->move({3, 2});
        auto p = l->placement(image.size());
        QCOMPARE(p.map(QPointF(0, 0)), QPointF(3, 2));
        QCOMPARE(p.map(QPointF(2, 2)), QPointF(5, 4));
    }
    void projectRoundTrip() {
        QTemporaryDir temp;
        auto d = Document::create({4, 3});
        QImage image(4, 3, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(30, 70, 110));
        d.addImage("测试图层", image);
        d.metadata["futureField"] = QJsonObject{{"keep", true}};
        d.active()->metadata["effects"] = QJsonObject{{"unknown", 42}};
        d.active()->metadata["text"] = QJsonObject{{"content", "Hello"},  {"fontName", "SegoeUI"},
                                                   {"fontSize", 18},      {"red", 0},
                                                   {"green", 0},          {"blue", 0},
                                                   {"alignment", "Left"}, {"tracking", 0},
                                                   {"leading", 0}};
        d.active()->mask = QImage(4, 3, QImage::Format_Grayscale8);
        d.active()->mask.fill(128);
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        auto path = temp.filePath("测试.comp");
        saveProject(d, path);
        auto loaded = loadProject(path);
        QCOMPARE(loaded.manifest(), d.manifest());
        QCOMPARE(loaded.active()->image.pixelColor(1, 1), QColor(30, 70, 110));
        QCOMPARE(loaded.active()->mask.constScanLine(1)[1], uchar(128));
        saveProject(loaded, path);
        QCOMPARE(loadProject(path).manifest(), d.manifest());
    }
    void formatVersions_data() {
        QTest::addColumn<int>("version");
        for (int v = 1; v <= 11; ++v)
            QTest::newRow(qPrintable(QString::number(v))) << v;
    }
    void formatVersions() {
        QFETCH(int, version);
        auto d = Document::create({1, 1});
        d.metadata["version"] = version;
        d.validate();
        QTemporaryDir temp;
        auto path = temp.filePath("legacy.comp");
        saveProject(d, path);
        QCOMPARE(loadProject(path).metadata.value("version").toInt(), version);
    }
    void rejectsFutureVersion() {
        auto d = Document::create({1, 1});
        d.metadata["version"] = 12;
        QVERIFY_EXCEPTION_THROWN(d.validate(), Error);
    }
    void rejectsUnsafeAsset() {
        auto d = Document::create({1, 1});
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        d.addImage("Layer", image);
        d.active()->metadata["imageFile"] = "../outside.png";
        QVERIFY_EXCEPTION_THROWN(d.validate(), Error);
    }
    void rejectsDuplicateIdCaseInsensitive() {
        auto d = Document::create({1, 1});
        d.addGroup("A");
        auto copy = *d.active();
        copy.metadata["id"] = copy.id().toLower();
        d.layers.push_back(copy);
        QVERIFY_EXCEPTION_THROWN(d.validate(), Error);
    }
    void rejectsFolderCycle() {
        auto d = Document::create({1, 1});
        auto a = d.addGroup("A"), b = d.addGroup("B");
        d.find(a)->metadata["parentID"] = b;
        d.find(b)->metadata["parentID"] = a;
        QVERIFY_EXCEPTION_THROWN(d.validate(), Error);
    }
    void rejectsClippingCycle() {
        auto d = Document::create({1, 1});
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        auto a = d.addImage("A", image), b = d.addImage("B", image);
        d.find(a)->metadata["maskSourceID"] = b;
        d.find(b)->metadata["maskSourceID"] = a;
        QVERIFY_EXCEPTION_THROWN(d.validate(), Error);
    }
    void failedSavePreservesPrevious() {
        QTemporaryDir temp;
        auto path = temp.filePath("test.comp");
        auto d = Document::create({1, 1});
        saveProject(d, path);
        QFile before(QDir(path).filePath("manifest.json"));
        QVERIFY(before.open(QIODevice::ReadOnly));
        auto bytes = before.readAll();
        before.close();
        d.metadata["version"] = 99;
        QVERIFY_EXCEPTION_THROWN(saveProject(d, path), Error);
        QFile after(QDir(path).filePath("manifest.json"));
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), bytes);
    }
    void refusesUnrelatedFolder() {
        QTemporaryDir temp;
        auto path = temp.filePath("unrelated.comp");
        QDir().mkpath(path);
        QFile file(QDir(path).filePath("precious.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep");
        file.close();
        QVERIFY_EXCEPTION_THROWN(saveProject(Document::create({1, 1}), path), Error);
        QVERIFY(file.exists());
    }
    void blendModes_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : compositor::blendModes())
            QTest::newRow(qPrintable(mode)) << mode;
    }
    void blendModes() {
        QFETCH(QString, mode);
        auto d = Document::create({1, 1});
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(100, 150, 200));
        d.addImage("Image", image);
        d.active()->metadata["blendMode"] = mode;
        auto rendered = renderDocument(d);
        QCOMPARE(rendered.pixelColor(0, 0), QColor(100, 150, 200));
    }
    void multiply() {
        auto d = Document::create({1, 1});
        QImage a(1, 1, QImage::Format_RGBA8888_Premultiplied);
        a.fill(QColor(128, 128, 128));
        d.addImage("Base", a);
        d.addImage("Top", a);
        d.active()->metadata["blendMode"] = "Multiply";
        auto p = renderDocument(d).pixelColor(0, 0);
        QVERIFY(std::abs(p.red() - 64) <= 1);
        QCOMPARE(p.alpha(), 255);
    }
    void optimizedBlendMatchesBaseline_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : compositor::blendModes())
            QTest::newRow(qPrintable(mode)) << mode;
    }
    void optimizedBlendMatchesBaseline() {
        QFETCH(QString, mode);
        QRandomGenerator random(4703);
        for (auto size : {QSize(17, 9), QSize(257, 259)}) {
            QImage back(size, QImage::Format_RGBA8888_Premultiplied), front = back.copy();
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    auto pixel = [&] {
                        auto alpha = (x % 7 == 0)   ? 0
                                     : (x % 7 == 1) ? 255
                                                    : int(random.bounded(256));
                        return QColor(int(random.bounded(256)), int(random.bounded(256)),
                                      int(random.bounded(256)), alpha);
                    };
                    back.setPixelColor(x, y, pixel());
                    front.setPixelColor(x, y, pixel());
                }
            auto beforeBack = back.copy(), beforeFront = front.copy();
            for (double opacity : {0.0, .13, .5, .73, 1.0}) {
                auto expected = back;
                blend_reference::referenceComposite(expected, front, mode, opacity);
                auto actual = back;
                composite(actual, front, parseBlendMode(mode), opacity);
                QCOMPARE(actual, expected);
                QCOMPARE(back, beforeBack);
                QCOMPARE(front, beforeFront);
            }
        }
    }
    void compositingSupportsSharedSource() {
        QImage image(257, 259, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(20, 170, 240, 128));
        auto before = image.copy(), expected = image;
        blend_reference::referenceComposite(expected, before, "Multiply", .7);
        composite(image, image, BlendMode::Multiply, .7);
        QCOMPARE(image, expected);
    }
    void blendTiming() {
        if (!qEnvironmentVariableIsSet("COMPOSITOR_BENCHMARK"))
            QSKIP("Set COMPOSITOR_BENCHMARK=1 to run the optional baseline comparison.");
        QImage front(1600, 1000, QImage::Format_RGBA8888_Premultiplied);
        front.fill(QColor(170, 55, 210, 128));
        for (auto mode : {QString("Normal"), QString("Multiply")}) {
            auto baseline = front.copy(), optimized = front.copy();
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 5; ++i)
                blend_reference::referenceComposite(baseline, front, mode, .73);
            auto baselineNs = timer.nsecsElapsed();
            timer.restart();
            for (int i = 0; i < 5; ++i)
                composite(optimized, front, parseBlendMode(mode), .73);
            auto optimizedNs = timer.nsecsElapsed();
            QCOMPARE(optimized, baseline);
            qInfo().noquote() << QString("%1: baseline %2 ms, optimized %3 ms, speedup %4x")
                                     .arg(mode)
                                     .arg(baselineNs / 1e6, 0, 'f', 2)
                                     .arg(optimizedNs / 1e6, 0, 'f', 2)
                                     .arg(double(baselineNs) / optimizedNs, 0, 'f', 2);
        }
    }
    void folderOpacityAndMask() {
        auto d = Document::create({2, 1});
        QImage image(2, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        auto id = d.addImage("Image", image);
        auto group = d.addGroup("Folder");
        d.find(id)->metadata["parentID"] = group;
        d.find(id)->metadata["opacity"] = 0.5;
        d.find(group)->metadata["opacity"] = 0.5;
        d.find(group)->mask = QImage(2, 1, QImage::Format_Grayscale8);
        d.find(group)->mask.fill(255);
        d.find(group)->mask.scanLine(0)[1] = 0;
        d.find(group)->metadata["maskFile"] = group + ".mask.png";
        auto out = renderDocument(d);
        QVERIFY(std::abs(out.pixelColor(0, 0).alpha() - 64) <= 1);
        QCOMPARE(out.pixelColor(1, 0).alpha(), 0);
    }
    void invisibleClippingSource() {
        auto d = Document::create({2, 1});
        QImage base(2, 1, QImage::Format_RGBA8888_Premultiplied);
        base.fill(Qt::transparent);
        base.setPixelColor(0, 0, Qt::red);
        auto a = d.addImage("Base", base);
        d.find(a)->metadata["isVisible"] = false;
        QImage top(2, 1, QImage::Format_RGBA8888_Premultiplied);
        top.fill(Qt::blue);
        auto b = d.addImage("Top", top);
        d.find(b)->metadata["maskSourceID"] = a;
        auto out = renderDocument(d);
        QCOMPARE(out.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(out.pixelColor(1, 0).alpha(), 0);
    }
    void rotationAndFlip() {
        auto d = Document::create({2, 1});
        QImage image(2, 1, QImage::Format_RGBA8888_Premultiplied);
        image.setPixelColor(0, 0, Qt::red);
        image.setPixelColor(1, 0, Qt::blue);
        d.addImage("Pixels", image);
        auto t = d.active()->transform();
        t["flipX"] = true;
        t["sampling"] = "Nearest";
        d.active()->metadata["transform"] = t;
        auto out = renderDocument(d);
        QCOMPARE(out.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(out.pixelColor(1, 0), QColor(Qt::red));
    }
    void originalNoiseIsDeterministic() {
        QImage image(5, 5, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(90, 130, 170, 128));
        auto a = applyFilter(image, "Add Noise", {{"seed", 3}, {"amount", 30}}),
             b = applyFilter(image, "Add Noise", {{"seed", 3}, {"amount", 30}}),
             c = applyFilter(image, "Add Noise", {{"seed", 4}, {"amount", 30}});
        QCOMPARE(a, b);
        QVERIFY(a != c);
        QCOMPARE(a.pixelColor(0, 0).alpha(), 128);
    }
    void exposureIdentityAndInvert() {
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(60, 100, 140));
        QCOMPARE(applyFilter(image, "Exposure"), image);
        auto c = applyFilter(image, "Invert").pixelColor(0, 0);
        QCOMPARE(c, QColor(195, 155, 115));
    }
    void selectionLimitsFilter() {
        QImage source(2, 1, QImage::Format_RGBA8888_Premultiplied);
        source.fill(Qt::black);
        auto modified = applyFilter(source, "Invert");
        QImage mask(2, 1, QImage::Format_Grayscale8);
        mask.fill(0);
        mask.scanLine(0)[0] = 255;
        auto out = limitToSelection(source, modified, mask);
        QCOMPARE(out.pixelColor(0, 0), QColor(Qt::white));
        QCOMPARE(out.pixelColor(1, 0), QColor(Qt::black));
    }
    void rejectsMissingImage() {
        auto d = Document::create({1, 1});
        QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        d.addImage("Image", image);
        d.active()->image = {};
        QVERIFY_EXCEPTION_THROWN(d.validateAssets(), Error);
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"

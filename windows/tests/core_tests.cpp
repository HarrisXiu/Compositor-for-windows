#include "blend.h"
#include "blend_reference.h"
#include "camera_raw.h"
#include "distort.h"
#include "document.h"
#include "effects.h"
#include "filters.h"
#include "paint_surface.h"
#include "render.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentMap>
#include <QtTest>
#include <numeric>
using namespace compositor;

class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void aMaskPlacedApartKeepsItsEdgeTone() {
        // As on the Mac: past a mask placed apart from its layer, and wherever it grows or is
        // distorted past its pixels, a mask shows the tone most of its edge has.
        for (int tone : {255, 0}) {
            QImage mask(10, 10, QImage::Format_Grayscale8);
            mask.fill(tone);
            for (int y = 4; y < 6; ++y)
                for (int x = 4; x < 6; ++x)
                    mask.scanLine(y)[x] = uchar(255 - tone);
            QCOMPARE(maskBackground(mask), tone);
            auto d = Document::create({20, 10});
            QImage red(20, 10, QImage::Format_RGBA8888_Premultiplied);
            red.fill(Qt::red);
            d.addImage("Red", red);
            auto layer = d.active();
            layer->mask = mask;
            layer->metadata["maskFile"] = d.activeId() + ".mask.png";
            layer->metadata["maskLinked"] = false;
            layer->metadata["maskPlacement"] = makeTransform({0, 0, 10, 10});
            const auto shown = renderDocument(d);
            QCOMPARE(qAlpha(shown.pixel(2, 2)), tone);
            QCOMPARE(qAlpha(shown.pixel(4, 4)), 255 - tone);
            QCOMPARE(qAlpha(shown.pixel(15, 5)), tone); // Past the mask, inside the layer.
            // The same through the tiled renderer.
            const auto area = renderArea(d, {{20, 10}, {12, 0, 8, 10}});
            QCOMPARE(qAlpha(area.pixel(3, 5)), tone);
            // Painting past the mask grows it in its edge tone.
            Layer grown = *layer;
            growPaintSurface(grown, true, {-5, 0, 15, 10});
            QCOMPARE(grown.mask.width(), 15);
            QCOMPARE(int(grown.mask.constScanLine(5)[0]), tone);
            // A distortion of it fills its edge tone outside the new shape.
            const QVector<QPointF> corners{{3, 0}, {7, 0}, {10, 10}, {0, 10}};
            const auto warped = warpImage(mask, makeTransform({0, 0, 10, 10}), corners, true);
            QCOMPARE(int(warped.image.constScanLine(0)[0]), tone);
        }
        // A reveal-all mask at a fractional position leaves no seam along its edge.
        auto d = Document::create({20, 10});
        QImage red(20, 10, QImage::Format_RGBA8888_Premultiplied);
        red.fill(Qt::red);
        d.addImage("Red", red);
        QImage white(10, 10, QImage::Format_Grayscale8);
        white.fill(255);
        d.active()->mask = white;
        d.active()->metadata["maskFile"] = d.activeId() + ".mask.png";
        d.active()->metadata["maskLinked"] = false;
        d.active()->metadata["maskPlacement"] = makeTransform({3.4, 0.3, 10, 10});
        const auto shown = renderDocument(d);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 20; ++x)
                QCOMPARE(qAlpha(shown.pixel(x, y)), 255);
    }
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
    // Layers that exercise everything an area render can get wrong: transformed, shrunk and
    // nearest-sampled layers, a mask placed on its own, clipping with an adjustment inside,
    // a masked folder, effects, and adjustments that read their neighbors or their position.
    static Document areaDocument(QStringList *ids = nullptr) {
        auto d = Document::create({97, 61});
        QRandomGenerator random(1301);
        auto pixels = [&](QSize size, int alphaFloor) {
            QImage image(size, QImage::Format_RGBA8888_Premultiplied);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    image.setPixelColor(x, y,
                                        QColor(int(random.bounded(256)), int(random.bounded(256)),
                                               int(random.bounded(256)),
                                               alphaFloor + int(random.bounded(256 - alphaFloor))));
            return image;
        };
        auto gradient = [](QSize size) {
            QImage mask(size, QImage::Format_Grayscale8);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    mask.scanLine(y)[x] = uchar((x * 255 / size.width() + y * 7) % 256);
            return mask;
        };
        auto adjustment = [&](const QJsonObject &a, const QString &clipTo = {}) {
            Layer l;
            l.metadata = {{"id", newId()},
                          {"name", a.value("kind")},
                          {"isVisible", true},
                          {"transform", makeTransform({0, 0, 97, 61})},
                          {"adjustment", a}};
            if (!clipTo.isEmpty())
                l.metadata["maskSourceID"] = clipTo;
            d.layers.push_back(l);
            return l.id();
        };
        auto base = d.addImage("Base", pixels({97, 61}, 255));
        auto rotated = d.addImage("Rotated", pixels({40, 30}, 0));
        auto t = d.active()->transform();
        t["origin"] = QJsonArray{20.5, 9};
        t["size"] = QJsonArray{51, 37};
        t["rotation"] = 23;
        d.active()->metadata["transform"] = t;
        d.active()->metadata["blendMode"] = "Overlay";
        d.active()->mask = gradient({20, 20});
        d.active()->metadata["maskFile"] = rotated + ".mask.png";
        d.active()->metadata["maskLinked"] = false;
        d.active()->metadata["maskPlacement"] = makeTransform({10, 5, 60, 40});
        auto clipped = d.addImage("Clipped", pixels({30, 30}, 128));
        d.active()->setBounds({30, 10, 30, 30});
        d.active()->metadata["maskSourceID"] = rotated;
        d.active()->metadata["blendMode"] = "Multiply";
        adjustment(makeAdjustment("Hue/Saturation", {{"hue", 40}, {"saturation", 20}}), rotated);
        auto folder = d.addGroup("Folder");
        d.active()->metadata["opacity"] = .8;
        d.active()->mask = gradient({97, 61});
        d.active()->metadata["maskFile"] = folder + ".mask.png";
        auto inside = d.addImage("Inside", pixels({25, 25}, 200));
        d.active()->metadata["parentID"] = folder;
        d.active()->setBounds({60, 30, 25, 25});
        t = d.active()->transform();
        t["sampling"] = "Nearest";
        d.active()->metadata["transform"] = t;
        d.active()->metadata["effects"] =
            QJsonObject{{"shadow", QJsonObject{{"distance", 4}, {"blur", 2}}},
                        {"stroke", QJsonObject{{"size", 2}, {"red", 1}}}};
        auto shrunk = d.addImage("Shrunk", pixels({300, 200}, 255));
        d.active()->setBounds({5, 35, 33, 22});
        auto blur = adjustment({{"kind", "Gaussian Blur"}, {"blurRadius", 2.5}});
        adjustment({{"kind", "Motion Blur"}, {"motionAngle", 30}, {"motionDistance", 7}});
        adjustment({{"kind", "Add Noise"}, {"noiseAmount", 25}, {"noiseSeed", 5}});
        adjustment({{"kind", "Grain"},
                    {"grainSettings", QJsonObject{{"amount", 40}, {"size", 2}, {"seed", 9}}}});
        if (ids)
            *ids = {base, rotated, clipped, folder, inside, shrunk, blur};
        return d;
    }
    void areasMatchFullRender_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("document size") << QSize(97, 61);
        QTest::newRow("reduced") << QSize(40, 25);
        QTest::newRow("enlarged") << QSize(150, 95);
    }
    // Qt resamples a transformed image stepping from the first pixel it draws, so where an area
    // starts can round a resampled pixel one level differently; nothing else may differ.
    void areasMatchFullRender() {
        QFETCH(QSize, size);
        auto d = areaDocument();
        auto full = renderDocument(d, size);
        int worst = 0, differing = 0;
        for (int y = 0; y < size.height(); y += 17)
            for (int x = 0; x < size.width(); x += 23) {
                auto pixels = QRect(x, y, 23, 17).intersected(QRect(QPoint(), size));
                auto area = renderArea(d, {size, pixels, false, false});
                auto expected = full.copy(pixels);
                for (int row = 0; row < pixels.height(); ++row)
                    for (int byte = 0; byte < pixels.width() * 4; ++byte) {
                        int difference = std::abs(area.constScanLine(row)[byte] -
                                                  expected.constScanLine(row)[byte]);
                        worst = std::max(worst, difference);
                        differing += difference > 0;
                    }
            }
        QVERIFY2(worst <= 1, qPrintable(QString("differs by up to %1").arg(worst)));
        QVERIFY2(differing * 20 <= size.width() * size.height() * 4,
                 qPrintable(QString("%1 channels differ").arg(differing)));
    }
    void backdropThenOverMatchesArea() {
        QStringList ids;
        auto d = areaDocument(&ids);
        RenderArea area{d.size(), QRect(13, 7, 50, 40)};
        auto expected = renderArea(d, area);
        for (const auto &id : ids)
            QCOMPARE(renderOver(d, area, id, renderBackdrop(d, area, id)), expected);
    }
    void backdropOutlivesEditsToItsLayer() {
        QStringList ids;
        auto d = areaDocument(&ids);
        RenderArea area{d.size(), QRect(0, 0, 97, 61)};
        for (const auto &id : {ids[1], ids[4]}) {
            auto backdrop = renderBackdrop(d, area, id);
            auto edited = d;
            auto l = edited.find(id);
            l->image.detach();
            QPainter p(&l->image);
            p.fillRect(QRect(3, 3, 9, 6), QColor(10, 220, 90));
            p.end();
            l->move({4, -3});
            QCOMPARE(renderOver(edited, area, id, backdrop), renderArea(edited, area));
        }
    }
    void halvingsKeepReductionsSharp() {
        auto d = Document::create({512, 512});
        QImage checker(512, 512, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x)
                checker.setPixelColor(x, y, (x + y) % 2 ? Qt::white : Qt::black);
        d.addImage("Checker", checker);
        const QSize size(80, 80);
        auto sharp = renderArea(d, {size, QRect(QPoint(), size), true});
        auto plain = renderArea(d, {size, QRect(QPoint(), size)});
        QCOMPARE(plain, renderDocument(d, size));
        int sharpError = 0, plainError = 0;
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                sharpError = std::max(sharpError, std::abs(sharp.pixelColor(x, y).red() - 128));
                plainError = std::max(plainError, std::abs(plain.pixelColor(x, y).red() - 128));
            }
        QVERIFY2(sharpError <= 2, qPrintable(QString::number(sharpError)));
        QVERIFY2(plainError > 40, qPrintable(QString::number(plainError)));
    }
    // Every effect at once, then the inside stroke, which erodes and so treats edges differently.
    static QJsonObject allEffects() {
        return {{"shadow", QJsonObject{{"distance", 9}, {"angle", 35}, {"blur", 4}}},
                {"outerGlow", QJsonObject{{"size", 3}}},
                {"innerGlow", QJsonObject{{"size", 4}}},
                {"innerShadow", QJsonObject{{"distance", 5}, {"blur", 3}}},
                {"colorOverlay", QJsonObject{{"green", 1}, {"opacity", .3}}},
                {"stroke", QJsonObject{{"size", 3}, {"red", 1}}}};
    }
    static QJsonObject insideStroke() {
        return {{"stroke", QJsonObject{{"size", 4}, {"inside", true}, {"blue", 1}}},
                {"shadow", QJsonObject{{"distance", 6}, {"blur", 2}}}};
    }
    // Opaque blobs on transparency, so the effects have edges to work on.
    static QImage blobs(QSize size, quint32 seed) {
        QRandomGenerator random(seed);
        QImage image(size, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < 12; ++i) {
            p.setBrush(QColor(int(random.bounded(256)), int(random.bounded(256)),
                              int(random.bounded(256)), 128 + int(random.bounded(128))));
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(random.bounded(size.width()), random.bounded(size.height())),
                          8 + random.bounded(30), 8 + random.bounded(30));
        }
        return image;
    }
    void effectsUpdateMatchesRebuild_data() {
        QTest::addColumn<QJsonObject>("effects");
        QTest::addColumn<QRect>("changed");
        QTest::newRow("all, inside") << allEffects() << QRect(40, 30, 25, 20);
        QTest::newRow("all, at the edge") << allEffects() << QRect(0, 70, 30, 20);
        QTest::newRow("inside stroke") << insideStroke() << QRect(55, 10, 40, 30);
    }
    void effectsUpdateMatchesRebuild() {
        QFETCH(QJsonObject, effects);
        QFETCH(QRect, changed);
        auto image = blobs({120, 90}, 5);
        auto built = renderEffects(image, effects);
        auto edited = image.copy();
        QPainter p(&edited);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(changed, Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setBrush(QColor(250, 40, 90));
        p.drawEllipse(changed);
        p.end();
        updateEffects(built, effects, changed, [&](const QRect &r) { return edited.copy(r); });
        auto rebuilt = renderEffects(edited, effects);
        QCOMPARE(built.inset, rebuilt.inset);
        QCOMPARE(built.image, rebuilt.image);
    }
    void carriedCachesMatchRebuilt_data() {
        QTest::addColumn<QJsonObject>("effects");
        QTest::addColumn<QSize>("maskSize");
        QTest::newRow("plain, rescaled mask") << QJsonObject() << QSize(399, 301);
        QTest::newRow("effects") << allEffects() << QSize(400, 300);
        QTest::newRow("inside stroke") << insideStroke() << QSize(400, 300);
    }
    void carriedCachesMatchRebuilt() {
        QFETCH(QJsonObject, effects);
        QFETCH(QSize, maskSize);
        auto d = Document::create({400, 300});
        d.addImage("Pixels", blobs({400, 300}, 29));
        auto l = d.active();
        l->mask = QImage(maskSize, QImage::Format_Grayscale8);
        for (int y = 0; y < l->mask.height(); ++y)
            for (int x = 0; x < l->mask.width(); ++x)
                l->mask.scanLine(y)[x] = uchar(255 - x * 128 / l->mask.width());
        l->metadata["maskFile"] = l->id() + ".mask.png";
        if (!effects.isEmpty())
            l->metadata["effects"] = effects;
        // At full size and at a quarter, where the pixels, mask and effects are drawn from
        // twice-halved copies.
        const RenderArea full{{400, 300}, QRect(0, 0, 400, 300)},
            quarter{{100, 75}, QRect(0, 0, 100, 75), true};
        renderArea(d, full);
        renderArea(d, quarter);
        const auto imageKey = l->image.cacheKey(), maskKey = l->mask.cacheKey();
        const QRect changed(37, 21, 51, 33);
        for (int y = changed.top(); y <= changed.bottom(); ++y) {
            auto p = l->image.scanLine(y);
            auto m = l->mask.scanLine(y);
            for (int x = changed.left(); x <= changed.right(); ++x) {
                p[x * 4 + 3] = uchar(255 - p[x * 4 + 3]);
                for (int c = 0; c < 3; ++c)
                    p[x * 4 + c] = std::min(p[x * 4 + c], p[x * 4 + 3]);
                m[x] = uchar(x * 3);
            }
        }
        carryRenderCaches(*l, false, imageKey, changed);
        carryRenderCaches(*l, true, maskKey, changed);
        const auto carriedFull = renderArea(d, full), carriedQuarter = renderArea(d, quarter);
        // Copies have new cache keys, so everything is built again from the edited pixels.
        l->image = l->image.copy();
        l->mask = l->mask.copy();
        QCOMPARE(carriedFull, renderArea(d, full));
        QCOMPARE(carriedQuarter, renderArea(d, quarter));
    }
    void layerExtentIncludesEffects() {
        auto d = Document::create({50, 50});
        QImage image(20, 20, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        d.addImage("Shape", image);
        d.active()->setBounds({10, 10, 20, 20});
        QCOMPARE(layerExtent(*d.active()), QRectF(10, 10, 20, 20));
        d.active()->metadata["effects"] =
            QJsonObject{{"shadow", QJsonObject{{"distance", 5}, {"blur", 2}}}};
        QCOMPARE(layerExtent(*d.active()), QRectF(-3, -3, 46, 46));
        d.addGroup("Folder");
        QVERIFY(layerExtent(*d.active()).isNull());
    }
    void renderReachCoversBlurs() {
        auto d = Document::create({100, 50});
        QCOMPARE(renderReach(d, {100, 50}), 0);
        for (const auto &a : {QJsonObject{{"kind", "Gaussian Blur"}, {"blurRadius", 2}},
                              QJsonObject{{"kind", "Motion Blur"}, {"motionDistance", 7}}}) {
            Layer l;
            l.metadata = {{"id", newId()},
                          {"isVisible", true},
                          {"transform", makeTransform({0, 0, 100, 50})},
                          {"adjustment", a}};
            d.layers.push_back(l);
        }
        QCOMPARE(renderReach(d, {100, 50}), 6 + 6);
        QCOMPARE(renderReach(d, {50, 25}), 3 + 4);
    }
    void tiledRenderingTiming() {
        if (!qEnvironmentVariableIsSet("COMPOSITOR_BENCHMARK"))
            QSKIP("Set COMPOSITOR_BENCHMARK=1 to run the optional canvas rendering comparison.");
        // 6000 x 4000 with a background and 19 more layers, some blended; the size the plan's
        // acceptance target names.
        auto d = Document::create({6000, 4000});
        QImage background(6000, 4000, QImage::Format_RGBA8888_Premultiplied);
        background.fill(QColor(40, 60, 90));
        d.addImage("Background", background);
        QRandomGenerator random(77);
        const QStringList modes{"Normal", "Multiply", "Screen", "Overlay"};
        for (int i = 0; i < 19; ++i) {
            QImage image(1800, 1200, QImage::Format_RGBA8888_Premultiplied);
            image.fill(QColor(int(random.bounded(256)), int(random.bounded(256)),
                              int(random.bounded(256)), 160));
            d.addImage(QString("Layer %1").arg(i), image);
            d.active()->setBounds(
                {double(random.bounded(4200)), double(random.bounded(2800)), 1800, 1200});
            d.active()->metadata["blendMode"] = modes[i % modes.size()];
        }
        const auto layerId = d.activeId();
        QElapsedTimer timer;
        timer.start();
        renderDocument(d, {1600, 1067});
        const auto previewMs = timer.nsecsElapsed() / 1e6;
        // A 1440 x 900 view at 100%: the 256-pixel tiles it touches, rendered as the canvas does.
        QVector<RenderArea> view;
        for (int y = 1536; y < 1536 + 900 + 256; y += 256)
            for (int x = 2048; x < 2048 + 1440 + 256; x += 256)
                view.push_back({d.size(), QRect(x, y, 256, 256), true, false});
        timer.restart();
        prepareRender(d, d.size(), true);
        QtConcurrent::blockingMap(view, [&](RenderArea &a) { renderArea(d, a); });
        const auto viewMs = timer.nsecsElapsed() / 1e6;
        // A brush dab crossing four tiles, with their backdrops kept from the stroke's start.
        QVector<RenderArea> dab(view.begin(), view.begin() + 2);
        dab += QVector<RenderArea>(view.begin() + 7, view.begin() + 9);
        QVector<QImage> backdrops;
        for (const auto &a : dab)
            backdrops << renderBackdrop(d, a, layerId);
        timer.restart();
        const int dabs = 20;
        for (int i = 0; i < dabs; ++i) {
            QVector<int> tiles(dab.size());
            std::iota(tiles.begin(), tiles.end(), 0);
            QtConcurrent::blockingMap(tiles,
                                      [&](int t) { renderOver(d, dab[t], layerId, backdrops[t]); });
        }
        const auto dabMs = timer.nsecsElapsed() / 1e6 / dabs;
        // Painting at 25%: the edited layer's halvings rebuilt whole, or carried over a dab.
        auto &edited = d.layers.first().image;
        auto dabAt = [&](int x) {
            const auto before = edited.cacheKey();
            QPainter p(&edited);
            p.fillRect(QRect(x, 2000, 120, 120), Qt::red);
            p.end();
            return before;
        };
        prepareRender(d, {1500, 1000}, true);
        dabAt(1000);
        timer.restart();
        prepareRender(d, {1500, 1000}, true);
        const auto rebuildMs = timer.nsecsElapsed() / 1e6;
        const auto previous = dabAt(1200);
        timer.restart();
        carryRenderCaches(d.layers.first(), false, previous, QRect(1200, 2000, 120, 120));
        prepareRender(d, {1500, 1000}, true);
        const auto carryMs = timer.nsecsElapsed() / 1e6;
        // A 6000 x 4000 layer with a drop shadow and a stroke: its effects rebuilt whole after a
        // dab, or carried over it.
        auto shaped = Document::create({6000, 4000});
        QImage shape(6000, 4000, QImage::Format_RGBA8888_Premultiplied);
        shape.fill(Qt::transparent);
        {
            QPainter p(&shape);
            p.setBrush(Qt::darkCyan);
            p.drawEllipse(QRect(500, 500, 5000, 3000));
        }
        shaped.addImage("Shape", shape);
        shaped.active()->metadata["effects"] =
            QJsonObject{{"shadow", QJsonObject{{"distance", 20}, {"blur", 10}}},
                        {"stroke", QJsonObject{{"size", 4}}}};
        auto &shapeImage = shaped.active()->image;
        const RenderArea tile{shaped.size(), QRect(1024, 1024, 256, 256)};
        auto shapeDab = [&] {
            const auto before = shapeImage.cacheKey();
            QPainter p(&shapeImage);
            p.fillRect(QRect(1100, 1100, 60, 60), Qt::red);
            p.end();
            return before;
        };
        renderArea(shaped, tile);
        shapeDab();
        timer.restart();
        renderArea(shaped, tile);
        const auto effectsRebuildMs = timer.nsecsElapsed() / 1e6;
        const auto shapeBefore = shapeDab();
        timer.restart();
        carryRenderCaches(*shaped.active(), false, shapeBefore, QRect(1100, 1100, 60, 60));
        renderArea(shaped, tile);
        const auto effectsCarryMs = timer.nsecsElapsed() / 1e6;
        qInfo().noquote() << QString("Dab on a 6000x4000 layer with effects: rebuilt %1 ms, "
                                     "carried %2 ms")
                                 .arg(effectsRebuildMs, 0, 'f', 1)
                                 .arg(effectsCarryMs, 0, 'f', 1);
        qInfo().noquote() << QString("Dab at 25%: halvings rebuilt %1 ms, carried %2 ms")
                                 .arg(rebuildMs, 0, 'f', 1)
                                 .arg(carryMs, 0, 'f', 1);
        qInfo().noquote() << QString("6000x4000, 20 layers: old 1600 px preview %1 ms per refresh; "
                                     "1440x900 view at 100% %2 ms (%3 tiles); "
                                     "brush dab over 4 tiles %4 ms")
                                 .arg(previewMs, 0, 'f', 1)
                                 .arg(viewMs, 0, 'f', 1)
                                 .arg(view.size())
                                 .arg(dabMs, 0, 'f', 1);
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

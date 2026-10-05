// SPDX-License-Identifier: MIT
#include "editable_layers.h"
#include "photoshop.h"
#include "photoshop_editable.h"
#include "text_fonts.h"
#include <QFontMetricsF>
#include "psd_descriptor_fixture.h"
#include "psd_fixture.h"
#include "render.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
using namespace compositor;
class PhotoshopTests : public QObject {
    Q_OBJECT
  private slots:
    void vectorPathOperationsAreBounded() {
        PsdFixtureWriter w;
        w.u32(3);
        w.u32(0);
        for (int i = 0; i < 1001; ++i) {
            w.u16(0);  // Closed subpath of one knot,
            w.u16(1);
            w.u16(1);  // united with the path so far.
            w.u16(1);
            w.bytes(QByteArray(18, 0));
            w.u16(2);
            for (int k = 0; k < 3; ++k) {
                w.u32(quint32(qint32(.5 * 16777216)));
                w.u32(quint32(qint32(.5 * 16777216)));
            }
        }
        const QByteArray bytes = w.data;
        auto editable = photoshopEditable({{"vmsk", QByteArrayView(bytes)}}, {8, 8});
        QVERIFY(!editable.vectorEnabled);
        QVERIFY(editable.notes.join(' ').contains("too many shape operations"));
    }
    void vectorMasksHolesInversionAndDisabled() {
        for (bool psb : {false, true})
            for (int flags : {0, 1, 4}) {
                PsdFixtureLayer l;
                l.bounds = {0, 0, 8, 8};
                l.planes = {{-1, QByteArray(64, char(255))},
                            {0, QByteArray(64, char(255))},
                            {1, QByteArray(64, 0)},
                            {2, QByteArray(64, 0)}};
                l.extra["vmsk"] = vectorMaskFixture(true, flags);
                auto result = readPhotoshop(psdFixture({l}, psb));
                auto image = renderDocument(result.document);
                QCOMPARE(image.pixelColor(2, 2).alpha(), flags == 1 ? 0 : 255);
                QCOMPARE(image.pixelColor(4, 4).alpha(), flags == 4 ? 255 : flags == 1 ? 255 : 0);
                QTemporaryDir dir;
                auto path = dir.filePath("Vector.comp");
                saveProject(result.document, path);
                QCOMPARE(renderDocument(loadProject(path)), image);
            }
    }
    void vectorFillWithoutRasterChannels() {
        PsdFixtureLayer l;
        l.bounds = {};
        l.planes.clear();
        l.extra["vmsk"] = vectorMaskFixture();
        l.extra["SoCo"] =
            psdDescriptor("null", {{"Clr ", psdObject("RGBC", {{"Rd  ", psdDouble(10)},
                                                               {"Grn ", psdDouble(100)},
                                                               {"Bl  ", psdDouble(240)}})}});
        auto result = readPhotoshop(psdFixture({l}));
        auto image = renderDocument(result.document);
        QVERIFY(image.pixelColor(4, 4).blue() > 200);
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        l.extra["vmsk"] = vectorMaskFixture(false, 1);
        image = renderDocument(readPhotoshop(psdFixture({l})).document);
        QCOMPARE(image.pixelColor(4, 4).alpha(), 0);
        QVERIFY(image.pixelColor(0, 0).blue() > 200);
    }
    void vectorMaskDoesNotEnableDisabledPixelMask() {
        PsdFixtureLayer l;
        l.hasMask = true;
        l.maskFlags = 2;
        l.planes.emplace_back(-2, QByteArray(4, char(0)));
        l.extra["vmsk"] = vectorMaskFixture();
        auto imported = readPhotoshop(psdFixture({l}, false, {2, 2}));
        QVERIFY(renderDocument(imported.document).pixelColor(1, 1).alpha() > 0);
    }
    void effectsDescriptorsAndLegacyOverlay() {
        auto color = psdObject(
            "RGBC", {{"Rd  ", psdDouble(0)}, {"Grn ", psdDouble(0)}, {"Bl  ", psdDouble(255)}});
        PsdFixtureWriter effects;
        effects.u32(0);
        effects.bytes(psdDescriptor("Lefx", {{"masterFXSwitch", psdBool(true)},
                                             {"Scl ", psdUnit(100)},
                                             {"SoFi", psdObject("SoFi", {{"enab", psdBool(true)},
                                                                         {"Opct", psdUnit(100)},
                                                                         {"Clr ", color}})},
                                             {"DrSh", psdObject("DrSh", {{"enab", psdBool(false)},
                                                                         {"Opct", psdUnit(50)},
                                                                         {"Clr ", color},
                                                                         {"blur", psdUnit(4)},
                                                                         {"Dstn", psdUnit(3)}})}}));
        PsdFixtureLayer l;
        l.extra["lfx2"] = effects.data;
        auto result = readPhotoshop(psdFixture({l}));
        auto native = result.document.layers[0].metadata.value("effects").toObject();
        QCOMPARE(native.value("colorOverlay").toObject().value("blue").toDouble(), 1.0);
        QVERIFY(!native.value("shadow").toObject().value("enabled").toBool());
        QCOMPARE(renderDocument(result.document).pixelColor(0, 0), QColor(Qt::blue));
        PsdFixtureWriter legacy, overlay;
        overlay.u32(2);
        overlay.bytes("norm");
        overlay.u16(0);
        overlay.u16(0);
        overlay.u16(65535);
        overlay.u16(0);
        overlay.u16(0);
        overlay.u8(255);
        overlay.u8(1);
        overlay.bytes(QByteArray(10, 0));
        legacy.u16(0);
        legacy.u16(1);
        legacy.bytes("8BIMsofi");
        legacy.u32(overlay.data.size());
        legacy.bytes(overlay.data);
        l.extra.clear();
        l.extra["lrFX"] = legacy.data;
        QCOMPARE(renderDocument(readPhotoshop(psdFixture({l})).document).pixelColor(0, 0),
                 QColor(Qt::green));
    }
    void invalidVectorsAndEffectsKeepPixelsWithReport() {
        PsdFixtureLayer l;
        l.extra["vmsk"] = vectorMaskFixture().left(20);
        l.extra["lfx2"] = QByteArray(8, 0);
        auto result = readPhotoshop(psdFixture({l}));
        QVERIFY(!result.conversions.isEmpty());
        QCOMPARE(result.document.layers[0].image.pixelColor(0, 0), QColor(Qt::red));
    }
    void allNativeEffectsAndMultiDescriptors() {
        const auto color = psdObject("Grsc", {{"Gry ", psdDouble(25)}});
        QList<std::pair<QByteArray, QByteArray>> items;
        for (auto name : {"DrSh", "IrSh", "OrGl", "IrGl", "SoFi", "FrFX"})
            items.append({name, psdObject(name, {{"enab", psdBool(true)},
                                                 {"Clr ", color},
                                                 {"Opct", psdUnit(75)},
                                                 {"blur", psdUnit(2)},
                                                 {"Sz  ", psdUnit(1)},
                                                 {"Styl", psdEnum("Styl", "InsF")}})});
        PsdFixtureWriter effects;
        effects.u32(0);
        effects.bytes(psdDescriptor("Lefx", items));
        PsdFixtureLayer layer;
        layer.extra["lfx2"] = effects.data;
        auto imported = readPhotoshop(psdFixture({layer}, true));
        auto native = imported.document.layers[0].metadata.value("effects").toObject();
        QCOMPARE(native.size(), 6);
        QCOMPARE(native.value("stroke").toObject().value("inside").toBool(), true);
        QCOMPARE(native.value("colorOverlay").toObject().value("red").toDouble(),
                 QColor::fromRgbF(.25, .25, .25).redF());
        QVERIFY(!renderDocument(imported.document).isNull());
        PsdFixtureWriter list;
        list.bytes("VlLs");
        list.u32(2);
        list.bytes(items.first().second);
        list.bytes(items.first().second);
        effects.data.clear();
        effects.u32(0);
        effects.bytes(psdDescriptor("Lefx", {{"dropShadowMulti", list.data}}));
        layer.extra["lfx2"] = effects.data;
        imported = readPhotoshop(psdFixture({layer}));
        QVERIFY(
            imported.document.layers[0].metadata.value("effects").toObject().contains("shadow"));
        QVERIFY(imported.conversions.join('\n').contains("Multiple"));
    }
    void editableTypeAndParagraph() {
        PsdFixtureLayer l;
        l.extra["TySh"] = typeFixture();
        auto result = readPhotoshop(psdFixture({l}, false, {256, 256}));
        auto &layer = result.document.layers[0];
        auto text = layer.metadata.value("text").toObject();
        QCOMPARE(text.value("content").toString(), QString("测试 Hello"));
        QCOMPARE(text.value("fontSize").toDouble(), 18.0);
        QCOMPARE(text.value("fontName").toString(), QString("Segoe UI"));
        QCOMPARE(text.value("alignment").toString(), QString("Center"));
        QCOMPARE(text.value("leading").toDouble(), 22.0);
        QVERIFY(std::abs(text.value("tracking").toDouble() - 1.8) < 1e-6);
        QVERIFY(std::abs(layer.transform().value("rotation").toDouble() -
                         .4 * 180 / 3.141592653589793) < 1e-6);
        QVERIFY(layer.image.width() > 2);
        QVERIFY(!result.conversions.isEmpty());
        l.extra["TySh"] = typeFixture(false, 0, true);
        result = readPhotoshop(psdFixture({l}, false, {256, 256}));
        auto box =
            result.document.layers[0].metadata.value("text").toObject().value("boxSize").toArray();
        QCOMPARE(box[0].toDouble(), 124.0);
        QCOMPARE(box[1].toDouble(), 104.0);
        QTemporaryDir dir;
        auto path = dir.filePath("Text.comp");
        saveProject(result.document, path);
        QCOMPARE(loadProject(path).manifest(), result.document.manifest());
    }
    void textRunsKeepTheirFontsAndColors() {
        PsdFixtureLayer l;
        l.extra["TySh"] = styledTypeFixture();
        auto result = readPhotoshop(psdFixture({l}, false, {256, 256}));
        const auto text = result.document.layers[0].metadata.value("text").toObject();
        QCOMPARE(text.value("fontName").toString(), QString("SegoeUI"));
        const auto fonts = text.value("fontRuns").toArray();
        QCOMPARE(fonts.size(), 1);
        QCOMPARE(fonts[0].toObject().value("location").toInt(), 6);
        QCOMPARE(fonts[0].toObject().value("length").toInt(), 5); // Photoshop's closing return is not text.
        QCOMPARE(fonts[0].toObject().value("fontName").toString(), QString("Arial-BoldMT"));
        const auto colors = text.value("colorRuns").toArray();
        QCOMPARE(colors.size(), 1);
        QCOMPARE(colors[0].toObject().value("red").toDouble(), 1.0);
        QVERIFY(!result.conversions.join("\n").contains("Only the first text style"));
        // The PostScript names resolve to the installed families: no substitution reported.
        if (QFontDatabase::families().contains("Segoe UI") && QFontDatabase::families().contains("Arial"))
            QVERIFY(!result.conversions.join("\n").contains("not installed"));
    }
    void paragraphTextsFirstBaselineSitsWherePhotoshopPutsIt() {
        PsdFixtureLayer l;
        l.extra["TySh"] = styledTypeFixture();
        auto result = readPhotoshop(psdFixture({l}, false, {256, 256}));
        const auto &layer = result.document.layers[0];
        const auto text = layer.metadata.value("text").toObject();
        QFont font(resolvedTextFont(text.value("fontName").toString()));
        font.setPixelSize(20);
        // The frame's top is at y = 20, so its first baseline is one ascent below that, although
        // 40-pixel lines would otherwise put it lower.
        const double origin = layer.transform().value("origin").toArray()[1].toDouble();
        QVERIFY(std::abs(origin + textFirstBaseline(text) - (20 + QFontMetricsF(font).ascent())) < .5);
        QVERIFY(textFirstBaseline(text) > 12 + QFontMetricsF(font).ascent() + 5);
    }
    void postScriptFontNamesResolveToInstalledFamilies() {
        const auto families = QFontDatabase::families();
        if (!families.contains("Arial") || !families.contains("Times New Roman") ||
            !families.contains("Segoe UI"))
            QSKIP("Needs the Windows fonts (run with the native platform plugin)");
        QCOMPARE(resolvedTextFont("Arial-BoldMT"), QString("Arial"));
        QCOMPARE(resolvedTextFont("Arial-ItalicMT"), QString("Arial"));
        QCOMPARE(resolvedTextFont("TimesNewRomanPSMT"), QString("Times New Roman"));
        QCOMPARE(resolvedTextFont("SegoeUI"), QString("Segoe UI"));
        QCOMPARE(resolvedTextFont("segoe-ui"), QString("Segoe UI"));
        QVERIFY(installedTextFont("NoSuchFontAnywhereMT").isEmpty());
        QCOMPARE(resolvedTextFont("NoSuchFontAnywhereMT"), QString("Segoe UI"));
    }
    void vectorFillLayerKeepsItsPixelMaskInPlace() {
        // No raster channels, a pixel mask hiding column 1 of the canvas, and a vector fill over
        // columns 1-6: the drawn path gets its own grid, the mask stays on the canvas's.
        PsdFixtureLayer l;
        l.bounds = {};
        l.planes.clear();
        l.hasMask = true;
        l.maskBounds = {0, 0, 8, 8};
        QByteArray mask(64, char(255));
        for (int y = 0; y < 8; ++y)
            mask[y * 8 + 1] = 0;
        l.planes.emplace_back(-2, mask);
        l.extra["vmsk"] = vectorMaskFixture();
        l.extra["SoCo"] =
            psdDescriptor("null", {{"Clr ", psdObject("RGBC", {{"Rd  ", psdDouble(10)},
                                                               {"Grn ", psdDouble(100)},
                                                               {"Bl  ", psdDouble(240)}})}});
        auto image = renderDocument(readPhotoshop(psdFixture({l})).document);
        QCOMPARE(image.pixelColor(1, 4).alpha(), 0);   // The masked column.
        QVERIFY(image.pixelColor(2, 4).blue() > 200);  // Filled and not masked.
        QVERIFY(image.pixelColor(6, 4).blue() > 200);
        QCOMPARE(image.pixelColor(4, 0).alpha(), 0);   // Outside the path.
    }
    void tooComplexVectorPathsAreReportedNotHung() {
        PsdFixtureWriter w;
        w.u32(3);
        w.u32(0);
        for (int i = 0; i < 2100; ++i) {
            w.u16(0);
            w.u16(1);
            w.u16(1);
            w.u16(1);
            w.bytes(QByteArray(18, 0));
            w.u16(2);
            for (int k = 0; k < 6; ++k)
                w.u32(quint32(k * 1000));
        }
        PsdFixtureLayer l;
        l.bounds = {0, 0, 8, 8};
        l.planes = {{-1, QByteArray(64, char(255))},
                    {0, QByteArray(64, char(255))},
                    {1, QByteArray(64, 0)},
                    {2, QByteArray(64, 0)}};
        l.extra["vmsk"] = w.data;
        QElapsedTimer clock;
        clock.start();
        auto result = readPhotoshop(psdFixture({l}));
        QVERIFY(clock.elapsed() < 10000);
        QVERIFY(result.conversions.join("\n").contains("too many shape operations"));
        QCOMPARE(renderDocument(result.document).pixelColor(4, 4).alpha(), 255);
    }
    void unsupportedTypeRasterFallback() {
        for (auto payload : {typeFixture(true), typeFixture(false, .5), QByteArray(10, 0)}) {
            PsdFixtureLayer l;
            l.extra["TySh"] = payload;
            auto result = readPhotoshop(psdFixture({l}));
            QVERIFY(!result.document.layers[0].metadata.contains("text"));
            QCOMPARE(result.document.layers[0].image.pixelColor(0, 0), QColor(Qt::red));
            QVERIFY(!result.conversions.isEmpty());
        }
    }
    void editableLiveShapes() {
        for (int kind : {1, 2, 5}) {
            PsdFixtureLayer l;
            l.bounds = {};
            l.planes.clear();
            l.extra = liveShapeFixture(kind);
            auto result = readPhotoshop(psdFixture({l}, false, {128, 128}));
            auto &layer = result.document.layers[0];
            auto shape = layer.metadata.value("shape").toObject();
            QCOMPARE(shape.value("kind").toString(), QString(kind == 5 ? "Ellipse" : "Rectangle"));
            QCOMPARE(shape.value("cornerRadius").toDouble(), 5.0);
            QCOMPARE(layer.image.size(), QSize(60, 40));
            QCOMPARE(layer.transform().value("origin").toArray()[0].toDouble(), 20.0);
            QVERIFY(layer.image.pixelColor(30, 20).blue() > layer.image.pixelColor(30, 20).red());
        }
    }
    void unlinkedMaskHasIndependentPlacement() {
        PsdFixtureLayer l;
        l.hasMask = true;
        l.maskFlags = 1;
        l.planes.emplace_back(-2, QByteArray(4, char(255)));
        auto result = readPhotoshop(psdFixture({l}));
        QCOMPARE(result.document.layers[0].metadata.value("maskPlacement"),
                 result.document.layers[0].metadata.value("transform"));
    }
    void rawAndRleVersions_data() {
        QTest::addColumn<bool>("psb");
        QTest::addColumn<int>("compression");
        QTest::newRow("PSD Raw") << false << 0;
        QTest::newRow("PSD RLE") << false << 1;
        QTest::newRow("PSB Raw") << true << 0;
        QTest::newRow("PSB RLE") << true << 1;
    }
    void rawAndRleVersions() {
        QFETCH(bool, psb);
        QFETCH(int, compression);
        PsdFixtureLayer red, blue;
        red.name = "测试图层";
        red.opacity = 128;
        red.blend = "mul ";
        red.compression = compression;
        blue.name = "Blue";
        blue.hidden = true;
        blue.bounds.moveTo(3, 1);
        auto result = readPhotoshop(psdFixture({red, blue}, psb));
        auto &d = result.document;
        QCOMPARE(d.size(), QSize(8, 8));
        QCOMPARE(d.layers.size(), 2);
        QCOMPARE(d.layers[0].name(), red.name);
        QCOMPARE(d.layers[0].blend(), QString("Multiply"));
        QCOMPARE(d.layers[0].opacity(), 128 / 255.0);
        QVERIFY(!d.layers[1].visible());
        QCOMPARE(d.layers[0].image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(d.metadata.value("resolution").toDouble(), 144.0);
        QVERIFY(result.conversions.isEmpty());
    }
    void nestedFoldersMasksAndClipping() {
        PsdFixtureLayer divider, base, clip, folder;
        divider.bounds = {};
        divider.planes.clear();
        divider.section = 3;
        base.name = "Base";
        base.bounds.moveTo(2, 1);
        base.hasMask = true;
        base.maskBounds = {3, 1, 1, 1};
        base.planes.emplace_back(-2, QByteArray(1, char(0)));
        clip.name = "Clipped";
        clip.clipped = true;
        folder.bounds = {};
        folder.planes.clear();
        folder.section = 1;
        folder.name = "Folder";
        folder.blend = "pass";
        auto result = readPhotoshop(psdFixture({divider, base, clip, folder}));
        auto &d = result.document;
        QCOMPARE(d.layers.size(), 3);
        QVERIFY(d.layers[2].group());
        QCOMPARE(d.layers[0].parent(), d.layers[2].id());
        QCOMPARE(d.layers[1].parent(), d.layers[2].id());
        QCOMPARE(d.layers[1].metadata.value("maskSourceID").toString(), d.layers[0].id());
        QCOMPARE(d.layers[0].mask.constScanLine(0)[0], uchar(255));
        QCOMPARE(d.layers[0].mask.constScanLine(0)[1], uchar(0));
        QCOMPARE(d.layers[0].mask.constScanLine(1)[1], uchar(255));
        QVERIFY(result.conversions.isEmpty());
    }
    void adjustmentAndConversionReport() {
        PsdFixtureLayer base, levels, smart;
        levels.bounds = {};
        levels.planes.clear();
        levels.name = "Levels";
        PsdFixtureWriter data;
        data.u16(2);
        for (int i = 0; i < 4; ++i) {
            data.u16(i == 0 ? 20 : 0);
            data.u16(255);
            data.u16(0);
            data.u16(255);
            data.u16(100);
        }
        data.bytes(QByteArray(250, 0));
        levels.extra["levl"] = data.data;
        smart.name = "Smart";
        smart.extra["SoLd"] = QByteArray(4, 0);
        smart.extra["lfx2"] = QByteArray(4, 0);
        auto result = readPhotoshop(psdFixture({base, levels, smart}));
        QCOMPARE(result.document.layers.size(), 3);
        auto a = result.document.layers[1].metadata.value("adjustment").toObject();
        QCOMPARE(a.value("kind").toString(), QString("Levels"));
        QCOMPARE(a.value("levels")
                     .toObject()
                     .value("ranges")
                     .toArray()[0]
                     .toObject()
                     .value("black")
                     .toInt(),
                 20);
        QCOMPARE(result.conversions.size(), 4);
    }
    void unusedChannelsSkipped() {
        PsdFixtureLayer l;
        l.planes.emplace_back(5, QByteArray::fromHex("00630000"));
        auto result = readPhotoshop(psdFixture({l}));
        QCOMPARE(result.document.layers[0].image.pixelColor(0, 0), QColor(Qt::red));
    }
    void rejectsUnsupportedCompression() {
        PsdFixtureLayer l;
        l.compression = 99;
        QVERIFY_EXCEPTION_THROWN(readPhotoshop(psdFixture({l})), Error);
    }
    void rejectsAllTruncatedPrefixes() {
        auto data = psdFixture({PsdFixtureLayer{}});
        for (int length = 0; length < data.size() - 8 * 8 * 3 - 2; ++length)
            QVERIFY_EXCEPTION_THROWN(readPhotoshop(QByteArrayView(data).first(length)), Error);
    }
    void cropToBudget() {
        PsdFixtureLayer l;
        l.bounds = {-1, 0, 2, 2};
        auto result = readPhotoshop(psdFixture({l}, false, {1, 2}), 2);
        QCOMPARE(result.document.layers[0].image.size(), QSize(1, 2));
        QCOMPARE(result.document.layers[0].image.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(!result.conversions.isEmpty());
        QVERIFY_EXCEPTION_THROWN(readPhotoshop(psdFixture({PsdFixtureLayer{}}, false, {2, 2}), 3),
                                 Error);
    }
    void flattenedFallback() {
        auto result = readPhotoshop(psdFixture({}));
        QCOMPARE(result.document.layers.size(), 1);
        QCOMPARE(result.document.layers[0].image.pixelColor(0, 0), QColor(25, 25, 25));
    }
    void importedProjectSaveRoundTrip() {
        QTemporaryDir dir;
        auto path = dir.filePath("sample.psb");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(psdFixture({PsdFixtureLayer{}}, true));
        f.close();
        QVERIFY(isPhotoshopFile(path));
        auto result = importPhotoshop(path);
        auto project = dir.filePath("Imported.comp");
        saveProject(result.document, project);
        auto loaded = loadProject(project);
        QCOMPARE(loaded.manifest(), result.document.manifest());
        QCOMPARE(loaded.active()->image, result.document.active()->image);
    }
};
QTEST_MAIN(PhotoshopTests)
#include "photoshop_tests.moc"

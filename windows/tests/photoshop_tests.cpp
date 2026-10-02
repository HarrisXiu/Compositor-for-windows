// SPDX-License-Identifier: MIT
#include "photoshop.h"
#include "psd_descriptor_fixture.h"
#include "psd_fixture.h"
#include "render.h"
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
using namespace compositor;
class PhotoshopTests : public QObject {
    Q_OBJECT
  private slots:
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
        QCOMPARE(result.conversions.size(), 3);
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

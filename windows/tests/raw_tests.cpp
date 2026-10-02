// SPDX-License-Identifier: MIT
#include "raw_fixture.h"
#include "raw_import.h"
#include <QColorSpace>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>
using namespace compositor;
class RawTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        auto dir = qEnvironmentVariable("COMPOSITOR_TEST_ARTIFACT_DIR");
        if (!dir.isEmpty()) {
            QFile file(QDir(dir).filePath("synthetic-bayer.dng"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(dngFixture()), qint64(dngFixture().size()));
        }
    }
    void developmentControlsAndReset() {
        QTemporaryDir dir;
        auto path = dir.filePath("原始照片.dng");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(dngFixture());
        f.close();
        auto source = RawSource::open(path);
        QCOMPARE(source->size(), QSize(48, 32));
        QVERIFY(source->camera().contains("Compositor"));
        auto s = source->asShot();
        auto base = source->develop(s);
        QCOMPARE(base.size(), source->size());
        QCOMPARE(base.colorSpace(), QColorSpace(QColorSpace::SRgb));
        auto p = base.pixelColor(24, 16);
        QVERIFY(p.red() > 0 && p.green() > 0 && p.blue() > 0);
        s.exposure = -2;
        auto dark = source->develop(s);
        QVERIFY(dark.pixelColor(24, 16).lightness() < p.lightness());
        s.exposure = 2;
        auto light = source->develop(s);
        QVERIFY(light.pixelColor(24, 16).lightness() > p.lightness());
        s.reset();
        s.temperature = 9000;
        auto warm = source->develop(s);
        s.temperature = 2500;
        auto cool = source->develop(s);
        QVERIFY(warm != cool);
        QVERIFY(warm.pixelColor(24, 16).redF() / warm.pixelColor(24, 16).blueF() >
                cool.pixelColor(24, 16).redF() / cool.pixelColor(24, 16).blueF());
        s.reset();
        s.tint = 80;
        auto tint = source->develop(s);
        QVERIFY(tint != base);
        s.reset();
        s.boost = 0;
        QVERIFY(source->develop(s) != base);
        auto preview = source->develop(s, 16);
        QVERIFY(std::max(preview.width(), preview.height()) <= 16);
        s.reset();
        QCOMPARE(source->develop(s), base);
        auto d = Document::create(source->size());
        d.addImage("RAW", base);
        auto project = dir.filePath("RAW.comp");
        saveProject(d, project);
        QCOMPARE(loadProject(project).active()->image.convertToFormat(base.format()), base);
    }
    void cameraOrientation() {
        QTemporaryDir dir;
        auto path = dir.filePath("Rotated.dng");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(dngFixture(6));
        f.close();
        auto source = RawSource::open(path);
        QCOMPARE(source->size(), QSize(32, 48));
        QCOMPARE(source->develop(source->asShot()).size(), QSize(32, 48));
    }
    void invalidInputAndSettings() {
        QTemporaryDir dir;
        auto path = dir.filePath("Bad.dng");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("invalid");
        f.close();
        QVERIFY_EXCEPTION_THROWN(RawSource::open(path), Error);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(dngFixture());
        f.close();
        auto source = RawSource::open(path);
        auto s = source->asShot();
        s.exposure = std::numeric_limits<double>::quiet_NaN();
        QVERIFY_EXCEPTION_THROWN(source->develop(s), Error);
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QVERIFY_EXCEPTION_THROWN(RawSource::open(path, cancel), Error);
    }
    void detection() {
        QVERIFY(isRawFile("Camera.NEF"));
        QVERIFY(isRawFile("Camera.CR3"));
        QVERIFY(!isRawFile("Image.tif"));
        QVERIFY(rawFilePatterns().contains("*.dng"));
    }
};
QTEST_GUILESS_MAIN(RawTests)
#include "raw_tests.moc"

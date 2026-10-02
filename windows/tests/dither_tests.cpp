// SPDX-License-Identifier: MIT
#include "dither.h"
#include "document.h"
#include "filters.h"
#include <QGuiApplication>
#include <QtTest>
using namespace compositor;
class DitherTests : public QObject {
    Q_OBJECT
  private slots:
    void allStyles_data() {
        QTest::addColumn<QString>("style");
        for (auto &s : ditherStyles())
            QTest::newRow(s.toUtf8().constData()) << s;
    }
    void allStyles() {
        QFETCH(QString, style);
        QImage image(67, 65, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor(x * 3, y * 3, 128, x % 3 == 0 ? 0 : 128));
        QJsonObject settings{{"style", style}, {"pixelSize", 1}, {"glow", 0}};
        auto out = applyFilter(image, "Dither", settings);
        QCOMPARE(out.size(), image.size());
        QCOMPARE(applyFilter(image, "Dither", settings), out);
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x) {
                auto p = out.constScanLine(y) + x * 4;
                QCOMPARE(p[3], image.constScanLine(y)[x * 4 + 3]);
                QVERIFY(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3]);
            }
    }
    void orderedThreshold() {
        QImage image(2, 2, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(128, 128, 128));
        auto out = applyDither(image, {{"style", "Bayer 2 × 2"}, {"pixelSize", 1}});
        QCOMPARE(out.pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(out.pixelColor(1, 0), QColor(Qt::white));
        QCOMPARE(out.pixelColor(0, 1), QColor(Qt::white));
        QCOMPARE(out.pixelColor(1, 1), QColor(Qt::black));
    }
    void scanlineBeamAndGlow() {
        auto render = [](int gray, double glow = 0) {
            QImage image(16, 32, QImage::Format_RGBA8888_Premultiplied);
            image.fill(QColor(gray, gray, gray));
            return applyDither(image,
                               {{"style", "Scanlines (CRT)"}, {"lineSpacing", 8}, {"glow", glow}});
        };
        auto white = render(255), gray = render(89), black = render(0), glow = render(255, 100);
        int w = 0, g = 0;
        for (int y = 0; y < 32; ++y) {
            int v = white.pixelColor(5, y).red();
            w += v;
            g += gray.pixelColor(5, y).red();
            QCOMPARE(black.pixelColor(5, y).red(), 0);
            if (y % 8 == 3 || y % 8 == 4)
                QCOMPARE(v, 255);
            if (y % 8 == 0)
                QVERIFY(v < 40);
        }
        QVERIFY(g * 2 < w);
        QVERIFY(glow.pixelColor(5, 0).red() > white.pixelColor(5, 0).red() + 40);
    }
    void beadsAndWobble() {
        QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        auto dots = applyDither(
            image, {{"style", "Scanlines (CRT)"}, {"lineSpacing", 8}, {"dots", 100}, {"glow", 0}});
        QCOMPARE(dots.pixelColor(3, 4).red(), 255);
        QCOMPARE(dots.pixelColor(4, 4).red(), 255);
        QVERIFY(dots.pixelColor(0, 4).red() < 60);
        QVERIFY(dots.pixelColor(8, 4).red() < 60);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x)
                image.setPixelColor(x, y, Qt::black);
        auto edges = [&](double wobble) {
            auto out = applyDither(image, {{"style", "Scanlines (CRT)"},
                                           {"lineSpacing", 8},
                                           {"wobble", wobble},
                                           {"glow", 0}});
            QSet<int> positions;
            for (int line = 0; line < 8; ++line)
                for (int x = 0; x < 64; ++x)
                    if (out.pixelColor(x, line * 8 + 4).red() > 128) {
                        positions.insert(x);
                        break;
                    }
            return positions;
        };
        QCOMPARE(edges(0), QSet<int>{32});
        QVERIFY(edges(12).size() >= 3);
    }
    void paletteAndChunkyEdges() {
        QImage image(7, 5, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        auto out =
            applyDither(image, {{"pixelSize", 3},
                                {"colors", "Two Colors"},
                                {"light", QJsonObject{{"red", 1}, {"green", 0}, {"blue", 0}}}});
        QCOMPARE(out.size(), image.size());
        QCOMPARE(out.pixelColor(6, 4), QColor(Qt::red));
        auto dots = applyDither(image, {{"pixelSize", 4}, {"pixelShape", "Dot"}});
        QVERIFY(dots.pixelColor(0, 0).red() < dots.pixelColor(1, 1).red());
        QVERIFY_EXCEPTION_THROWN(applyDither(image, {{"style", "Invalid"}}), Error);
    }
};
QTEST_MAIN(DitherTests)
#include "dither_tests.moc"

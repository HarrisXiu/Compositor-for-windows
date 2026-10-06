// SPDX-License-Identifier: MIT
#include "distort.h"
#include "editor.h"
#include "language.h"
#include "render.h"
#include <QApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cmath>
using namespace compositor;
namespace {
QRectF boxOf(const Layer &l) {
    const auto t = l.transform();
    const auto o = t.value("origin").toArray(), s = t.value("size").toArray();
    return {o.at(0).toDouble(), o.at(1).toDouble(), s.at(0).toDouble(), s.at(1).toDouble()};
}
// Left half red, right half blue.
QImage halves(QSize size) {
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::blue);
    QPainter(&image).fillRect(0, 0, size.width() / 2, size.height(), Qt::red);
    return image;
}
double meanDifference(const QImage &a, const QImage &b) {
    double total = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const auto p = a.pixelColor(x, y), q = b.pixelColor(x, y);
            total += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) +
                     std::abs(p.blue() - q.blue()) + std::abs(p.alpha() - q.alpha());
        }
    return total / (double(a.width()) * a.height() * 4);
}
// Two 16-pixel layers, A red at the left and B blue to its right, on a 128-pixel canvas.
Document twoLayers(QString *a = nullptr, QString *b = nullptr, bool inFolder = false) {
    auto d = Document::create({128, 128});
    QImage red(16, 16, QImage::Format_RGBA8888_Premultiplied),
        blue(16, 16, QImage::Format_RGBA8888_Premultiplied);
    red.fill(Qt::red);
    blue.fill(Qt::blue);
    const auto first = d.addImage("A", red), second = d.addImage("B", blue);
    d.find(first)->setBounds(QRectF(0, 0, 16, 16));
    d.find(second)->setBounds(QRectF(48, 0, 16, 16));
    if (inFolder) {
        const auto folder = d.addGroup("Folder");
        d.find(first)->metadata["parentID"] = folder;
        d.find(second)->metadata["parentID"] = folder;
        d.metadata["activeLayerID"] = folder;
    }
    if (a)
        *a = first;
    if (b)
        *b = second;
    return d;
}
void ready(EditorPage &p) {
    p.resize(320, 320);
    p.show();
    QTest::qWait(20);
    p.canvas->zoomTo(1);
    p.canvas->waitForRendering();
    p.session.view.snap = false;
}
QPoint at(EditorPage &p, double x, double y) {
    return QPoint(
        qRound((p.canvas->width() - p.document.size().width() * p.canvas->zoom) / 2 +
               x * p.canvas->zoom),
        qRound((p.canvas->height() - p.document.size().height() * p.canvas->zoom) / 2 +
               y * p.canvas->zoom));
}
void mouse(Canvas *c, QEvent::Type type, QPoint position, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent e(type, QPointF(position), QPointF(c->mapToGlobal(position)),
                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(c, &e);
}
void drag(EditorPage &p, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}) {
    mouse(p.canvas, QEvent::MouseButtonPress, at(p, from.x(), from.y()), mods);
    mouse(p.canvas, QEvent::MouseMove, at(p, to.x(), to.y()), mods);
    mouse(p.canvas, QEvent::MouseButtonRelease, at(p, to.x(), to.y()), mods);
}
} // namespace
class TransformTests : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        UiLanguage::instance().setLanguage("en", false);
    }
    void cornersAndBoxes() {
        QJsonObject box{{"origin", QJsonArray{10, 20}},
                        {"size", QJsonArray{40, 10}},
                        {"rotation", 90}};
        const auto corners = transformCorners(box);
        // Rotated a quarter turn clockwise about (30, 25): the top-left goes to the top-right.
        QVERIFY(QLineF(corners[0], QPointF(35, 5)).length() < 1e-9);
        QVERIFY(QLineF(corners[2], QPointF(25, 45)).length() < 1e-9);
        const auto upright = uprightBox({box, QJsonObject{{"origin", QJsonArray{0, 0}},
                                                          {"size", QJsonArray{4, 4}},
                                                          {"rotation", 0}}});
        QCOMPARE(upright["origin"].toArray()[0].toDouble(), 0.0);
        QCOMPARE(upright["origin"].toArray()[1].toDouble(), 0.0);
        QCOMPARE(upright["size"].toArray()[0].toDouble(), 35.0);
        QCOMPARE(upright["size"].toArray()[1].toDouble(), 45.0);
        QVERIFY(uprightBox({}).isEmpty());
        QVERIFY(distortUsable({{0, 0}, {10, 0}, {10, 10}, {0, 10}}));
        QVERIFY(!distortUsable({{0, 0}, {10, 0}, {20, 0}, {30, 0}}));
        QVERIFY(distortConvex({{0, 0}, {10, 0}, {10, 10}, {0, 10}}));
        QVERIFY(!distortConvex({{0, 0}, {40, 0}, {10, 10}, {0, 40}}));
        QVERIFY(distortUsable({{0, 0}, {40, 0}, {10, 10}, {0, 40}}));
    }
    void followingCarriesMembersWithTheBox() {
        const QJsonObject box{{"origin", QJsonArray{0, 0}}, {"size", QJsonArray{64, 16}},
                              {"rotation", 0}};
        const QJsonObject member{{"origin", QJsonArray{48, 0}}, {"size", QJsonArray{16, 16}},
                                 {"rotation", 0}, {"flipX", false}, {"flipY", false},
                                 {"sampling", "High quality"}};
        // The box doubled: the member doubles and keeps its place within it.
        const QJsonObject doubled{{"origin", QJsonArray{0, 0}}, {"size", QJsonArray{128, 32}},
                                  {"rotation", 0}};
        auto moved = followedTransform(member, box, doubled);
        QCOMPARE(moved["origin"].toArray()[0].toDouble(), 96.0);
        QCOMPARE(moved["size"].toArray()[0].toDouble(), 32.0);
        QCOMPARE(moved["size"].toArray()[1].toDouble(), 32.0);
        QCOMPARE(moved["sampling"].toString(), QString("High quality"));
        // A plain move carries exactly.
        const QJsonObject shifted{{"origin", QJsonArray{5, 7}}, {"size", QJsonArray{64, 16}},
                                  {"rotation", 0}};
        moved = followedTransform(member, box, shifted);
        QCOMPARE(moved["origin"].toArray()[0].toDouble(), 53.0);
        QCOMPARE(moved["origin"].toArray()[1].toDouble(), 7.0);
        QCOMPARE(moved["size"], member["size"]);
        // A quarter turn about the box's center takes the member around with it.
        QJsonObject turned = box;
        turned["rotation"] = 90;
        moved = followedTransform(member, box, turned);
        QCOMPARE(std::fmod(moved["rotation"].toDouble() + 360, 360.0), 90.0);
        const auto corners = transformCorners(moved);
        const QPointF center = (corners[0] + corners[2]) / 2;
        QVERIFY(QLineF(center, QPointF(32, 32)).length() < 1e-6);
    }
    void identityDistortionKeepsThePicture() {
        auto d = Document::create({80, 80});
        const auto id = d.addImage("Halves", halves({20, 10}));
        auto *layer = d.find(id);
        layer->setBounds(QRectF(20, 25, 40, 20));
        auto t = layer->transform();
        t["rotation"] = 30;
        t["flipX"] = true;
        layer->metadata["transform"] = t;
        const auto before = renderDocument(d);
        distortLayer(*layer, layer->transform(), transformCorners(layer->transform()));
        QVERIFY(layer->transform().value("rotation").toDouble() == 0);
        QVERIFY(!layer->transform().value("flipX").toBool());
        const auto after = renderDocument(d);
        QVERIFY(meanDifference(before, after) < 6.0);
        // Flipped, the red half is the one on the right.
        QVERIFY(after.pixelColor(48, 40).red() > 200 && after.pixelColor(48, 40).blue() < 50);
        QVERIFY(after.pixelColor(31, 30).blue() > 200 && after.pixelColor(31, 30).red() < 50);
    }
    void distortionPlacesPixelsOnTheShape() {
        auto d = Document::create({64, 64});
        QImage red(40, 40, QImage::Format_RGBA8888_Premultiplied);
        red.fill(Qt::red);
        const auto id = d.addImage("Red", red);
        auto *layer = d.find(id);
        layer->setBounds(QRectF(10, 10, 40, 40));
        // The top-right corner pulled down 20 pixels.
        auto corners = transformCorners(layer->transform());
        corners[1].ry() += 20;
        distortLayer(*layer, layer->transform(), corners);
        QCOMPARE(boxOf(*layer), QRectF(10, 10, 40, 40));
        const auto shot = renderDocument(d);
        QCOMPARE(shot.pixelColor(14, 16).alpha(), 255);  // Inside, near the top-left.
        QCOMPARE(shot.pixelColor(46, 14).alpha(), 0);    // Above the slanted top edge.
        QCOMPARE(shot.pixelColor(46, 44).alpha(), 255);  // Inside, near the bottom-right.
    }
    void foldedShapesAreWarpedAsTriangles() {
        auto d = Document::create({64, 64});
        QImage red(40, 40, QImage::Format_RGBA8888_Premultiplied);
        red.fill(Qt::red);
        const auto id = d.addImage("Red", red);
        auto *layer = d.find(id);
        layer->setBounds(QRectF(0, 0, 40, 40));
        const QVector<QPointF> folded{{0, 0}, {40, 0}, {10, 10}, {0, 40}};
        QVERIFY(!distortConvex(folded));
        distortLayer(*layer, layer->transform(), folded);
        const auto shot = renderDocument(d);
        QCOMPARE(shot.pixelColor(2, 20).alpha(), 255);
        QCOMPARE(shot.pixelColor(35, 30).alpha(), 0);
    }
    void collapsedShapesAreRejected() {
        auto d = Document::create({64, 64});
        const auto id = d.addImage("Red", halves({40, 40}));
        auto *layer = d.find(id);
        const QVector<QPointF> line{{0, 0}, {10, 0}, {20, 0}, {30, 0}};
        QVERIFY_EXCEPTION_THROWN(distortLayer(*layer, layer->transform(), line), Error);
    }
    void maskFollowsTheDistortion() {
        auto d = Document::create({64, 64});
        QImage red(20, 10, QImage::Format_RGBA8888_Premultiplied);
        red.fill(Qt::red);
        const auto id = d.addImage("Masked", red);
        auto *layer = d.find(id);
        layer->setBounds(QRectF(10, 20, 40, 20));
        layer->mask = QImage(20, 10, QImage::Format_Grayscale8);
        layer->mask.fill(0);
        for (int y = 0; y < 10; ++y)
            std::fill(layer->mask.scanLine(y) + 10, layer->mask.scanLine(y) + 20, uchar(255));
        layer->metadata["maskFile"] = id + ".mask.png";
        auto corners = transformCorners(layer->transform());
        corners[2].rx() += 10; // Bottom-right pulled right.
        distortLayer(*layer, layer->transform(), corners);
        QCOMPARE(layer->mask.size(), layer->image.size());
        QCOMPARE(layer->mask.format(), QImage::Format_Grayscale8);
        const auto shot = renderDocument(d);
        QCOMPARE(shot.pixelColor(15, 30).alpha(), 0);   // The masked-out left half.
        QCOMPARE(shot.pixelColor(44, 30).alpha(), 255); // The visible right half.
    }
    void ctrlDraggingAHandleDistortsAsOneUndoStep() {
        EditorPage p(twoLayers());
        p.document.metadata["activeLayerID"] = p.document.layers[0].id();
        p.session.selectedLayerIDs = {p.document.layers[0].id()};
        ready(p);
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        // The 16-pixel layer's top-right corner pulled down 8 pixels, then applied with Enter.
        drag(p, {16, 0}, {16, 8}, Qt::ControlModifier);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Distort"));
        QVERIFY(p.document.active()->image != image);
        QCOMPARE(boxOf(*p.document.active()), QRectF(0, 0, 16, 16));
        p.canvas->waitForRendering();
        const auto shot = renderDocument(p.document);
        QCOMPARE(shot.pixelColor(14, 2).alpha(), 0); // Above the slanted edge.
        QCOMPARE(shot.pixelColor(2, 6).alpha(), 255);
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        p.history.redo();
        QVERIFY(p.document.active()->image != image);
    }
    void aDistortionWaitsUntilAppliedAndKeepsReshaping() {
        // As on the Mac: a distortion waits once its handle is let go, its handles keep
        // distorting without Ctrl, and the pixels are resampled once, when it is applied.
        EditorPage p(twoLayers());
        p.document.metadata["activeLayerID"] = p.document.layers[0].id();
        p.session.selectedLayerIDs = {p.document.layers[0].id()};
        ready(p);
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        drag(p, {16, 0}, {16, 8}, Qt::ControlModifier);
        QVERIFY(p.canvas->hasPendingDistortion());
        QVERIFY(p.canvas->hasLivePreview()); // Shown on the canvas, not yet made.
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        // The bottom-right handle, dragged without Ctrl, keeps distorting.
        drag(p, {16, 16}, {24, 16});
        QVERIFY(p.canvas->hasPendingDistortion());
        // A drag away from the handles moves the whole shape.
        drag(p, {40, 40}, {44, 40});
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.active()->image, image);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QVERIFY(!p.canvas->hasPendingDistortion());
        QVERIFY(!p.canvas->hasLivePreview());
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Distort"));
        const auto box = boxOf(*p.document.active());
        QVERIFY2(std::abs(box.left() - 4) <= 1 && std::abs(box.right() - 28) <= 1,
                 qPrintable(QString("%1 %2").arg(box.left()).arg(box.right())));
        // Full resolution: the layer was not left at a preview size.
        QVERIFY(p.document.active()->image.width() >= 23);
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
        // Escape abandons a waiting distortion.
        drag(p, {16, 0}, {16, 8}, Qt::ControlModifier);
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QVERIFY(!p.canvas->hasPendingDistortion());
        QVERIFY(!p.canvas->hasLivePreview());
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        // Choosing another tool applies it.
        drag(p, {16, 0}, {16, 8}, Qt::ControlModifier);
        p.canvas->setTool(Tool::Brush);
        QVERIFY(!p.canvas->hasPendingDistortion());
        QCOMPARE(p.history.undoText(), QString("Distort"));
        QVERIFY(p.document.active()->image != image);
    }
    void distortingCanBeCanceledAndNeedsAMove() {
        EditorPage p(twoLayers());
        p.session.selectedLayerIDs = {p.document.activeId()};
        ready(p);
        const auto before = p.document.manifest();
        const auto image = p.document.active()->image;
        const auto box = boxOf(*p.document.active());
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, box.right(), box.top()), Qt::ControlModifier);
        mouse(p.canvas, QEvent::MouseMove, at(p, box.right(), box.top() + 6), Qt::ControlModifier);
        QVERIFY(p.document.active()->image != image); // The preview.
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.active()->image, image);
        QCOMPARE(p.history.count(), 0);
        // A handle let go where it was grabbed distorts nothing and records nothing.
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, box.right(), box.top()), Qt::ControlModifier);
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, box.right(), box.top()), Qt::ControlModifier);
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.active()->image, image);
    }
    void selectedLayersScaleTogether() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b));
        p.session.selectedLayerIDs = {a, b};
        ready(p);
        const auto before = p.document.manifest();
        // The box around both is 64 × 16; its bottom-right handle doubles it.
        drag(p, {64, 16}, {128, 32});
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Transform Layers"));
        QCOMPARE(boxOf(*p.document.find(a)), QRectF(0, 0, 32, 32));
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(96, 0, 32, 32));
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(boxOf(*p.document.find(a)), QRectF(0, 0, 16, 16));
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(48, 0, 16, 16));
    }
    void selectedLayersRotateAboutTheirBox() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b));
        p.session.selectedLayerIDs = {a, b};
        ready(p);
        // The rotation handle sits above the box's top-center; drag it round to its right.
        drag(p, {32, -24}, {62, 8}, Qt::ShiftModifier);
        for (const auto &id : {a, b})
            QCOMPARE(std::fmod(p.document.find(id)->transform()["rotation"].toDouble() + 360, 360.0),
                     90.0);
        auto center = [&](const QString &id) {
            const auto c = transformCorners(p.document.find(id)->transform());
            return (c[0] + c[2]) / 2;
        };
        QVERIFY(QLineF(center(a), QPointF(32, -16)).length() < 1e-6);
        QVERIFY(QLineF(center(b), QPointF(32, 32)).length() < 1e-6);
    }
    void selectedLayersDistortTogether() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b));
        p.session.selectedLayerIDs = {a, b};
        ready(p);
        // The box's bottom-right corner pulled right: both layers lean with it.
        drag(p, {64, 16}, {80, 16}, Qt::ControlModifier);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QCOMPARE(p.history.undoText(), QString("Distort Layers"));
        QVERIFY(boxOf(*p.document.find(b)).right() > 64);
        QVERIFY(boxOf(*p.document.find(a)).right() < boxOf(*p.document.find(b)).right());
        p.history.undo();
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(48, 0, 16, 16));
    }
    void overlayFollowsAGroupDistortion() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b));
        p.session.selectedLayerIDs = {a, b};
        ready(p);
        p.canvas->zoomTo(3);
        p.canvas->waitForRendering();
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 64, 16), Qt::ControlModifier);
        mouse(p.canvas, QEvent::MouseMove, at(p, 80, 24), Qt::ControlModifier);
        mouse(p.canvas, QEvent::MouseMove, at(p, 84, 28), Qt::ControlModifier);
        auto shot = p.canvas->grab().toImage();
        for (int i = 0; i < 20 && p.canvas->rendering(); ++i) {
            p.canvas->waitForRendering();
            shot = p.canvas->grab().toImage();
        }
        // The handle square now sits where the corner was dragged to.
        const auto corner = at(p, 84, 28);
        const auto pixelRatio = shot.devicePixelRatio();
        bool found = false;
        for (int y = -4; y <= 4 && !found; ++y)
            for (int x = -4; x <= 4; ++x)
                found |= shot.pixelColor(QPoint(qRound((corner.x() + x) * pixelRatio),
                                               qRound((corner.y() + y) * pixelRatio))) == QColor(Qt::white);
        QVERIFY(found);
        if (qEnvironmentVariableIsSet("COMPOSITOR_TRANSFORM_SCREENSHOT"))
            shot.save(qEnvironmentVariable("COMPOSITOR_TRANSFORM_SCREENSHOT"));
        mouse(p.canvas, QEvent::MouseButtonRelease, at(p, 84, 28), Qt::ControlModifier);
        QTest::keyClick(p.canvas, Qt::Key_Return);
        QCOMPARE(p.history.undoText(), QString("Distort Layers"));
    }
    void aFoldersContentsTransformTogether() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b, true));
        QVERIFY(p.document.active()->group());
        p.session.selectedLayerIDs = {p.document.activeId()};
        ready(p);
        drag(p, {64, 16}, {128, 32});
        QCOMPARE(boxOf(*p.document.find(a)), QRectF(0, 0, 32, 32));
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(96, 0, 32, 32));
        p.history.undo();
        // Moving it with the Move tool takes the contents too.
        drag(p, {8, 8}, {18, 13});
        QCOMPARE(boxOf(*p.document.find(a)), QRectF(10, 5, 16, 16));
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(58, 5, 16, 16));
        QCOMPARE(p.history.undoText(), QString("Move Layer"));
        p.history.undo();
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(48, 0, 16, 16));
    }
    void moveToolDragsAllSelectedLayers() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b));
        p.session.selectedLayerIDs = {a, b};
        ready(p);
        const auto before = p.document.manifest();
        drag(p, {8, 8}, {18, 13});
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Move Layers"));
        QCOMPARE(boxOf(*p.document.find(a)), QRectF(10, 5, 16, 16));
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(58, 5, 16, 16));
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
    }
    void optionDragDuplicatesTheSelection() {
        QString a, b;
        EditorPage p(twoLayers(&a, &b));
        p.session.selectedLayerIDs = {a, b};
        ready(p);
        const auto before = p.document.manifest();
        drag(p, {8, 8}, {18, 13}, Qt::AltModifier);
        QCOMPARE(p.document.layers.size(), 4);
        QCOMPARE(p.history.count(), 1);
        QCOMPARE(p.history.undoText(), QString("Duplicate Layers"));
        // The originals stay; the copies, now selected, moved.
        QCOMPARE(boxOf(*p.document.find(a)), QRectF(0, 0, 16, 16));
        QCOMPARE(boxOf(*p.document.find(b)), QRectF(48, 0, 16, 16));
        QCOMPARE(p.session.selectedLayerIDs.size(), 2);
        QVERIFY(!p.session.selectedLayerIDs.contains(a));
        QRectF copies;
        for (const auto &id : p.session.selectedLayerIDs)
            copies = copies.united(boxOf(*p.document.find(id)));
        QCOMPARE(copies, QRectF(10, 5, 64, 16));
        p.history.undo();
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.document.layers.size(), 2);
        QCOMPARE(p.session.selectedLayerIDs, QSet<QString>({a, b}));
    }
    void escapeDuringAnOptionDragRemovesTheCopies() {
        EditorPage p(twoLayers());
        ready(p);
        const auto before = p.document.manifest();
        const auto selected = p.session.selectedLayerIDs;
        mouse(p.canvas, QEvent::MouseButtonPress, at(p, 56, 8), Qt::AltModifier);
        mouse(p.canvas, QEvent::MouseMove, at(p, 66, 18), Qt::AltModifier);
        QCOMPARE(p.document.layers.size(), 3);
        QTest::keyClick(p.canvas, Qt::Key_Escape);
        QCOMPARE(p.document.layers.size(), 2);
        QCOMPARE(p.document.manifest(), before);
        QCOMPARE(p.session.selectedLayerIDs, selected);
        QCOMPARE(p.history.count(), 0);
    }
    void linkedPlacedMasksFollowResizing() {
        auto d = twoLayers();
        auto *layer = d.active();
        layer->mask = QImage(16, 16, QImage::Format_Grayscale8);
        layer->mask.fill(255);
        layer->metadata["maskFile"] = layer->id() + ".mask.png";
        auto placement = layer->transform();
        placement["origin"] = QJsonArray{52, 4};
        placement["size"] = QJsonArray{8, 8};
        layer->metadata["maskPlacement"] = placement;
        EditorPage p(d);
        ready(p);
        // The layer at (48, 0, 16, 16) doubled from its top-left corner.
        drag(p, {64, 16}, {80, 32});
        const auto mask = p.document.active()->metadata["maskPlacement"].toObject();
        QCOMPARE(mask["origin"].toArray()[0].toDouble(), 56.0);
        QCOMPARE(mask["origin"].toArray()[1].toDouble(), 8.0);
        QCOMPARE(mask["size"].toArray()[0].toDouble(), 16.0);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("TransformTests");
    TransformTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "transform_tests.moc"

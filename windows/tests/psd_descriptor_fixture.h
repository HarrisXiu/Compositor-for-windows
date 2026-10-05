// SPDX-License-Identifier: MIT
#pragma once
#include "psd_fixture.h"
#include <cmath>
inline QByteArray psdBool(bool value) {
    return QByteArray("bool") + char(value);
}
inline QByteArray vectorMaskFixture(bool hole = false, int flags = 0) {
    PsdFixtureWriter w;
    w.u32(3);
    w.u32(flags);
    w.u16(6);
    w.bytes(QByteArray(24, 0));
    w.u16(8);
    w.u16(0);
    w.bytes(QByteArray(22, 0));
    auto rect = [&](double left, double top, double right, double bottom) {
        w.u16(0);
        w.u16(4);
        w.u16(-1);
        w.u16(1);
        w.bytes(QByteArray(18, 0));
        for (auto p : {QPointF(left, top), QPointF(right, top), QPointF(right, bottom),
                       QPointF(left, bottom)}) {
            w.u16(2);
            for (int i = 0; i < 3; ++i) {
                w.u32(quint32(qint32(p.y() * 16777216)));
                w.u32(quint32(qint32(p.x() * 16777216)));
            }
        }
    };
    rect(.125, .125, .875, .875);
    if (hole)
        rect(.375, .375, .625, .625);
    return w.data;
}
inline QByteArray psdDescriptor(const QByteArray &classId,
                                const QList<std::pair<QByteArray, QByteArray>> &items,
                                bool versioned = true) {
    PsdFixtureWriter w;
    if (versioned)
        w.u32(16);
    w.unicode("");
    w.identifier(classId);
    w.u32(quint32(items.size()));
    for (auto &[key, value] : items) {
        w.identifier(key);
        w.bytes(value);
    }
    return w.data;
}
inline QByteArray psdDouble(double v) {
    PsdFixtureWriter w;
    w.bytes("doub");
    w.f64(v);
    return w.data;
}
inline QByteArray psdUnit(double v) {
    PsdFixtureWriter w;
    w.bytes("UntF#Pxl");
    w.f64(v);
    return w.data;
}
inline QByteArray psdLong(int v) {
    PsdFixtureWriter w;
    w.bytes("long");
    w.u32(v);
    return w.data;
}
inline QByteArray psdText(const QString &v) {
    PsdFixtureWriter w;
    w.bytes("TEXT");
    w.unicode(v);
    return w.data;
}
inline QByteArray psdEnum(QByteArray type, QByteArray v) {
    PsdFixtureWriter w;
    w.bytes("enum");
    w.identifier(type);
    w.identifier(v);
    return w.data;
}
inline QByteArray psdData(QByteArray v) {
    PsdFixtureWriter w;
    w.bytes("tdta");
    w.u32(quint32(v.size()));
    w.bytes(v);
    return w.data;
}
inline QByteArray psdObject(QByteArray cls, QList<std::pair<QByteArray, QByteArray>> items) {
    return "Objc" + psdDescriptor(cls, items, false);
}
inline QByteArray typeFixture(bool vertical = false, double shear = 0, bool paragraph = false) {
    PsdFixtureWriter w;
    w.u16(1);
    double angle = .4;
    for (double v : {std::cos(angle), -std::sin(angle) + shear, std::sin(angle), std::cos(angle),
                     100.0, 100.0})
        w.f64(v);
    w.u16(50);
    QByteArray engine = "<< /ResourceDict << /FontSet [ << /Name (Segoe UI) >> ] >> /EngineDict << "
                        "/StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /Font 0 "
                        "/FontSize 18 /Tracking 100 /FillColor << /Values [1 .2 .3 .4] >> "
                        "/AutoLeading false /Leading 22 >> >> >> ] >> /ParagraphRun << /RunArray [ "
                        "<< /ParagraphSheet << /Properties << /Justification 2 >> >> >> ] >> >> >>";
    QList<std::pair<QByteArray, QByteArray>> items{
        {"Txt ", psdText("测试 Hello")},
        {"Ornt", psdEnum("Ornt", vertical ? "Vrtc" : "Hrzn")},
        {"EngineData", psdData(engine)}};
    if (paragraph) {
        items.append({"bounds", psdObject("Rctn", {{"Left", psdDouble(10)},
                                                   {"Top ", psdDouble(20)},
                                                   {"Rght", psdDouble(110)},
                                                   {"Btom", psdDouble(100)}})});
        items.append({"boundingBox", psdObject("Rctn", {{"Left", psdDouble(10)},
                                                        {"Top ", psdDouble(20)},
                                                        {"Rght", psdDouble(50)},
                                                        {"Btom", psdDouble(40)}})});
    }
    w.bytes(psdDescriptor("TxLr", items));
    w.u16(1);
    w.bytes(psdDescriptor("warp", {{"warpStyle", psdEnum("warpStyle", "warpNone")}}));
    return w.data;
}
// Upright paragraph text at (10, 20): "Hello world" whose second word is in another font and
// color; 20-pixel type on 40-pixel lines. `fonts` are the PostScript names of the two fonts.
inline QByteArray styledTypeFixture(const QByteArray &first = "SegoeUI",
                                    const QByteArray &second = "Arial-BoldMT") {
    PsdFixtureWriter w;
    w.u16(1);
    for (double v : {1.0, 0.0, 0.0, 1.0, 10.0, 20.0})
        w.f64(v);
    w.u16(50);
    QByteArray engine = "<< /ResourceDict << /FontSet [ << /Name (" + first + ") >> << /Name (" + second +
                        ") >> ] >> /EngineDict << /StyleRun << /RunArray [ << /StyleSheet << "
                        "/StyleSheetData << /Font 0 /FontSize 20 /FillColor << /Values [1 0 0 0] >> "
                        "/AutoLeading false /Leading 40 >> >> >> << /StyleSheet << /StyleSheetData << "
                        "/Font 1 /FontSize 20 /FillColor << /Values [1 1 0 0] >> /AutoLeading false "
                        "/Leading 40 >> >> >> ] /RunLengthArray [ 6 6 ] >> >> >>";
    QList<std::pair<QByteArray, QByteArray>> items{
        {"Txt ", psdText("Hello world")},
        {"Ornt", psdEnum("Ornt", "Hrzn")},
        {"EngineData", psdData(engine)},
        {"bounds", psdObject("Rctn", {{"Left", psdDouble(0)},
                                      {"Top ", psdDouble(0)},
                                      {"Rght", psdDouble(200)},
                                      {"Btom", psdDouble(100)}})},
        {"boundingBox", psdObject("Rctn", {{"Left", psdDouble(0)},
                                           {"Top ", psdDouble(4)},
                                           {"Rght", psdDouble(110)},
                                           {"Btom", psdDouble(24)}})}};
    w.bytes(psdDescriptor("TxLr", items));
    w.u16(1);
    w.bytes(psdDescriptor("warp", {{"warpStyle", psdEnum("warpStyle", "warpNone")}}));
    return w.data;
}
inline QHash<QByteArray, QByteArray> liveShapeFixture(int kind = 2) {
    auto fill = psdDescriptor("null", {{"Clr ", psdObject("RGBC", {{"Rd  ", psdDouble(30)},
                                                                   {"Grn ", psdDouble(100)},
                                                                   {"Bl  ", psdDouble(200)}})}});
    PsdFixtureWriter w;
    w.u32(1);
    w.bytes(psdDescriptor(
        "null", {{"keyOriginType", psdLong(kind)},
                 {"keyOriginShapeBBox", psdObject("Rctn", {{"Left", psdUnit(20)},
                                                           {"Top ", psdUnit(10)},
                                                           {"Rght", psdUnit(80)},
                                                           {"Btom", psdUnit(50)}})},
                 {"keyOriginRRectRadii", psdObject("Radii", {{"topLeft", psdUnit(5)},
                                                             {"topRight", psdUnit(5)},
                                                             {"bottomRight", psdUnit(5)},
                                                             {"bottomLeft", psdUnit(5)}})}}));
    return {{"SoCo", fill}, {"vogk", w.data}};
}

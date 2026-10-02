// SPDX-License-Identifier: MIT
#pragma once
#include <QByteArray>
#include <QHash>
#include <QRect>
#include <QString>
#include <bit>
#include <vector>
// Tiny independently encoded PSD/PSB files; these fixtures are never used by the application.
struct PsdFixtureLayer {
    QString name = "Layer";
    QRect bounds{0, 0, 2, 2};
    std::vector<std::pair<int, QByteArray>> planes{{-1, QByteArray(4, char(255))},
                                                   {0, QByteArray(4, char(255))},
                                                   {1, QByteArray(4, char(0))},
                                                   {2, QByteArray(4, char(0))}};
    QHash<QByteArray, QByteArray> extra;
    QByteArray blend = "norm";
    int opacity = 255, section = 0, compression = 0;
    bool hidden = false, clipped = false, hasMask = false;
    QRect maskBounds{0, 0, 2, 2};
    int maskDefault = 255, maskFlags = 0;
};
struct PsdFixtureWriter {
    QByteArray data;
    void u8(int v) {
        data.append(char(v));
    }
    void u16(int v) {
        u8((v >> 8) & 255);
        u8(v & 255);
    }
    void u32(quint32 v) {
        u16(int(v >> 16));
        u16(int(v & 65535));
    }
    void u64(quint64 v) {
        u32(quint32(v >> 32));
        u32(quint32(v));
    }
    void f64(double v) {
        u64(std::bit_cast<quint64>(v));
    }
    void identifier(QByteArray key) {
        u32(key.size() == 4 ? 0 : quint32(key.size()));
        bytes(key);
    }
    void unicode(const QString &text) {
        u32(quint32(text.size()));
        for (auto c : text)
            u16(c.unicode());
    }
    void length(quint64 v, bool psb) {
        if (psb)
            u64(v);
        else
            u32(quint32(v));
    }
    void bytes(const QByteArray &a) {
        data.append(a);
    }
    void rect(QRect r) {
        u32(r.y());
        u32(r.x());
        u32(r.y() + r.height());
        u32(r.x() + r.width());
    }
};
inline QByteArray psdFixture(const std::vector<PsdFixtureLayer> &layers, bool psb = false,
                             QSize size = {8, 8}) {
    PsdFixtureWriter header, records, payload;
    header.bytes("8BPS");
    header.u16(psb ? 2 : 1);
    header.bytes(QByteArray(6, 0));
    header.u16(3);
    header.u32(size.height());
    header.u32(size.width());
    header.u16(8);
    header.u16(3);
    header.u32(0);
    PsdFixtureWriter resource;
    resource.bytes("8BIM");
    resource.u16(1005);
    resource.u16(0);
    resource.u32(16);
    resource.u32(144 * 65536);
    resource.bytes(QByteArray(12, 0));
    header.u32(resource.data.size());
    header.bytes(resource.data);
    records.u16(int(layers.size()));
    for (const auto &l : layers) {
        std::vector<std::pair<int, QByteArray>> encoded;
        for (const auto &[id, plane] : l.planes) {
            PsdFixtureWriter bytes;
            bytes.u16(l.compression);
            auto dims = id == -2 ? l.maskBounds.size() : l.bounds.size();
            if (l.compression == 0)
                bytes.bytes(plane);
            else if (l.compression == 1) {
                PsdFixtureWriter counts, body;
                for (int y = 0; y < dims.height(); ++y) {
                    QByteArray row;
                    if (dims.width() > 0) {
                        row.append(char(dims.width() - 1));
                        row.append(plane.mid(y * dims.width(), dims.width()));
                    }
                    if (psb)
                        counts.u32(row.size());
                    else
                        counts.u16(row.size());
                    body.bytes(row);
                }
                bytes.bytes(counts.data);
                bytes.bytes(body.data);
            } else
                bytes.bytes(plane);
            encoded.emplace_back(id, bytes.data);
        }
        records.rect(l.bounds);
        records.u16(int(encoded.size()));
        for (const auto &[id, bytes] : encoded) {
            records.u16(id);
            records.length(bytes.size(), psb);
            payload.bytes(bytes);
        }
        records.bytes("8BIM");
        records.bytes(l.blend);
        records.u8(l.opacity);
        records.u8(l.clipped);
        records.u8(l.hidden ? 2 : 0);
        records.u8(0);
        PsdFixtureWriter extra;
        if (l.hasMask) {
            extra.u32(20);
            extra.rect(l.maskBounds);
            extra.u8(l.maskDefault);
            extra.u8(l.maskFlags);
            extra.u16(0);
        } else
            extra.u32(0);
        extra.u32(0);
        extra.u8(1);
        extra.bytes("L");
        extra.bytes(QByteArray(2, 0));
        auto additional = [&](const QByteArray &key, const QByteArray &bytes) {
            extra.bytes("8BIM");
            extra.bytes(key);
            bool large =
                psb && QList<QByteArray>{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                         "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"}
                           .contains(key);
            extra.length(bytes.size(), large);
            extra.bytes(bytes);
            if (bytes.size() % 2)
                extra.u8(0);
        };
        PsdFixtureWriter unicode;
        unicode.u32(l.name.size());
        for (auto c : l.name)
            unicode.u16(c.unicode());
        additional("luni", unicode.data);
        if (l.section) {
            PsdFixtureWriter section;
            section.u32(l.section);
            additional("lsct", section.data);
        }
        for (auto it = l.extra.begin(); it != l.extra.end(); ++it)
            additional(it.key(), it.value());
        records.u32(extra.data.size());
        records.bytes(extra.data);
    }
    records.bytes(payload.data);
    if (records.data.size() % 2)
        records.u8(0);
    PsdFixtureWriter info, section;
    info.length(records.data.size(), psb);
    info.bytes(records.data);
    info.u32(0);
    header.length(info.data.size(), psb);
    header.bytes(info.data);
    header.u16(0);
    header.bytes(QByteArray(size.width() * size.height() * 3, char(25)));
    return header.data;
}

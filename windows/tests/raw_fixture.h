// SPDX-License-Identifier: MIT
#pragma once
#include <QByteArray>
#include <QList>
#include <QSize>
#include <algorithm>
// A small, uncompressed Bayer DNG, authored here without camera files or external fixtures.
inline QByteArray dngFixture(int orientation = 1, QSize size = {48, 32}) {
    auto le16 = [](quint16 v) {
        QByteArray b;
        b.append(char(v));
        b.append(char(v >> 8));
        return b;
    };
    auto le32 = [](quint32 v) {
        QByteArray b;
        for (int i = 0; i < 4; ++i)
            b.append(char(v >> (8 * i)));
        return b;
    };
    struct Tag {
        quint16 id, type;
        quint32 count;
        QByteArray data;
    };
    QList<Tag> tags;
    auto shortTag = [&](int id, int v) { tags.append({quint16(id), 3, 1, le16(quint16(v))}); };
    auto longTag = [&](int id, int v) { tags.append({quint16(id), 4, 1, le32(quint32(v))}); };
    auto stringTag = [&](int id, const char *s) {
        QByteArray b(s);
        b.append('\0');
        tags.append({quint16(id), 2, quint32(b.size()), b});
    };
    longTag(254, 0);
    longTag(256, size.width());
    longTag(257, size.height());
    shortTag(258, 16);
    shortTag(259, 1);
    shortTag(262, 32803);
    stringTag(271, "Compositor");
    stringTag(272, "Synthetic Bayer");
    longTag(273, 0);
    shortTag(274, orientation);
    shortTag(277, 1);
    longTag(278, size.height());
    longTag(279, size.width() * size.height() * 2);
    shortTag(284, 1);
    tags.append({33421, 3, 2, le16(2) + le16(2)});
    tags.append({33422, 1, 4, QByteArray::fromHex("00010102")});
    tags.append({50706, 1, 4, QByteArray::fromHex("01040000")});
    tags.append({50707, 1, 4, QByteArray::fromHex("01010000")});
    stringTag(50708, "Compositor Synthetic DNG");
    tags.append({50710, 1, 3, QByteArray::fromHex("000102")});
    shortTag(50711, 1);
    tags.append({50713, 3, 2, le16(1) + le16(1)});
    tags.append({50714, 5, 1, le32(64) + le32(1)});
    longTag(50717, 65535);
    QByteArray matrix;
    for (int i = 0; i < 9; ++i)
        matrix += le32(i % 4 == 0 ? 1 : 0) + le32(1);
    tags.append({50721, 10, 9, matrix});
    tags.append({50728, 5, 3, le32(1) + le32(2) + le32(1) + le32(1) + le32(2) + le32(3)});
    shortTag(50778, 21);
    std::sort(tags.begin(), tags.end(), [](auto &a, auto &b) { return a.id < b.id; });
    int offset = 8 + 2 + 12 * int(tags.size()) + 4;
    QByteArray table = le16(quint16(tags.size())), extra;
    for (auto &tag : tags)
        if (tag.data.size() > 4)
            offset += int(tag.data.size()) + (tag.data.size() % 2);
    for (auto &tag : tags) {
        if (tag.id == 273)
            tag.data = le32(offset);
        table += le16(tag.id) + le16(tag.type) + le32(tag.count);
        if (tag.data.size() <= 4)
            table += tag.data.leftJustified(4, '\0');
        else {
            table += le32(quint32(8 + 2 + 12 * tags.size() + 4 + extra.size()));
            extra += tag.data;
            if (extra.size() % 2)
                extra.append('\0');
        }
    }
    table += le32(0);
    QByteArray pixels;
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            int channel = (y % 2 == 0 && x % 2 == 0) ? 0 : (y % 2 && x % 2) ? 2 : 1;
            int value = 4000 + x * 350 + y * 80;
            double multiplier = channel == 0 ? .55 : channel == 2 ? .7 : 1;
            pixels += le16(quint16(64 + value * multiplier));
        }
    return QByteArray("II") + le16(42) + le32(8) + table + extra + pixels;
}

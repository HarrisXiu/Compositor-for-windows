// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QByteArrayView>
#include <bit>
#include <limits>
namespace compositor {
// Every sub-reader is confined to its declared section. Lengths never escape the mapped file.
class BinaryReader {
  public:
    explicit BinaryReader(QByteArrayView data) : data_(data) {}
    qsizetype remaining() const {
        return data_.size() - position_;
    }
    void need(qsizetype length) const {
        require(length >= 0 && length <= remaining(), "Truncated or invalid Photoshop file");
    }
    QByteArrayView bytes(qsizetype length) {
        need(length);
        auto out = data_.sliced(position_, length);
        position_ += length;
        return out;
    }
    void skip(qsizetype length) {
        (void)bytes(length);
    }
    quint8 u8() {
        return quint8(bytes(1)[0]);
    }
    quint16 u16() {
        auto a = bytes(2);
        return (quint16(quint8(a[0])) << 8) | quint8(a[1]);
    }
    qint16 i16() {
        return qint16(u16());
    }
    quint32 u32() {
        auto a = bytes(4);
        return (quint32(quint8(a[0])) << 24) | (quint32(quint8(a[1])) << 16) |
               (quint32(quint8(a[2])) << 8) | quint8(a[3]);
    }
    qint32 i32() {
        return qint32(u32());
    }
    quint64 u64() {
        auto hi = u32();
        auto lo = u32();
        return (quint64(hi) << 32) | lo;
    }
    double f64() {
        return std::bit_cast<double>(u64());
    }
    qsizetype length(bool large = false) {
        quint64 value = large ? u64() : u32();
        require(value <= quint64(std::numeric_limits<qsizetype>::max()),
                "Photoshop section is too large");
        auto out = qsizetype(value);
        need(out);
        return out;
    }
    BinaryReader section(bool large = false) {
        return BinaryReader(bytes(length(large)));
    }
    QByteArray string(qsizetype length) {
        auto a = bytes(length);
        return QByteArray(a.data(), a.size());
    }

  private:
    QByteArrayView data_;
    qsizetype position_ = 0;
};
} // namespace compositor

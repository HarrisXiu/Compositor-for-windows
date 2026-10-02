// SPDX-License-Identifier: MIT
// Port of Compositor/IO/PSD (MIT). Format reference: Adobe Photoshop File Formats, November 2019.
#include "photoshop.h"
#include "binary_reader.h"
#include "editable_layers.h"
#include "filters.h"
#include "photoshop_editable.h"
#include <QColorSpace>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace compositor {
namespace {
struct Bounds {
    qint64 left = 0, top = 0, right = 0, bottom = 0;
    int width() const {
        return int(right - left);
    }
    int height() const {
        return int(bottom - top);
    }
    QRectF rect() const {
        return {double(left), double(top), double(width()), double(height())};
    }
};
Bounds bounds(BinaryReader &r) {
    Bounds b;
    b.top = r.i32();
    b.left = r.i32();
    b.bottom = r.i32();
    b.right = r.i32();
    require(b.right >= b.left && b.bottom >= b.top && b.right - b.left <= 300000 &&
                b.bottom - b.top <= 300000,
            "Invalid Photoshop layer bounds");
    return b;
}
struct RawLayer {
    QString name = "Layer";
    Bounds source, image, maskSource, mask;
    std::vector<std::pair<int, qsizetype>> channels;
    QHash<QByteArray, QByteArrayView> extra;
    int section = 0;
    double opacity = 1, fill = 1;
    bool hidden = false, clipping = false, hasMask = false, maskEnabled = true, maskLinked = true,
         maskFromRender = false, cropped = false;
    uchar maskDefault = 255;
    QByteArray blend = "norm";
    QImage pixels, maskPixels;
};
QString unicodeName(QByteArrayView data) {
    BinaryReader r(data);
    auto count = r.u32();
    require(count <= 100000, "Photoshop name exceeds limit");
    r.need(qsizetype(count) * 2);
    QString out;
    out.reserve(count);
    for (quint32 i = 0; i < count; ++i)
        out.append(QChar(r.u16()));
    while (out.endsWith(QChar(0)))
        out.chop(1);
    return out;
}
QString pascalName(QByteArrayView bytes) {
#ifdef Q_OS_WIN
    int count = MultiByteToWideChar(10000, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (count > 0) {
        std::wstring units(size_t(count), L'\0');
        MultiByteToWideChar(10000, 0, bytes.data(), int(bytes.size()), units.data(), count);
        return QString::fromStdWString(units);
    }
#endif
    return QString::fromLatin1(bytes.data(), bytes.size());
}
RawLayer readRecord(BinaryReader &r, bool psb) {
    RawLayer l;
    l.source = l.image = bounds(r);
    int count = r.u16();
    require(count <= 56, "Too many Photoshop layer channels");
    for (int i = 0; i < count; ++i) {
        int id = r.i16();
        quint64 size = psb ? r.u64() : r.u32();
        require(size <= quint64(std::numeric_limits<qsizetype>::max()),
                "Photoshop channel is too large");
        l.channels.emplace_back(id, qsizetype(size));
    }
    require(r.string(4) == "8BIM", "Invalid Photoshop layer signature");
    l.blend = r.string(4);
    l.opacity = r.u8() / 255.0;
    l.clipping = r.u8() != 0;
    l.hidden = (r.u8() & 2) != 0;
    r.skip(1);
    auto extra = r.section();
    auto mask = extra.section();
    if (mask.remaining() >= 18) {
        l.hasMask = true;
        l.maskSource = l.mask = bounds(mask);
        l.maskDefault = mask.u8();
        int flags = mask.u8();
        l.maskEnabled = (flags & 2) == 0;
        l.maskLinked = (flags & 1) == 0;
        l.maskFromRender = (flags & 8) != 0;
    }
    extra.skip(extra.length());
    int nameSize = extra.u8();
    l.name = pascalName(extra.bytes(nameSize));
    extra.skip((4 - (nameSize + 1) % 4) % 4);
    const QList<QByteArray> largeKeys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                      "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    int blocks = 0;
    while (extra.remaining() >= 12) {
        require(++blocks <= 10000, "Too many Photoshop layer blocks");
        auto signature = extra.string(4);
        require(signature == "8BIM" || signature == "8B64",
                "Invalid Photoshop additional layer block");
        auto key = extra.string(4);
        auto size = extra.length(signature == "8B64" || (psb && largeKeys.contains(key)));
        auto payload = extra.bytes(size);
        l.extra[key] = payload;
        if (size % 2)
            extra.skip(1);
        if (key == "luni")
            l.name = unicodeName(payload);
        else if (key == "iOpa" && !payload.empty())
            l.fill = quint8(payload[0]) / 255.0;
        else if ((key == "lsct" || key == "lsdk") && payload.size() >= 4) {
            BinaryReader section(payload);
            l.section = int(section.u32());
        }
    }
    if (l.name.trimmed().isEmpty())
        l.name = "Layer";
    require(l.name.toUtf8().size() <= 16384, "Photoshop layer name exceeds limit");
    return l;
}
Bounds cropped(Bounds b, QSize canvas) {
    b.left = std::clamp(b.left, qint64(0), qint64(canvas.width()));
    b.top = std::clamp(b.top, qint64(0), qint64(canvas.height()));
    b.right = std::clamp(b.right, b.left, qint64(canvas.width()));
    b.bottom = std::clamp(b.bottom, b.top, qint64(canvas.height()));
    return b;
}
bool fits(const std::vector<RawLayer> &layers, qint64 budget) {
    qint64 colors = 0, masks = 0;
    for (const auto &l : layers) {
        colors += qint64(l.image.width()) * l.image.height();
        masks += qint64(l.mask.width()) * l.mask.height();
        if (l.image.width() > MaxSide || l.image.height() > MaxSide || l.mask.width() > MaxSide ||
            l.mask.height() > MaxSide || colors > budget || masks > budget)
            return false;
    }
    return true;
}
QByteArray unpackRle(BinaryReader &r, const std::vector<quint32> &lengths, int width, int height,
                     QRect crop) {
    QByteArray output(qsizetype(crop.width()) * crop.height(), Qt::Uninitialized);
    std::vector<uchar> row(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        BinaryReader encoded(r.bytes(lengths[y]));
        if (y < crop.top() || y >= crop.top() + crop.height())
            continue;
        int written = 0;
        while (written < width) {
            int n = qint8(encoded.u8());
            if (n == -128)
                continue;
            if (n >= 0) {
                int count = n + 1;
                require(count <= width - written, "Invalid Photoshop RLE run");
                auto bytes = encoded.bytes(count);
                std::copy_n(reinterpret_cast<const uchar *>(bytes.data()), count,
                            row.data() + written);
                written += count;
            } else {
                int count = 1 - n;
                require(count <= width - written, "Invalid Photoshop RLE run");
                auto value = encoded.u8();
                std::fill_n(row.data() + written, count, value);
                written += count;
            }
        }
        std::copy_n(row.data() + crop.x(), crop.width(),
                    reinterpret_cast<uchar *>(output.data()) +
                        qsizetype(y - crop.y()) * crop.width());
    }
    return output;
}
QByteArray decodePlane(BinaryReader &r, int compression, int width, int height, bool psb,
                       QRect crop) {
    require(crop.x() >= 0 && crop.y() >= 0 && crop.width() >= 0 && crop.height() >= 0 &&
                qint64(crop.x()) + crop.width() <= width &&
                qint64(crop.y()) + crop.height() <= height,
            "Invalid Photoshop channel crop");
    if (compression == 0) {
        auto source = r.bytes(qsizetype(width) * height);
        QByteArray out(qsizetype(crop.width()) * crop.height(), Qt::Uninitialized);
        for (int y = 0; y < crop.height(); ++y)
            std::copy_n(source.data() + qsizetype(y + crop.y()) * width + crop.x(), crop.width(),
                        out.data() + qsizetype(y) * crop.width());
        return out;
    }
    require(compression == 1,
            "Unsupported Photoshop compression; use 8-bit RGB with Raw or RLE channels");
    std::vector<quint32> lengths;
    lengths.reserve(height);
    for (int y = 0; y < height; ++y)
        lengths.push_back(psb ? r.u32() : r.u16());
    return unpackRle(r, lengths, width, height, crop);
}
void premultiply(QImage &image) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            auto p = image.scanLine(y) + x * 4;
            for (int c = 0; c < 3; ++c)
                p[c] = uchar((int(p[c]) * p[3] + 127) / 255);
        }
}
void decodeChannels(BinaryReader &r, RawLayer &l, bool psb) {
    auto size = QSize(l.image.width(), l.image.height());
    if (!size.isEmpty()) {
        l.pixels = QImage(size, QImage::Format_RGBA8888_Premultiplied);
        require(!l.pixels.isNull(), "Not enough memory for Photoshop pixels");
        l.pixels.fill(Qt::black);
    }
    for (auto [id, length] : l.channels) {
        auto channel = BinaryReader(r.bytes(length));
        if (id < -2 || id > 2 || channel.remaining() < 2)
            continue;
        int compression = channel.u16();
        bool mask = id == -2;
        const auto &source = mask ? l.maskSource : l.source;
        const auto &target = mask ? l.mask : l.image;
        if (target.width() <= 0 || target.height() <= 0)
            continue;
        QRect crop(int(target.left - source.left), int(target.top - source.top), target.width(),
                   target.height());
        auto plane = decodePlane(channel, compression, source.width(), source.height(), psb, crop);
        if (mask) {
            l.maskPixels = QImage(target.width(), target.height(), QImage::Format_Grayscale8);
            require(!l.maskPixels.isNull(), "Not enough memory for Photoshop mask");
            for (int y = 0; y < target.height(); ++y)
                std::copy_n(reinterpret_cast<const uchar *>(plane.constData()) +
                                qsizetype(y) * target.width(),
                            target.width(), l.maskPixels.scanLine(y));
        } else {
            int c = id == -1 ? 3 : id;
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    l.pixels.scanLine(y)[x * 4 + c] =
                        uchar(plane.constData()[qsizetype(y) * size.width() + x]);
        }
    }
    if (!l.pixels.isNull())
        premultiply(l.pixels);
}
QJsonObject parseAdjustment(const RawLayer &l) {
    if (l.extra.contains("nvrt"))
        return {{"kind", "Invert"}};
    if (l.extra.contains("levl")) {
        auto payload = l.extra.value("levl");
        if (payload.size() < 42)
            return {};
        BinaryReader r(payload);
        r.u16();
        QJsonArray ranges;
        for (int c = 0; c < 4; ++c) {
            double black = std::clamp(double(r.u16()), 0.0, 254.0),
                   white = std::clamp(double(r.u16()), black + 1, 255.0),
                   outBlack = std::clamp(double(r.u16()), 0.0, 255.0),
                   outWhite = std::clamp(double(r.u16()), 0.0, 255.0),
                   gamma = std::clamp(r.u16() / 100.0, .1, 9.99);
            ranges.append(QJsonObject{{"black", black},
                                      {"white", white},
                                      {"outputBlack", outBlack},
                                      {"outputWhite", outWhite},
                                      {"gamma", gamma}});
        }
        return makeAdjustment("Levels", {{"channel", "RGB"}, {"ranges", ranges}});
    }
    if (l.extra.contains("curv")) {
        BinaryReader r(l.extra.value("curv"));
        if (r.remaining() < 5)
            return {};
        auto first = r.u8();
        if (first != 0)
            return {};
        auto version = r.u16();
        if (version != 1 && version != 4)
            return {};
        auto count = r.u16();
        if (count > 56)
            return {};
        QJsonArray identity{QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 255}, {"y", 255}}},
            channels{identity, identity, identity, identity};
        for (int c = 0; c < std::min(4, int(count)); ++c) {
            int countPoints = r.u16();
            if (countPoints > 32)
                return {};
            std::vector<QPointF> points;
            for (int i = 0; i < countPoints; ++i) {
                double output = std::min(255, int(r.u16())), input = std::min(255, int(r.u16()));
                points.emplace_back(input, output);
            }
            if (points.size() < 2)
                continue;
            std::sort(points.begin(), points.end(), [](auto a, auto b) { return a.x() < b.x(); });
            if (points.front().x() != 0)
                points.insert(points.begin(), {0, points.front().y()});
            if (points.back().x() != 255)
                points.push_back({255, points.back().y()});
            QJsonArray curve;
            double previous = -1;
            for (auto p : points) {
                if (p.x() <= previous)
                    return {};
                curve.append(QJsonObject{{"x", p.x()}, {"y", p.y()}});
                previous = p.x();
            }
            if (curve.size() > 32)
                return {};
            channels[c] = curve;
        }
        return makeAdjustment("Curves", {{"channel", "RGB"}, {"channels", channels}});
    }
    if (l.extra.contains("hue2") || l.extra.contains("hue ")) {
        BinaryReader r(l.extra.value(l.extra.contains("hue2") ? "hue2" : "hue "));
        if (r.remaining() < 16)
            return {};
        r.u16();
        bool colorize = r.u8() != 0;
        r.u8();
        auto values = [&] {
            double h = r.i16(), s = r.i16(), v = r.i16();
            return QJsonObject{{"hue", h}, {"saturation", s}, {"lightness", v}};
        };
        auto color = values(), master = values();
        QJsonObject adjustments{{"Master", colorize ? color : master}}, bands;
        for (auto name : {"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"}) {
            if (r.remaining() < 14)
                break;
            QJsonObject band;
            for (auto key : {"falloffStart", "rangeStart", "rangeEnd", "falloffEnd"}) {
                double d = std::fmod(r.i16(), 360);
                band[QLatin1String(key)] = d < 0 ? d + 360 : d;
            }
            auto a = values();
            bands[QLatin1String(name)] = band;
            adjustments[QLatin1String(name)] = a;
        }
        return makeAdjustment("Hue/Saturation", {{"range", "Master"},
                                                 {"colorize", colorize},
                                                 {"invertRange", false},
                                                 {"adjustments", adjustments},
                                                 {"bands", bands}});
    }
    return {};
}
QString blendMode(QByteArray key) {
    static const QHash<QByteArray, QString> modes{
        {"norm", "Normal"},       {"mul ", "Multiply"},
        {"scrn", "Screen"},       {"over", "Overlay"},
        {"sLit", "Soft Light"},   {"dark", "Darken"},
        {"lite", "Lighten"},      {"diff", "Difference"},
        {"div ", "Color Dodge"},  {"idiv", "Color Burn"},
        {"hue ", "Hue"},          {"sat ", "Saturation"},
        {"colr", "Color"},        {"lum ", "Luminosity"},
        {"lbrn", "Linear Burn"},  {"lddg", "Linear Dodge (Add)"},
        {"hLit", "Hard Light"},   {"vLit", "Vivid Light"},
        {"lLit", "Linear Light"}, {"pLit", "Pin Light"},
        {"hMix", "Hard Mix"},     {"smud", "Exclusion"},
        {"fsub", "Subtract"},     {"fdiv", "Divide"}};
    return modes.value(key);
}
QImage maskOnGrid(const RawLayer &r, QSize size, QPoint origin) {
    QImage mask(size, QImage::Format_Grayscale8);
    require(!mask.isNull(), "Not enough memory for Photoshop mask grid");
    mask.fill(r.maskDefault);
    if (r.maskPixels.isNull())
        return mask;
    qint64 x = r.mask.left - origin.x(), y = r.mask.top - origin.y();
    for (int row = 0; row < r.maskPixels.height(); ++row) {
        qint64 targetY = y + row;
        if (targetY < 0 || targetY >= size.height())
            continue;
        int first = int(std::max(qint64(0), -x)),
            last = int(std::min(qint64(r.maskPixels.width()), qint64(size.width()) - x));
        if (first < last)
            std::copy_n(r.maskPixels.constScanLine(row) + first, last - first,
                        mask.scanLine(int(targetY)) + int(x + first));
    }
    return mask;
}
void assemble(PhotoshopImport &import, std::vector<RawLayer> &raw, qint64 budget,
              const QColorSpace &profile) {
    auto &d = import.document;
    std::vector<QString> groups;
    QHash<QString, QString> bases;
    qint64 colors = 0, masks = 0;
    const QList<QByteArray> adjustmentKeys{"levl", "curv", "hue2", "hue ", "expA", "grdm",
                                           "brit", "blnc", "nvrt", "thrs", "post", "mixr",
                                           "selc", "blwh", "phfl", "vibA"};
    for (auto &r : raw) {
        if (r.section == 3) {
            groups.push_back(newId());
            require(groups.size() <= 64, "Photoshop folder nesting exceeds limit");
            continue;
        }
        bool group = r.section == 1 || r.section == 2;
        QString id;
        if (group) {
            require(!groups.empty(), "Photoshop folder divider is missing");
            id = groups.back();
            groups.pop_back();
        } else
            id = newId();
        QString parent = groups.empty() ? QString() : groups.back();
        QJsonObject adjustment;
        try {
            adjustment = parseAdjustment(r);
        } catch (const Error &) {
            import.conversions << r.name + ": damaged adjustment settings were skipped.";
        }
        bool hasAdjustment = std::any_of(adjustmentKeys.begin(), adjustmentKeys.end(),
                                         [&](const auto &key) { return r.extra.contains(key); });
        if (hasAdjustment && adjustment.isEmpty() && !group) {
            import.conversions << r.name + ": unsupported Photoshop adjustment was skipped.";
            bases.remove(parent);
            continue;
        }
        bool hasEffects =
            r.extra.contains("lfx2") || r.extra.contains("lrFX") || r.extra.contains("lmfx");
        auto note = [&](const QString &message) { import.conversions << r.name + ": " + message; };
        if (hasEffects)
            note("Photoshop layer effects were discarded; appearance may differ.");
        if (r.extra.contains("SoLd") || r.extra.contains("SoLE"))
            note("Smart object imported as pixels; linked contents cannot be edited.");
        auto editable = photoshopEditable(r.extra, d.size());
        for (const auto &message : editable.notes)
            note(message);
        if (r.extra.contains("txt2") && editable.text.isEmpty())
            note("Unsupported Photoshop type engine was rasterized.");
        if (r.cropped)
            note("Pixels outside the canvas were cropped to fit the memory budget.");
        auto blend = blendMode(r.blend);
        if (group) {
            if (r.blend != "pass" && r.blend != "norm")
                note("Folder blending converted to Normal pass-through.");
            blend = "Normal";
        } else if (blend.isEmpty()) {
            note("Unsupported blend mode converted to Normal.");
            blend = "Normal";
        }
        double opacity = r.opacity * (hasEffects && r.fill != 1 ? 1 : r.fill);
        auto rect = group || !adjustment.isEmpty() ? QRectF(QPointF(), d.size()) : r.image.rect();
        if (rect.isEmpty())
            rect = QRectF(QPointF(), d.size());
        Layer l;
        l.metadata = {
            {"id", id},           {"name", r.name},     {"isVisible", !r.hidden},
            {"opacity", opacity}, {"blendMode", blend}, {"transform", makeTransform(rect)}};
        if (!parent.isEmpty())
            l.metadata["parentID"] = parent;
        if (group)
            l.metadata["isGroup"] = true;
        if (!adjustment.isEmpty()) {
            l.metadata["adjustment"] = adjustment;
            note("Adjustment parameters may differ from Photoshop.");
        } else if (!group && !r.pixels.isNull()) {
            l.image = std::move(r.pixels);
            if (profile.isValid()) {
                l.image.setColorSpace(profile);
                l.image.convertToColorSpace(QColorSpace::SRgb);
            } else
                l.image.setColorSpace(QColorSpace::SRgb);
            l.metadata["imageFile"] = id + ".png";
            colors += qint64(l.image.width()) * l.image.height();
        }
        if (!group && adjustment.isEmpty()) {
            if (!editable.text.isEmpty()) {
                l.metadata["text"] = editable.text;
                {
                    colors -= qint64(l.image.width()) * l.image.height();
                    l.image = renderText(editable.text);
                    l.image.setColorSpace(QColorSpace::SRgb);
                    l.metadata["imageFile"] = id + ".png";
                    QFont font(editable.text.value("fontName").toString());
                    font.setPixelSize(int(std::lround(editable.text.value("fontSize").toDouble())));
                    QFontMetricsF metrics(font);
                    auto family = QFontInfo(font).family();
                    if (family != editable.text.value("fontName").toString())
                        note("Font substituted with " + family + "; text appearance may differ.");
                    QString alignment = editable.text.value("alignment").toString();
                    QPointF anchor = editable.textFrame
                                         ? QPointF(12, 12)
                                         : QPointF(alignment == "Center"  ? l.image.width() / 2.0
                                                   : alignment == "Right" ? l.image.width() - 12.0
                                                                          : 12.0,
                                                   12 + metrics.ascent());
                    auto local = anchor - QPointF(l.image.width() / 2.0, l.image.height() / 2.0);
                    if (editable.textFlipY)
                        local.setY(-local.y());
                    QTransform rotation;
                    rotation.rotate(editable.textRotation);
                    auto center = editable.textAnchor - rotation.map(local);
                    rect = {center - QPointF(l.image.width() / 2.0, l.image.height() / 2.0),
                            l.image.size()};
                    auto transform = makeTransform(rect);
                    transform["rotation"] = editable.textRotation;
                    transform["flipY"] = editable.textFlipY;
                    l.metadata["transform"] = transform;
                    colors += qint64(l.image.width()) * l.image.height();
                }
            } else if (!editable.shape.isEmpty()) {
                l.metadata["shape"] = editable.shape;
                if (l.image.isNull()) {
                    rect = editable.shapeBounds.toAlignedRect();
                    require(qint64(rect.width()) * rect.height() <= budget - colors,
                            "Photoshop shape exceeds memory budget");
                    l.image = renderShape(editable.shape, rect.size().toSize());
                    l.metadata["imageFile"] = id + ".png";
                    l.metadata["transform"] = makeTransform(rect);
                    colors += qint64(l.image.width()) * l.image.height();
                }
            }
        }
        if (r.hasMask && !r.maskFromRender) {
            QSize size = l.image.isNull() ? d.size() : l.image.size();
            require(qint64(size.width()) * size.height() <= budget - masks,
                    "Photoshop masks exceed memory budget");
            if (!editable.text.isEmpty()) {
                l.mask = QImage(size, QImage::Format_Grayscale8);
                require(!l.mask.isNull(), "Not enough memory for text mask");
                l.mask.fill(r.maskDefault);
                QPainter painter(&l.mask);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setTransform(l.placement(size).inverted());
                painter.drawImage(QPointF(r.mask.left, r.mask.top), r.maskPixels);
            } else
                l.mask = maskOnGrid(r, size, rect.topLeft().toPoint());
            masks += qint64(size.width()) * size.height();
            l.metadata["maskFile"] = id + ".mask.png";
            l.metadata["maskEnabled"] = r.maskEnabled;
            l.metadata["maskLinked"] = r.maskLinked;
            if (!r.maskLinked)
                l.metadata["maskPlacement"] = l.metadata.value("transform");
        }
        if (r.clipping) {
            if (bases.contains(parent))
                l.metadata["maskSourceID"] = bases.value(parent);
            else
                note("Clipping was skipped because its base layer is unsupported.");
        } else if (!group && adjustment.isEmpty())
            bases[parent] = id;
        else
            bases.remove(parent);
        require(colors <= budget, "Photoshop pixels exceed memory budget");
        d.layers.push_back(std::move(l));
    }
    require(groups.empty(), "Photoshop folder is incomplete");
    if (!d.layers.isEmpty())
        d.metadata["activeLayerID"] = d.layers.back().id();
}
QImage mergedImage(BinaryReader &r, QSize size, int channels, bool psb) {
    int compression = r.u16();
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    require(!image.isNull(), "Not enough memory for Photoshop composite");
    image.fill(Qt::black);
    std::vector<quint32> lengths;
    if (compression == 1) {
        lengths.reserve(size_t(channels) * size.height());
        for (int i = 0; i < channels * size.height(); ++i)
            lengths.push_back(psb ? r.u32() : r.u16());
    } else
        require(compression == 0, "Unsupported Photoshop composite compression");
    for (int c = 0; c < channels; ++c) {
        QByteArray plane;
        if (compression == 0) {
            auto data = r.bytes(qsizetype(size.width()) * size.height());
            if (c < 4)
                plane = QByteArray(data.data(), data.size());
        } else {
            std::vector<quint32> counts(lengths.begin() + size_t(c) * size.height(),
                                        lengths.begin() + size_t(c + 1) * size.height());
            if (c < 4)
                plane = unpackRle(r, counts, size.width(), size.height(), {QPoint(), size});
            else
                for (auto count : counts)
                    r.skip(count);
        }
        if (c < 4)
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x)
                    image.scanLine(y)[x * 4 + c] =
                        uchar(plane.constData()[qsizetype(y) * size.width() + x]);
    }
    premultiply(image);
    return image;
}
} // namespace
bool isPhotoshopFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == "8BPS";
}
PhotoshopImport readPhotoshop(QByteArrayView data, qint64 pixelBudget) {
    BinaryReader r(data);
    require(r.string(4) == "8BPS", "Not a Photoshop file");
    int version = r.u16();
    require(version == 1 || version == 2, "Unsupported Photoshop file version");
    bool psb = version == 2;
    auto reserved = r.bytes(6);
    for (auto c : reserved)
        require(c == 0, "Invalid Photoshop header");
    int channels = r.u16();
    require(channels >= 3 && channels <= 56, "Invalid Photoshop channel count");
    quint32 height = r.u32(), width = r.u32();
    require(width >= 1 && height >= 1 && width <= MaxSide && height <= MaxSide &&
                qint64(width) * height <= MaxSurfacePixels && qint64(width) * height <= pixelBudget,
            "Photoshop canvas exceeds size or memory limit");
    require(r.u16() == 8, "Only 8-bit RGB Photoshop files can be imported");
    require(r.u16() == 3, "Only 8-bit RGB Photoshop files can be imported");
    r.skip(r.length());
    PhotoshopImport import{Document::create({int(width), int(height)}), {}};
    auto resources = r.section();
    QColorSpace profile;
    while (resources.remaining() >= 12) {
        require(resources.string(4) == "8BIM", "Invalid Photoshop resource signature");
        auto id = resources.u16();
        int length = resources.u8();
        resources.skip(length);
        if ((length + 1) % 2)
            resources.skip(1);
        auto payload = resources.bytes(resources.length());
        if (payload.size() % 2)
            resources.skip(1);
        if (id == 1005 && payload.size() >= 4) {
            BinaryReader resolution(payload);
            double value = resolution.u32() / 65536.0;
            import.document.metadata["resolution"] =
                value < 1 ? 72 : std::clamp(value, 1.0, 9600.0);
        } else if (id == 1039) {
            require(payload.size() <= 16 * 1024 * 1024, "Photoshop ICC profile exceeds 16 MiB");
            profile = QColorSpace::fromIccProfile(QByteArray(payload.data(), payload.size()));
            if (!profile.isValid())
                import.conversions
                    << "Unrecognized ICC profile; imported colors are interpreted as sRGB.";
        }
    }
    auto section = r.section(psb);
    std::vector<RawLayer> raw;
    if (section.remaining() >= (psb ? 8 : 4)) {
        auto info = section.section(psb);
        if (info.remaining() > 0) {
            int count = std::abs(int(info.i16()));
            require(count <= 10000, "Photoshop layer count exceeds limit");
            raw.reserve(count);
            for (int i = 0; i < count; ++i)
                raw.push_back(readRecord(info, psb));
            if (!fits(raw, pixelBudget)) {
                for (auto &l : raw) {
                    auto image = cropped(l.image, import.document.size()),
                         mask = cropped(l.mask, import.document.size());
                    l.cropped = image.rect() != l.image.rect() || mask.rect() != l.mask.rect();
                    l.image = image;
                    l.mask = mask;
                }
                require(fits(raw, pixelBudget), "Photoshop project exceeds memory budget");
            }
            for (auto &l : raw)
                decodeChannels(info, l, psb);
        }
    }
    assemble(import, raw, pixelBudget, profile);
    if (import.document.layers.isEmpty()) {
        auto image = mergedImage(r, import.document.size(), channels, psb);
        if (profile.isValid()) {
            image.setColorSpace(profile);
            image.convertToColorSpace(QColorSpace::SRgb);
        }
        import.document.addImage("Background", image);
    }
    import.document.validateAssets();
    import.conversions.removeDuplicates();
    return import;
}
PhotoshopImport importPhotoshop(const QString &path, qint64 pixelBudget) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot open Photoshop file");
    require(file.size() > 0 && file.size() <= 8LL * 1024 * 1024 * 1024,
            "Photoshop file exceeds 8 GiB limit");
    auto bytes = file.map(0, file.size());
    require(bytes, "Cannot map Photoshop file");
    return readPhotoshop(
        QByteArrayView(reinterpret_cast<const char *>(bytes), qsizetype(file.size())), pixelBudget);
}
} // namespace compositor

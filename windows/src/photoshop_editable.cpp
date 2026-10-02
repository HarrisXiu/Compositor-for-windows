// SPDX-License-Identifier: MIT
// Port of the descriptor and type-engine readers in Compositor/IO/PSD/PSDText.swift.
#include "photoshop_editable.h"
#include "binary_reader.h"
#include <QJsonArray>
#include <QVariant>
#include <algorithm>
#include <cmath>
namespace compositor {
namespace {
QString unicode(BinaryReader &r) {
    auto n = r.u32();
    require(n <= 1000000, "Photoshop descriptor text exceeds limit");
    r.need(qsizetype(n) * 2);
    QString out;
    out.reserve(n);
    for (quint32 i = 0; i < n; ++i)
        out.append(QChar(r.u16()));
    return out;
}
QString identifier(BinaryReader &r) {
    auto n = r.u32();
    require(n <= 10000, "Photoshop descriptor key exceeds limit");
    return QString::fromLatin1(r.string(n ? n : 4));
}
class Descriptor {
    BinaryReader &r_;
    int nodes_ = 0;

  public:
    explicit Descriptor(BinaryReader &r) : r_(r) {}
    QVariantMap read(bool versioned = true, int depth = 0) {
        require(depth <= 64 && ++nodes_ <= 100000, "Photoshop descriptor complexity exceeds limit");
        if (versioned)
            require(r_.u32() == 16, "Unsupported Photoshop descriptor");
        unicode(r_);
        identifier(r_);
        auto count = r_.u32();
        require(count <= 10000, "Photoshop descriptor items exceed limit");
        QVariantMap map;
        for (quint32 i = 0; i < count; ++i) {
            auto key = identifier(r_);
            auto type = r_.string(4);
            map[key] = value(type, depth + 1);
        }
        return map;
    }
    QVariant value(QByteArray type, int depth) {
        require(depth <= 64 && ++nodes_ <= 100000, "Photoshop descriptor complexity exceeds limit");
        if (type == "TEXT")
            return unicode(r_);
        if (type == "enum") {
            identifier(r_);
            return identifier(r_);
        }
        if (type == "doub" || type == "UntF") {
            if (type == "UntF")
                r_.skip(4);
            double v = r_.f64();
            require(std::isfinite(v), "Non-finite Photoshop descriptor value");
            return v;
        }
        if (type == "long")
            return r_.i32();
        if (type == "comp")
            return qint64(r_.u64());
        if (type == "bool")
            return r_.u8() != 0;
        if (type == "Objc" || type == "GlbO")
            return read(false, depth);
        if (type == "tdta" || type == "alis") {
            auto n = r_.u32();
            require(n <= 8000000, "Photoshop descriptor data exceeds limit");
            return r_.string(n);
        }
        if (type == "VlLs") {
            auto count = r_.u32();
            require(count <= 10000, "Photoshop descriptor list exceeds limit");
            QVariantList list;
            for (quint32 i = 0; i < count; ++i) {
                auto itemType = r_.string(4);
                list.append(value(itemType, depth + 1));
            }
            return list;
        }
        if (type == "type" || type == "GlbC") {
            unicode(r_);
            identifier(r_);
            return 0;
        }
        if (type == "obj ") {
            auto n = r_.u32();
            require(n <= 10000, "Photoshop references exceed limit");
            for (quint32 i = 0; i < n; ++i) {
                auto form = r_.string(4);
                if (form == "prop") {
                    unicode(r_);
                    identifier(r_);
                    identifier(r_);
                } else if (form == "Clss") {
                    unicode(r_);
                    identifier(r_);
                } else if (form == "Enmr") {
                    unicode(r_);
                    identifier(r_);
                    identifier(r_);
                    identifier(r_);
                } else if (form == "rele") {
                    unicode(r_);
                    identifier(r_);
                    r_.i32();
                } else if (form == "Idnt" || form == "indx")
                    r_.i32();
                else if (form == "name")
                    unicode(r_);
                else
                    throw Error("Unsupported Photoshop reference");
            }
            return 0;
        }
        throw Error("Unsupported Photoshop descriptor value");
    }
};
QString decodeEngine(QByteArray bytes) {
    if (bytes.startsWith(QByteArray::fromHex("feff"))) {
        BinaryReader r(QByteArrayView(bytes).sliced(2));
        QString out;
        while (r.remaining() >= 2)
            out.append(QChar(r.u16()));
        return out;
    }
    return QString::fromLatin1(bytes);
}
class Engine {
    QByteArrayView data_;
    qsizetype at_ = 0;
    int nodes_ = 0;
    char peek(qsizetype ahead = 0) const {
        return at_ + ahead < data_.size() ? data_[at_ + ahead] : 0;
    }
    bool delimiter(char c) const {
        return uchar(c) <= 32 || QByteArrayView("/<>[]()").contains(c);
    }
    void space() {
        while (at_ < data_.size()) {
            if (uchar(peek()) <= 32)
                ++at_;
            else if (peek() == '%') {
                while (at_ < data_.size() && peek() != '\r' && peek() != '\n')
                    ++at_;
            } else
                break;
        }
    }
    bool take(QByteArrayView token) {
        if (data_.sliced(at_).startsWith(token)) {
            at_ += token.size();
            return true;
        }
        return false;
    }
    QByteArray token() {
        auto start = at_;
        while (at_ < data_.size() && !delimiter(peek()))
            ++at_;
        return QByteArray(data_.data() + start, at_ - start);
    }

  public:
    explicit Engine(QByteArrayView data) : data_(data) {
        auto start = data_.indexOf("<<");
        require(start >= 0, "Missing Photoshop type engine");
        at_ = start;
    }
    QVariant read(int depth = 0) {
        require(depth <= 64 && ++nodes_ <= 100000,
                "Photoshop type engine exceeds complexity limit");
        space();
        require(at_ < data_.size(), "Truncated Photoshop type engine");
        if (take("<<")) {
            QVariantMap map;
            for (;;) {
                space();
                if (take(">>"))
                    return map;
                require(take("/"), "Invalid Photoshop type dictionary");
                auto key = QString::fromLatin1(token());
                require(!key.isEmpty(), "Empty Photoshop type key");
                map[key] = read(depth + 1);
            }
        }
        if (take("[")) {
            QVariantList list;
            for (;;) {
                space();
                if (take("]"))
                    return list;
                list.append(read(depth + 1));
            }
        }
        if (take("(")) {
            QByteArray bytes;
            int nesting = 1;
            while (at_ < data_.size() && nesting) {
                char c = peek();
                ++at_;
                if (c == ')') {
                    if (--nesting)
                        bytes.append(c);
                } else if (c == '(') {
                    require(++nesting <= 64, "Photoshop string nesting exceeds limit");
                    bytes.append(c);
                } else if (c == '\\') {
                    require(at_ < data_.size(), "Truncated Photoshop escape");
                    char e = peek();
                    ++at_;
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int i = 0; i < 2 && peek() >= '0' && peek() <= '7'; ++i) {
                            v = v * 8 + peek() - '0';
                            ++at_;
                        }
                        bytes.append(char(v & 255));
                    } else if (e == 'n')
                        bytes.append('\n');
                    else if (e == 'r')
                        bytes.append('\r');
                    else if (e == 't')
                        bytes.append('\t');
                    else if (e != '\n' && e != '\r')
                        bytes.append(e);
                } else
                    bytes.append(c);
                require(bytes.size() <= 1000000, "Photoshop engine string exceeds limit");
            }
            require(!nesting, "Truncated Photoshop engine string");
            return decodeEngine(bytes);
        }
        if (take("<")) {
            QByteArray hex;
            while (at_ < data_.size() && peek() != '>') {
                if (uchar(peek()) > 32)
                    hex.append(peek());
                ++at_;
            }
            require(take(">"), "Truncated Photoshop hex string");
            return decodeEngine(QByteArray::fromHex(hex));
        }
        if (take("/"))
            return QString::fromLatin1(token());
        auto word = token();
        require(!word.isEmpty(), "Invalid Photoshop engine token");
        if (word == "true")
            return true;
        if (word == "false")
            return false;
        if (word == "null")
            return QString();
        bool ok = false;
        double v = word.toDouble(&ok);
        require(ok && std::isfinite(v), "Invalid Photoshop type number");
        return v;
    }
};
QVariant walk(QVariant value, std::initializer_list<const char *> keys) {
    for (auto key : keys)
        value = value.toMap().value(QLatin1String(key));
    return value;
}
QVariant find(QVariant value, const QString &key) {
    if (value.metaType().id() == QMetaType::QVariantMap) {
        auto m = value.toMap();
        if (m.contains(key))
            return m.value(key);
        for (auto it = m.begin(); it != m.end(); ++it) {
            auto v = find(it.value(), key);
            if (v.isValid())
                return v;
        }
    } else if (value.metaType().id() == QMetaType::QVariantList)
        for (auto &item : value.toList()) {
            auto v = find(item, key);
            if (v.isValid())
                return v;
        }
    return {};
}
QString clean(QString text) {
    while (text.startsWith(QChar(0xfeff)) || text.startsWith(QChar(0)))
        text.remove(0, 1);
    while (text.endsWith(QChar(0)))
        text.chop(1);
    return text.replace("\r\n", "\n").replace('\r', '\n');
}
QRectF rectangle(QVariantMap value) {
    auto side = [&](const char *key) {
        auto k = QLatin1String(key);
        return value.value(k, value.value(QString(k).trimmed())).toDouble();
    };
    return {side("Left"), side("Top "), side("Rght") - side("Left"), side("Btom") - side("Top ")};
}
void text(PhotoshopEditable &out, QByteArrayView data) {
    require(data.size() <= 8000000, "Photoshop type data exceeds limit");
    BinaryReader r(data);
    require(r.u16() == 1, "Unsupported Photoshop type version");
    double xx = r.f64(), xy = r.f64(), yx = r.f64(), yy = r.f64(), tx = r.f64(), ty = r.f64();
    require(std::isfinite(xx) && std::isfinite(xy) && std::isfinite(yx) && std::isfinite(yy) &&
                std::isfinite(tx) && std::isfinite(ty),
            "Invalid Photoshop type placement");
    double scale = std::hypot(xx, yx), cos = xx / std::max(1e-6, scale),
           sin = yx / std::max(1e-6, scale), localX = cos * xy + sin * yy,
           localY = -sin * xy + cos * yy, largest = std::max(scale, std::abs(localY));
    require(scale > 1e-6 && std::abs(localY) > 1e-6 && std::abs(localX) <= .02 * largest &&
                std::abs(scale - std::abs(localY)) <= .02 * largest,
            "Photoshop text shear or uneven scale is unsupported");
    out.textAnchor = {tx, ty};
    out.textRotation = std::atan2(sin, cos) * 180 / 3.141592653589793;
    out.textFlipY = localY < 0;
    require(r.u16() == 50, "Unsupported Photoshop text descriptor version");
    Descriptor reader(r);
    auto description = reader.read();
    require(description.value("Ornt").toString() != "Vrtc",
            "Vertical Photoshop text is unsupported");
    if (r.remaining() >= 2 && r.u16() == 1) {
        auto warp = reader.read().value("warpStyle").toString();
        if (!warp.isEmpty() && warp != "warpNone" && warp != "none")
            out.notes << "Photoshop text warp was omitted.";
    }
    QVariant engine;
    auto bytes = description.value("EngineData").toByteArray();
    if (!bytes.isEmpty()) {
        try {
            engine = Engine(bytes).read();
        } catch (const Error &) {
            out.notes << "Damaged text engine: default text style was used.";
        }
    }
    QString content = clean(description.value("Txt ", description.value("Txt")).toString());
    if (content.isEmpty())
        content = clean(walk(engine, {"EngineDict", "Editor", "Text"}).toString());
    require(!content.isEmpty() && content.size() <= 100000, "Invalid Photoshop text content");
    auto runs = walk(engine, {"EngineDict", "StyleRun", "RunArray"}).toList();
    auto first = runs.isEmpty() ? engine : runs.first();
    auto sheet = walk(first, {"StyleSheet", "StyleSheetData"});
    if (!sheet.isValid())
        sheet = first;
    auto values = walk(sheet, {"FillColor", "Values"}).toList();
    double red = 0, green = 0, blue = 0;
    if (values.size() >= 4) {
        red = values[1].toDouble();
        green = values[2].toDouble();
        blue = values[3].toDouble();
    } else if (values.size() >= 3) {
        red = values[0].toDouble();
        green = values[1].toDouble();
        blue = values[2].toDouble();
    }
    auto channel = [](double v) { return std::clamp(v > 1 ? v / 255 : v, 0.0, 1.0); };
    double fontSize =
        std::clamp(sheet.toMap().value("FontSize", 12).toDouble() * scale, 1.0, 2000.0);
    auto fonts = walk(engine, {"ResourceDict", "FontSet"}).toList();
    int index = sheet.toMap().value("Font").toInt();
    QString font = index >= 0 && index < fonts.size()
                       ? fonts[index].toMap().value("Name").toString()
                       : QString();
    if (font.isEmpty())
        font = "Segoe UI";
    auto paragraphs = walk(engine, {"EngineDict", "ParagraphRun", "RunArray"}).toList();
    int justification = walk(paragraphs.isEmpty() ? engine : paragraphs.first(),
                             {"ParagraphSheet", "Properties", "Justification"})
                            .toInt();
    if (justification > 2)
        out.notes << "Full justification was converted to left alignment.";
    if (sheet.toMap().value("FauxBold").toBool() || sheet.toMap().value("FauxItalic").toBool())
        out.notes << "Faux bold or italic was omitted.";
    for (int i = 1; i < runs.size(); ++i)
        if (walk(runs[i], {"StyleSheet", "StyleSheetData"}) != sheet) {
            out.notes << "Only the first text style was retained.";
            break;
        }
    out.text = {
        {"content", content},
        {"fontName", font},
        {"fontSize", fontSize},
        {"red", channel(red)},
        {"green", channel(green)},
        {"blue", channel(blue)},
        {"tracking",
         std::clamp(sheet.toMap().value("Tracking").toDouble() * fontSize / 1000, -100.0, 1000.0)},
        {"leading",
         sheet.toMap().value("AutoLeading", true).toBool()
             ? 0
             : std::clamp(sheet.toMap().value("Leading").toDouble() * scale, 0.0, 5000.0)},
        {"alignment", justification == 1   ? "Right"
                      : justification == 2 ? "Center"
                                           : "Left"}};
    auto bounds = rectangle(description.value("bounds").toMap()),
         glyph = rectangle(description.value("boundingBox").toMap());
    if (bounds.width() > glyph.width() + 4 && bounds.height() > glyph.height() + 4 &&
        glyph.isValid()) {
        double w = bounds.width() * scale + 24, h = bounds.height() * scale + 24;
        require(w >= 24 && h >= 24 && w <= MaxSide && h <= MaxSide && w * h <= MaxSurfacePixels,
                "Photoshop paragraph bounds exceed limit");
        out.text["boxSize"] = QJsonArray{w, h};
        out.textAnchor = {xx * bounds.x() + xy * bounds.y() + tx,
                          yx * bounds.x() + yy * bounds.y() + ty};
        out.textFrame = true;
    }
    out.notes << "Editable text is rendered with Windows fonts; appearance may differ.";
}
QVariantMap descriptor(QByteArrayView data, bool header = false) {
    require(data.size() <= 8000000, "Photoshop vector data exceeds limit");
    BinaryReader r(data);
    if (header)
        require(r.u32() == 1, "Unsupported Photoshop vector origin version");
    return Descriptor(r).read();
}
void vector(PhotoshopEditable &out, const QHash<QByteArray, QByteArrayView> &extra, QSize canvas) {
    if (!extra.contains("SoCo") || !extra.contains("vogk"))
        return;
    auto fill = descriptor(extra.value("SoCo")), origin = descriptor(extra.value("vogk"), true);
    auto type = find(origin, "keyOriginType").toInt();
    require(type == 1 || type == 2 || type == 5, "Unsupported live Photoshop shape");
    auto bounds = rectangle(find(origin, "keyOriginShapeBBox").toMap());
    require(bounds.width() >= 1 && bounds.height() >= 1 && std::abs(bounds.x()) <= 1000000 &&
                std::abs(bounds.y()) <= 1000000 && bounds.width() <= MaxSide &&
                bounds.height() <= MaxSide,
            "Photoshop shape bounds exceed limit");
    double radius = 0;
    auto radii = find(origin, "keyOriginRRectRadii").toMap();
    if (!radii.isEmpty()) {
        double low = 1e9, high = 0;
        for (auto key : {"topLeft", "topRight", "bottomRight", "bottomLeft"}) {
            require(radii.contains(key), "Incomplete Photoshop corner radii");
            double v = radii.value(key).toDouble();
            low = std::min(low, v);
            high = std::max(high, v);
        }
        require(high - low <= .5 && low >= 0, "Unequal Photoshop corner radii are unsupported");
        radius = high;
    }
    if (extra.contains("vstk")) {
        auto stroke = descriptor(extra.value("vstk"));
        require(find(stroke, "fillEnabled").toBool(), "Photoshop shape fill is disabled");
        if (find(stroke, "strokeEnabled").toBool())
            out.notes << "Photoshop shape stroke was omitted from the editable shape.";
    }
    auto rgb = find(fill, "Clr ").toMap();
    if (rgb.isEmpty())
        rgb = fill;
    auto color = [&](const QString &key) {
        auto value = find(rgb, key);
        require(value.isValid(), "Unsupported Photoshop shape fill");
        double v = value.toDouble();
        return std::clamp(v > 1 ? v / 255 : v, 0.0, 1.0);
    };
    out.shape = {{"kind", type == 5 ? "Ellipse" : "Rectangle"},
                 {"cornerRadius", radius},
                 {"red", color("Rd  ")},
                 {"green", color("Grn ")},
                 {"blue", color("Bl  ")}};
    out.shapeBounds = bounds;
    out.notes << "Shape is editable; its saved raster appearance is retained until edited.";
    Q_UNUSED(canvas);
}
} // namespace
PhotoshopEditable photoshopEditable(const QHash<QByteArray, QByteArrayView> &extra, QSize canvas) {
    PhotoshopEditable out;
    if (extra.contains("TySh") || extra.contains("tySh")) {
        try {
            text(out, extra.value(extra.contains("TySh") ? "TySh" : "tySh"));
        } catch (const Error &) {
            out.text = {};
            out.notes << "Photoshop text was rasterized because its type settings are unsupported "
                         "or damaged.";
        }
    }
    if (extra.contains("vmsk") || extra.contains("vsms") || extra.contains("vogk")) {
        try {
            vector(out, extra, canvas);
        } catch (const Error &) {
            out.shape = {};
        }
        if (out.shape.isEmpty())
            out.notes << "Photoshop vector was rasterized; this shape is unsupported for editing.";
    }
    return out;
}
} // namespace compositor

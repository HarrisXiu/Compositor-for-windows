// SPDX-License-Identifier: MIT
#include "editable_layers.h"
#include "document.h"
#include <QAbstractTextDocumentLayout>
#include <QJsonArray>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <algorithm>
#include <cmath>
namespace compositor {
static double number(const QJsonObject &o, const char *key, double fallback = 0) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}
static QColor color(const QJsonObject &o) {
    return QColor::fromRgbF(std::clamp(number(o, "red"), 0.0, 1.0),
                            std::clamp(number(o, "green"), 0.0, 1.0),
                            std::clamp(number(o, "blue"), 0.0, 1.0));
}
void loadTextDocument(QTextDocument &document, const QJsonObject &style) {
    auto content = style.value("content").toString();
    require(content.size() <= 100000, "Text exceeds limit");
    auto size = number(style, "fontSize", 72);
    require(std::isfinite(size) && size >= 1 && size <= 2000, "Invalid text font size");
    QFont font(style.value("fontName").toString("Segoe UI"));
    font.setPixelSize(int(std::lround(size)));
    font.setLetterSpacing(QFont::AbsoluteSpacing, number(style, "tracking"));
    document.setDefaultFont(font);
    document.setDocumentMargin(12);
    document.setPlainText(content);
    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setFont(font);
    format.setForeground(color(style));
    cursor.mergeCharFormat(format);
    QTextBlockFormat block;
    auto alignment = style.value("alignment").toString("Left");
    block.setAlignment(alignment == "Center"  ? Qt::AlignHCenter
                       : alignment == "Right" ? Qt::AlignRight
                                              : Qt::AlignLeft);
    block.setLineHeight(number(style, "leading") > 0 ? number(style, "leading") : size * 1.2,
                        QTextBlockFormat::FixedHeight);
    cursor.mergeBlockFormat(block);
    for (const auto &value : style.value("colorRuns").toArray()) {
        auto run = value.toObject();
        int start = run.value("location").toInt(), length = run.value("length").toInt();
        require(start >= 0 && length > 0 && qint64(start) + length <= content.size(),
                "Invalid text color run");
        cursor.setPosition(start);
        cursor.setPosition(start + length, QTextCursor::KeepAnchor);
        QTextCharFormat f;
        f.setForeground(color(run));
        cursor.mergeCharFormat(f);
    }
    for (const auto &value : style.value("fontRuns").toArray()) {
        auto run = value.toObject();
        int start = run.value("location").toInt(), length = run.value("length").toInt();
        require(start >= 0 && length > 0 && qint64(start) + length <= content.size(),
                "Invalid text font run");
        cursor.setPosition(start);
        cursor.setPosition(start + length, QTextCursor::KeepAnchor);
        QTextCharFormat f;
        auto face = font;
        face.setFamily(run.value("fontName").toString());
        f.setFont(face);
        cursor.mergeCharFormat(f);
    }
    auto box = style.value("boxSize").toArray();
    if (box.size() == 2)
        document.setTextWidth(box[0].toDouble());
    else
        document.setTextWidth(-1);
}
QJsonObject textStyleFromDocument(const QTextDocument &document, const QJsonObject &base) {
    auto out = base;
    auto content = document.toPlainText();
    out["content"] = content;
    QJsonArray colors, fonts;
    auto baseColor = color(base);
    auto baseFont = base.value("fontName").toString();
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            auto format = fragment.charFormat();
            int start = fragment.position(), length = fragment.length();
            auto c = format.foreground().color();
            if (c.isValid() && c != baseColor)
                colors.append(QJsonObject{{"location", start},
                                          {"length", length},
                                          {"red", c.redF()},
                                          {"green", c.greenF()},
                                          {"blue", c.blueF()}});
            auto family = format.font().family();
            if (!family.isEmpty() && family != baseFont)
                fonts.append(
                    QJsonObject{{"location", start}, {"length", length}, {"fontName", family}});
        }
    if (colors.isEmpty())
        out.remove("colorRuns");
    else
        out["colorRuns"] = colors;
    if (fonts.isEmpty())
        out.remove("fontRuns");
    else
        out["fontRuns"] = fonts;
    return out;
}
QImage renderText(const QJsonObject &style) {
    QTextDocument document;
    loadTextDocument(document, style);
    auto box = style.value("boxSize").toArray();
    QSize size;
    if (box.size() == 2)
        size = QSize(int(std::ceil(box[0].toDouble())), int(std::ceil(box[1].toDouble())));
    else {
        size = document.size().toSize();
    }
    size = size.expandedTo({1, 1});
    require(size.width() <= MaxSide && size.height() <= MaxSide &&
                qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "Text box exceeds size limit");
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    require(!image.isNull(), "Not enough memory for text");
    image.fill(Qt::transparent);
    QPainter p(&image);
    document.drawContents(&p);
    return image;
}
QImage renderShape(const QJsonObject &style, QSize size) {
    require(size.width() > 0 && size.height() > 0 && size.width() <= MaxSide &&
                size.height() <= MaxSide &&
                qint64(size.width()) * size.height() <= MaxSurfacePixels,
            "Shape exceeds size limit");
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    require(!image.isNull(), "Not enough memory for shape");
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color(style));
    auto kind = style.value("kind").toString();
    QRectF rect(QPointF(), size);
    if (kind == "Ellipse")
        p.drawEllipse(rect);
    else if (kind == "Line") {
        auto start = style.value("start").toArray(), end = style.value("end").toArray();
        auto point = [&](const QJsonArray &a, QPointF fallback) {
            return a.size() == 2
                       ? QPointF(a[0].toDouble() * size.width(), a[1].toDouble() * size.height())
                       : fallback;
        };
        p.setPen(QPen(color(style), number(style, "lineWidth", 4), Qt::SolidLine, Qt::RoundCap));
        p.drawLine(point(start, {0, 0}), point(end, {double(size.width()), double(size.height())}));
    } else {
        double radius = std::clamp(number(style, "cornerRadius"), 0.0,
                                   std::min(size.width(), size.height()) / 2.0);
        p.drawRoundedRect(rect, radius, radius);
    }
    return image;
}
} // namespace compositor

// SPDX-License-Identifier: MIT
#include "demo.h"
#include <QFont>
#include <QPainter>
namespace compositor {
Document createDemoDocument() {
    auto d = Document::create({1000, 660});
    QImage background(d.size(), QImage::Format_RGBA8888_Premultiplied);
    QPainter painter(&background);
    QLinearGradient gradient(0, 0, 1000, 660);
    gradient.setColorAt(0, QColor("#18283e"));
    gradient.setColorAt(1, QColor("#090d18"));
    painter.fillRect(background.rect(), gradient);
    painter.end();
    d.addImage("Background", background);
    QImage grid(d.size(), QImage::Format_RGBA8888_Premultiplied);
    grid.fill(Qt::transparent);
    painter.begin(&grid);
    painter.setPen(QColor(150, 185, 220, 22));
    for (int x = 0; x < 1000; x += 40)
        painter.drawLine(x, 0, x, 660);
    for (int y = 0; y < 660; y += 40)
        painter.drawLine(0, y, 1000, y);
    painter.end();
    d.addImage("Layout Grid", grid);
    QImage light(540, 540, QImage::Format_RGBA8888_Premultiplied);
    light.fill(Qt::transparent);
    painter.begin(&light);
    painter.setRenderHint(QPainter::Antialiasing);
    QRadialGradient glow(270, 270, 260);
    glow.setColorAt(0, QColor(40, 195, 220, 180));
    glow.setColorAt(1, QColor(40, 195, 220, 0));
    painter.fillRect(light.rect(), glow);
    painter.end();
    d.addImage("Teal Light", light);
    d.active()->move({520, 60});
    d.active()->metadata["blendMode"] = "Screen";
    d.active()->metadata["opacity"] = 0.7;
    QImage card(280, 280, QImage::Format_RGBA8888_Premultiplied);
    card.fill(Qt::transparent);
    painter.begin(&card);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#287ca7"));
    painter.setPen(QPen(QColor("#68d9e5"), 3));
    painter.drawRoundedRect(QRectF(25, 25, 230, 230), 32, 32);
    painter.setBrush(QColor("#142b43"));
    painter.setPen(QPen(QColor("#e2f6ff"), 4));
    painter.drawRoundedRect(QRectF(70, 70, 150, 150), 22, 22);
    painter.setBrush(QColor("#c5eaf6"));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(106, 106, 78, 78));
    painter.end();
    d.addImage("Layer Artwork", card);
    d.active()->move({650, 120});
    auto transform = d.active()->transform();
    transform["rotation"] = -12;
    d.active()->metadata["transform"] = transform;
    d.active()->metadata["effects"] = QJsonObject{
        {"shadow", QJsonObject{{"angle", 90},
                               {"distance", 15},
                               {"blur", 18},
                               {"red", 0},
                               {"green", 0},
                               {"blue", 0},
                               {"opacity", .7}}},
        {"outerGlow",
         QJsonObject{{"size", 12}, {"red", .2}, {"green", .8}, {"blue", 1}, {"opacity", .4}}}};
    QImage typography(620, 320, QImage::Format_RGBA8888_Premultiplied);
    typography.fill(Qt::transparent);
    painter.begin(&typography);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font("Segoe UI");
    font.setPixelSize(18);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 3);
    painter.setFont(font);
    painter.setPen(QColor("#75cfdf"));
    painter.drawText(0, 28, "NATIVE IMAGE EDITING");
    font.setLetterSpacing(QFont::AbsoluteSpacing, -1);
    font.setPixelSize(74);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor("#edf4fc"));
    painter.drawText(0, 140, "Compositor");
    font.setPixelSize(40);
    font.setBold(false);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    painter.setFont(font);
    painter.setPen(QColor("#adc4de"));
    painter.drawText(0, 198, "For Windows");
    font.setPixelSize(18);
    painter.setFont(font);
    painter.setPen(QColor("#7990a8"));
    painter.drawText(0, 260, "C++20  /  Qt 6  /  Original C algorithms");
    painter.end();
    d.addImage("Typography", typography);
    d.active()->move({70, 110});
    d.validateAssets();
    return d;
}
} // namespace compositor

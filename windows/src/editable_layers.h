// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QTextDocument>
namespace compositor {
void loadTextDocument(QTextDocument &document, const QJsonObject &style);
QJsonObject textStyleFromDocument(const QTextDocument &document, const QJsonObject &base);
QImage renderText(const QJsonObject &style);
// Where renderText draws the first line's baseline, in the image's pixels from its top.
double textFirstBaseline(const QJsonObject &style);
QImage renderShape(const QJsonObject &style, QSize size);
} // namespace compositor

// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QTextDocument>
namespace compositor {
void loadTextDocument(QTextDocument &document, const QJsonObject &style);
QJsonObject textStyleFromDocument(const QTextDocument &document, const QJsonObject &base);
QImage renderText(const QJsonObject &style);
QImage renderShape(const QJsonObject &style, QSize size);
} // namespace compositor

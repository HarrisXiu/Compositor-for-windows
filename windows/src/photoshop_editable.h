// SPDX-License-Identifier: MIT
#pragma once
#include <QByteArrayView>
#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QStringList>
namespace compositor {
struct PhotoshopEditable {
    QJsonObject text, shape;
    QJsonObject effects;
    QPainterPath vectorPath;
    QColor vectorFill, vectorStroke;
    double vectorStrokeWidth = 0;
    bool vectorEnabled = false, vectorInverted = false;
    QStringList notes;
    QRectF shapeBounds;
    QPointF textAnchor;
    double textRotation = 0;
    bool textFlipY = false, textFrame = false;
};
PhotoshopEditable photoshopEditable(const QHash<QByteArray, QByteArrayView> &extra, QSize canvas);
} // namespace compositor

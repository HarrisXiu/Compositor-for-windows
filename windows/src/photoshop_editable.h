// SPDX-License-Identifier: MIT
#pragma once
#include <QByteArrayView>
#include <QHash>
#include <QJsonObject>
#include <QRectF>
#include <QSize>
#include <QStringList>
namespace compositor {
struct PhotoshopEditable {
    QJsonObject text, shape;
    QStringList notes;
    QRectF shapeBounds;
    QPointF textAnchor;
    double textRotation = 0;
    bool textFlipY = false, textFrame = false;
};
PhotoshopEditable photoshopEditable(const QHash<QByteArray, QByteArrayView> &extra, QSize canvas);
} // namespace compositor

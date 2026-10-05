// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QRectF>
#include <QString>

namespace compositor {
struct Layer;
QImage applyFilter(const QImage &source, const QString &kind, const QJsonObject &settings = {});
QImage gaussianBlur(const QImage &source, double sigma, bool clampEdges = false);
QImage applyAdjustment(const QImage &source, const QJsonObject &adjustment);
QJsonObject adjustmentSettings(const QJsonObject &adjustment);
QJsonObject makeAdjustment(const QString &kind, const QJsonObject &settings);
QImage limitToSelection(const QImage &original, const QImage &filtered, const QImage &coverage);
// How far past the pixels it reads a filter's result reaches, in layer pixels: the blurs spread
// outwards (about three standard deviations, or half a streak); other filters stay put.
double filterMargin(const QString &kind, const QJsonObject &settings);
// Pads a pixel layer so a filter can reach past its edge: by the filter's margin on every side, and
// over `documentArea` (Content-Aware Fill's selection) when given. The pixels keep their place on the
// document; an implicit mask grows revealed. Returns whether the layer grew.
bool growForFilter(Layer &layer, const QString &kind, const QJsonObject &settings,
                   const QRectF &documentArea = {});
} // namespace compositor

// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
namespace compositor {
double curveValue(double x, const QJsonArray &points);
void applyCameraRawColor(QImage &image, const QJsonObject &settings);
QImage cameraRawGeometry(const QImage &image, const QJsonObject &settings);
QJsonObject cameraRawWhiteBalance(const QImage &image);
void cameraRawPreviewOverlay(QImage &image, const QJsonObject &settings);
} // namespace compositor

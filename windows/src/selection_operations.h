// SPDX-License-Identifier: MIT
#pragma once
#include <QColor>
#include <QImage>
#include <QVector>
#include <atomic>
namespace compositor {
QImage resizeSelectionMask(const QImage &mask, int radius, bool expand,
                           const std::atomic<bool> *canceled = nullptr);
QImage colorRangeMask(const QImage &image, const QVector<QColor> &include,
                     const QVector<QColor> &exclude, int fuzziness, bool invert);
}

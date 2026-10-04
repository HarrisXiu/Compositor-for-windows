// SPDX-License-Identifier: MIT
#include "selection_operations.h"
#include "document.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>
extern "C" {
#include "WandPixels.h"
}
namespace compositor {
// Disk dilation/erosion. Sliding row extrema retain feathered coverage and avoid a
// quadratic scan of each pixel's neighborhood. Outside the canvas has zero coverage.
QImage resizeSelectionMask(const QImage &mask, int radius, bool expand,
                           const std::atomic<bool> *canceled) {
    require(mask.format() == QImage::Format_Grayscale8, "Expected a selection mask");
    require(radius >= 0 && radius <= 200, "Selection radius must be between 0 and 200 pixels");
    if (!radius)
        return mask;
    QImage out(mask.size(), QImage::Format_Grayscale8);
    require(!out.isNull(), "Not enough memory for selection");
    out.fill(expand ? 0 : 255);
    const int w = mask.width(), h = mask.height();
    for (int dy = -radius; dy <= radius; ++dy) {
        const int reach = int(std::floor(std::sqrt(double(radius * radius - dy * dy))));
        for (int y = 0; y < h; ++y) {
            if (canceled && canceled->load(std::memory_order_relaxed))
                return {};
            auto target = out.scanLine(y);
            if (y + dy < 0 || y + dy >= h) {
                if (!expand)
                    std::fill_n(target, w, uchar(0));
                continue;
            }
            auto source = mask.constScanLine(y + dy);
            std::deque<int> queue;
            int next = 0;
            for (int x = 0; x < w; ++x) {
                while (next < w && next <= x + reach) {
                    while (!queue.empty() && (expand ? source[queue.back()] <= source[next]
                                                    : source[queue.back()] >= source[next]))
                        queue.pop_back();
                    queue.push_back(next++);
                }
                while (!queue.empty() && queue.front() < x - reach)
                    queue.pop_front();
                const uchar value = !expand && (x < reach || x + reach >= w)
                                        ? 0 : source[queue.front()];
                target[x] = expand ? std::max(target[x], value) : std::min(target[x], value);
            }
        }
    }
    return out;
}
QImage colorRangeMask(const QImage &image, const QVector<QColor> &include,
                     const QVector<QColor> &exclude, int fuzziness, bool invert) {
    require(!image.isNull(), "No image to sample");
    require(fuzziness >= 0 && fuzziness <= 200, "Fuzziness must be between 0 and 200");
    auto rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage mask(image.size(), QImage::Format_Grayscale8);
    require(!rgba.isNull() && !mask.isNull(), "Not enough memory for selection");
    auto pack = [](const QVector<QColor> &colors) {
        std::vector<uchar> result;
        for (const auto &color : colors) {
            result.push_back(uchar(color.red()));
            result.push_back(uchar(color.green()));
            result.push_back(uchar(color.blue()));
        }
        return result;
    };
    const auto in = pack(include), ex = pack(exclude);
    std::vector<uchar> packed(size_t(mask.width()) * mask.height());
    color_range_mask(rgba.constBits(), size_t(rgba.width()), size_t(rgba.height()),
                     size_t(rgba.bytesPerLine()), in.data(), int(include.size()), ex.data(),
                     int(exclude.size()), fuzziness, invert, packed.data());
    for (int y = 0; y < mask.height(); ++y)
        std::copy_n(packed.data() + size_t(y) * mask.width(), mask.width(), mask.scanLine(y));
    return mask;
}
}

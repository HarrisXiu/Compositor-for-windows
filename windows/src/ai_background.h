// SPDX-License-Identifier: MIT
#pragma once
#include "ai_selection.h"
#include "document.h"
namespace compositor {
struct BackgroundSettings {
    bool advanced = false;
    int refine = 12, contrast = 25, shift = 0;
};
QImage refineBackgroundMatte(const QImage &mask, const QImage &guide, BackgroundSettings settings,
                             std::shared_ptr<AiCancellation> cancellation = {});
QImage backgroundLayerMask(const Document &document, const Layer &layer, const QImage &matte,
                           const QImage &selection = {},
                           std::shared_ptr<AiCancellation> cancellation = {});
QImage backgroundCutout(const QImage &image, const QImage &mask,
                        std::shared_ptr<AiCancellation> cancellation = {});
void installBackgroundMask(Layer &layer, const QImage &mask);
} // namespace compositor

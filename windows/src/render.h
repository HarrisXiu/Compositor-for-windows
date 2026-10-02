// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
namespace compositor {
QImage renderDocument(const Document &document, QSize outputSize = {});
QImage layerSelection(const Document &document, const Layer &layer, const QImage &selection);
} // namespace compositor

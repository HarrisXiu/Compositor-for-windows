// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QSet>
#include <optional>

namespace compositor {
QStringList layerRoots(const Document &document, const QSet<QString> &selected);
QSet<QString> layerSubtrees(const Document &document, const QSet<QString> &selected);
QString mergeLabel(const Document &document, const QSet<QString> &selected);
QString mergeLayers(Document &document, const QSet<QString> &selected);
QString groupLayers(Document &document, const QSet<QString> &selected);
QStringList ungroupLayer(Document &document, const QString &id);
void moveLayers(Document &document, const QSet<QString> &selected, int direction);
void moveLayersOut(Document &document, const QSet<QString> &selected);
void moveLayersTo(Document &document, const QSet<QString> &selected, const QString &parent,
                  const QString &anchor = {}, bool above = false);
QStringList copyLayers(Document &destination, const Document &source, const QSet<QString> &selected,
                       std::optional<QPointF> center = std::nullopt, bool duplicate = false);
QImage layerAlphaSelection(const Document &document, const Layer &layer, bool mask);
} // namespace compositor

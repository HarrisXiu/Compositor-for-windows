// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QMimeData>
#include <QSet>
#include <memory>
namespace compositor {
struct LayerTransfer {
    Document source;
    QSet<QString> selected;
};
class LayerTransferMimeData final : public QMimeData {
  public:
    LayerTransferMimeData(const Document &source, const QSet<QString> &selected);
    ~LayerTransferMimeData() override;

  private:
    QString token_;
    std::shared_ptr<const LayerTransfer> transfer_;
};
std::shared_ptr<const LayerTransfer> layerTransfer(const QMimeData *mime);
} // namespace compositor

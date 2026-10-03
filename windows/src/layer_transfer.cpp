// SPDX-License-Identifier: MIT
#include "layer_transfer.h"
#include <QHash>
namespace compositor {
namespace {
constexpr auto MimeType = "application/x-compositor-layer-transfer";
auto &transfers() {
    // Clipboard data may outlive static destructors during QApplication shutdown.
    static auto *registry = new QHash<QString, std::weak_ptr<const LayerTransfer>>;
    return *registry;
}
} // namespace
LayerTransferMimeData::LayerTransferMimeData(const Document &source, const QSet<QString> &selected)
    : token_(newId()), transfer_(std::make_shared<LayerTransfer>(LayerTransfer{source, selected})) {
    transfers()[token_] = transfer_;
    setData(MimeType, token_.toUtf8());
}
LayerTransferMimeData::~LayerTransferMimeData() {
    transfers().remove(token_);
}
std::shared_ptr<const LayerTransfer> layerTransfer(const QMimeData *mime) {
    if (!mime || !mime->hasFormat(MimeType))
        return {};
    return transfers().value(QString::fromUtf8(mime->data(MimeType))).lock();
}
} // namespace compositor

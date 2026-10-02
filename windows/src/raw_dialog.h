// SPDX-License-Identifier: MIT
#pragma once
#include "raw_import.h"
#include <QDialog>
class QLabel;
class QDoubleSpinBox;
class QDialogButtonBox;
class QTimer;
namespace compositor {
class RawDevelopDialog : public QDialog {
    Q_OBJECT
  public:
    explicit RawDevelopDialog(const QString &path, QWidget *parent = nullptr);
    ~RawDevelopDialog();
    QImage importedImage() const {
        return imported_;
    }

  private:
    std::shared_ptr<RawSource> source_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    QLabel *preview_, *status_;
    QDoubleSpinBox *exposure_, *temperature_, *tint_, *boost_;
    QDialogButtonBox *buttons_;
    QTimer *timer_;
    QImage imported_;
    bool busy_ = true, final_ = false, failed_ = false;
    quint64 revision_ = 0;
    RawSettings settings() const;
    void schedule();
    void render();
    void showPreview(const QImage &image);
    void fail(const QString &message);
};
} // namespace compositor

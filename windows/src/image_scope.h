// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QWidget>
#include <array>
namespace compositor {
class ImageScope final : public QWidget {
  public:
    explicit ImageScope(QWidget *parent = nullptr);
    int mode = 0;
    std::array<std::array<double, 256>, 3> histograms{};
    std::array<double, 4096> vectors{};
    void setImage(const QImage &image);

  protected:
    void paintEvent(QPaintEvent *) override;
};
} // namespace compositor

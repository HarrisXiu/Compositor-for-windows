#pragma once
#include "ai_download.h"
#include <QDialog>
#include <QVector>
class QLabel;
class QProgressBar;
class QPushButton;

namespace compositor {
class AiModelsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit AiModelsDialog(QWidget *parent = nullptr, QString cacheRoot = {});
    ~AiModelsDialog() override;
    void reject() override;
  private:
    struct Row {
        QLabel *status = nullptr;
        QProgressBar *progress = nullptr;
        QPushButton *download = nullptr, *restart = nullptr, *import = nullptr;
    };
    void refresh(int preserveStatus = -1);
    void enableActions(bool enabled);
    void importModel(int row);
    void downloadModel(int row, bool restart);
    void nextAsset();
    void endOperation(const QString &message, bool installed = false);
    QString root_;
    QVector<Row> rows_;
    QPushButton *cancel_ = nullptr;
    AiModelDownloader *downloader_;
    std::shared_ptr<AiCancellation> checking_, operation_;
    int current_ = -1, asset_ = 0;
    bool restart_ = false;
};
}

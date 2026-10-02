// SPDX-License-Identifier: MIT
#pragma once
#include <QObject>
#include <QString>
class QComboBox;
namespace compositor {
class UiLanguage final : public QObject {
    Q_OBJECT
  public:
    static UiLanguage &instance();
    void initialize();
    QString code() const {
        return code_;
    }
    void setLanguage(const QString &code, bool persist = true);
    QString text(const QString &source) const;
    void translateObject(QObject *object, bool children = false);
  signals:
    void languageChanged();

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    UiLanguage();
    QString code_ = "en";
    bool changing_ = false;
};
QString uiText(const QString &source);
QString comboValue(const QComboBox *combo);
void selectComboValue(QComboBox *combo, const QString &source);
} // namespace compositor

// SPDX-License-Identifier: MIT
#pragma once
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QVector>
class QAction;
class QWidget;
namespace compositor {
struct ShortcutEntry {
    QString id, group, title;
    QKeySequence original;
};
class Shortcuts : public QObject {
    Q_OBJECT
  public:
    static Shortcuts &instance();
    const QVector<ShortcutEntry> &entries() const {
        return entries_;
    }
    void registerAction(QAction *action, const QString &id, const QString &group);
    QKeySequence sequence(const ShortcutEntry &entry) const;
    QKeyCombination canvasKey(QKeyCombination input) const;
    QString validate(const QMap<QString, QKeySequence> &values) const;
    bool save(const QMap<QString, QKeySequence> &values);
    void validateLoaded();
    void showDialog(QWidget *parent);
  signals:
    void changed();

  private:
    Shortcuts();
    QVector<ShortcutEntry> entries_;
    QMap<QString, QKeySequence> overrides_;
    QVector<QPair<QString, QPointer<QAction>>> actions_;
};
} // namespace compositor

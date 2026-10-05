// SPDX-License-Identifier: MIT
#pragma once
#include "document.h"
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QLockFile>
#include <QObject>
#include <QTimer>
#include <memory>

namespace compositor {
// Hash referenced assets as well as the manifest: equal-size PNG replacements are changes too.
QByteArray projectFingerprint(const QString &path);
class ProjectMonitor final : public QObject {
    Q_OBJECT
  public:
    explicit ProjectMonitor(QObject *parent = nullptr);
    void setPath(const QString &path, const QByteArray &savedFingerprint = {});
    void acknowledge();
    void setPaused(bool paused);
    QByteArray fingerprint() const {
        return accepted_;
    }
  signals:
    void projectReady(const compositor::Document &document, const QByteArray &fingerprint);
    void reloadFailed(const QString &message);

  private:
    void watch();
    void check(bool force = true);
    QString path_;
    QFileSystemWatcher watcher_;
    QTimer debounce_, poll_;
    QByteArray accepted_, observed_;
    QByteArray stamp_;
    QElapsedTimer audit_;
    bool paused_ = false, loading_ = false;
    int attempts_ = 0;
};

struct RecoveryEntry {
    QString project, source, title;
};
// Each process holds a lock for its recovery folder. Other running instances are never restored
// or cleaned, and crash snapshots are kept until the user recovers or discards them.
class RecoveryStore {
  public:
    explicit RecoveryStore(QString root);
    ~RecoveryStore();
    QString sessionPath() const {
        return session_;
    }
    void write(const QString &id, const Document &document, const QString &source,
               const QString &title);
    void remove(const QString &id);
    QList<RecoveryEntry> available() const;
    void discard(const RecoveryEntry &entry);
    static void cleanStaging(const QString &parent);

  private:
    QString root_, session_;
    std::unique_ptr<QLockFile> lock_;
};
QStringList recentFiles();
void rememberFile(const QString &path);
} // namespace compositor

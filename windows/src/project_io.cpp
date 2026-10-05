// SPDX-License-Identifier: MIT
#include "project_io.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace compositor {
static QByteArray projectStamp(const QString &path) {
    QByteArray stamp;
    auto append = [&](const QFileInfo &file) {
        stamp += file.fileName().toUtf8() + ':' + QByteArray::number(file.size()) + ':' +
                 QByteArray::number(file.lastModified().toMSecsSinceEpoch()) + ';';
    };
    append(QFileInfo(QDir(path).filePath("manifest.json")));
    for (const auto &file : QDir(QDir(path).filePath("images")).entryInfoList(QDir::Files))
        append(file);
    return stamp;
}
QByteArray projectFingerprint(const QString &path) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QFile manifest(QDir(path).filePath("manifest.json"));
    if (!manifest.open(QIODevice::ReadOnly))
        return "missing";
    require(manifest.size() <= 4 * 1024 * 1024, "Manifest exceeds 4 MiB");
    auto bytes = manifest.readAll();
    hash.addData(bytes);
    auto json = QJsonDocument::fromJson(bytes).object();
    QStringList names;
    for (const auto &v : json.value("layers").toArray()) {
        auto layer = v.toObject();
        for (auto key : {"imageFile", "maskFile"}) {
            auto name = layer.value(QLatin1String(key)).toString();
            if (!name.isEmpty()) {
                require(QFileInfo(name).fileName() == name && !name.contains('\\'),
                        "Invalid project asset name");
                names << name;
            }
        }
    }
    names.removeDuplicates();
    names.sort();
    const auto root = QFileInfo(path).canonicalFilePath() + '/';
    for (const auto &name : names) {
        hash.addData(name.toUtf8());
        QFile file(QDir(path).filePath("images/" + name));
        auto canonical = QFileInfo(file).canonicalFilePath();
        require(!canonical.isEmpty() && canonical.startsWith(root, Qt::CaseInsensitive),
                "Project asset is missing or outside the project");
        // Projects with larger assets cannot be opened again, so they are not written either.
        require(file.size() <= 512LL * 1024 * 1024,
                "Layer image " + name + " exceeds the 512 MiB file limit");
        require(file.open(QIODevice::ReadOnly) && hash.addData(&file),
                "Cannot read project asset " + name);
    }
    return hash.result();
}
qint64 ProjectMonitor::auditInterval(qint64 hashMilliseconds) {
    return std::clamp<qint64>(hashMilliseconds * 50, 3000, 120000);
}
ProjectMonitor::ProjectMonitor(QObject *parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(350);
    poll_.setInterval(350);
    auto schedule = [this] {
        attempts_ = 0;
        debounce_.start();
    };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, schedule);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, schedule);
    connect(&debounce_, &QTimer::timeout, this, [this] { check(); });
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!debounce_.isActive())
            check(false);
    });
}
void ProjectMonitor::setPath(const QString &path, const QByteArray &savedFingerprint) {
    path_ = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    acknowledge();
    if (!savedFingerprint.isEmpty()) {
        accepted_ = savedFingerprint;
        observed_ = accepted_;
        debounce_.start();
    }
    if (path_.isEmpty())
        poll_.stop();
    else
        poll_.start();
}
void ProjectMonitor::acknowledge() {
    try {
        accepted_ = path_.isEmpty() ? QByteArray() : projectFingerprint(path_);
    } catch (const Error &) {
        accepted_.clear();
    }
    observed_ = accepted_;
    stamp_ = projectStamp(path_);
    audit_.start();
    watch();
}
void ProjectMonitor::setPaused(bool paused) {
    paused_ = paused;
    if (!paused)
        debounce_.start();
}
void ProjectMonitor::watch() {
    auto files = watcher_.files(), dirs = watcher_.directories();
    if (!files.isEmpty())
        watcher_.removePaths(files);
    if (!dirs.isEmpty())
        watcher_.removePaths(dirs);
    if (path_.isEmpty())
        return;
    // Qt's Windows directory watches prevent renaming the watched package during atomic saves.
    // Watch its parent for replacement, and poll package metadata without holding handles open.
    QStringList paths{QFileInfo(path_).absolutePath()};
#ifndef Q_OS_WIN
    paths << QStringList{path_, QDir(path_).filePath("images"),
                         QDir(path_).filePath("manifest.json")};
    QDir images(QDir(path_).filePath("images"));
    for (const auto &name : images.entryList(QDir::Files))
        paths << images.filePath(name);
#endif
    for (const auto &path : paths)
        if (QFileInfo::exists(path))
            watcher_.addPath(path);
}
void ProjectMonitor::check(bool force) {
    if (path_.isEmpty() || paused_ || loading_)
        return;
    const auto stamp = projectStamp(path_);
    if (!force && stamp == stamp_ && audit_.isValid() && audit_.elapsed() < auditInterval_)
        return;
    stamp_ = stamp;
    audit_.restart();
    loading_ = true;
    struct Result {
        Document document;
        QByteArray digest;
        QString error;
        qint64 hashMilliseconds = 0;
    };
    const auto path = path_;
    const auto accepted = accepted_;
    auto future = new QFutureWatcher<Result>(this);
    connect(future, &QFutureWatcher<Result>::finished, this, [this, future, path] {
        auto result = future->result();
        future->deleteLater();
        loading_ = false;
        if (path != path_ || paused_)
            return;
        watch();
        if (!result.error.isEmpty()) {
            if (++attempts_ <= 8)
                debounce_.start();
            else
                emit reloadFailed(result.error);
            return;
        }
        attempts_ = 0;
        auditInterval_ = auditInterval(result.hashMilliseconds);
        if (result.digest == accepted_) {
            observed_ = result.digest;
            return;
        }
        // Wait for two stable observations so manifest/asset write bursts arrive as one reload.
        if (observed_ != result.digest) {
            observed_ = result.digest;
            debounce_.start();
            return;
        }
        accepted_ = result.digest;
        emit projectReady(result.document, result.digest);
    });
    future->setFuture(QtConcurrent::run([path, accepted] {
        Result result;
        try {
            QElapsedTimer timer;
            timer.start();
            result.digest = projectFingerprint(path);
            result.hashMilliseconds = timer.elapsed();
            if (result.digest != accepted) {
                result.document = loadProject(path);
                require(result.digest == projectFingerprint(path),
                        "Project is still being written");
            }
        } catch (const std::exception &e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
}
RecoveryStore::RecoveryStore(QString root) : root_(QDir(root).absolutePath()) {
    require(QDir().mkpath(root_), "Cannot create recovery folder");
    session_ = QDir(root_).filePath(newId());
    require(QDir().mkpath(session_), "Cannot create recovery session");
    lock_ = std::make_unique<QLockFile>(QDir(session_).filePath("session.lock"));
    lock_->setStaleLockTime(0);
    require(lock_->tryLock(), "Cannot lock recovery session");
}
RecoveryStore::~RecoveryStore() {
    lock_->unlock();
    if (QDir(session_).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
        QDir(root_).rmdir(QFileInfo(session_).fileName());
}
void RecoveryStore::write(const QString &id, const Document &document, const QString &source,
                          const QString &title) {
    require(normalizedId(id) == id, "Invalid recovery ID");
    saveProject(document, QDir(session_).filePath(id + ".comp"));
    QSaveFile info(QDir(session_).filePath(id + ".json"));
    auto bytes = QJsonDocument(QJsonObject{{"source", source}, {"title", title}}).toJson();
    require(info.open(QIODevice::WriteOnly) && info.write(bytes) == bytes.size() && info.commit(),
            "Cannot save recovery information");
}
void RecoveryStore::remove(const QString &id) {
    require(normalizedId(id) == id, "Invalid recovery ID");
    QDir(QDir(session_).filePath(id + ".comp")).removeRecursively();
    QFile::remove(QDir(session_).filePath(id + ".json"));
}
QList<RecoveryEntry> RecoveryStore::available() const {
    QList<RecoveryEntry> entries;
    for (const auto &dir : QDir(root_).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (dir == QFileInfo(session_).fileName())
            continue;
        QDir session(QDir(root_).filePath(dir));
        if (QFileInfo(session.path()).isSymLink())
            continue;
        QLockFile lock(session.filePath("session.lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock())
            continue;
        cleanStaging(session.path());
        for (const auto &project : session.entryList({"*.comp"}, QDir::Dirs)) {
            if (QFileInfo(session.filePath(project)).isSymLink())
                continue;
            QFile info(session.filePath(QFileInfo(project).completeBaseName() + ".json"));
            QJsonObject json;
            if (info.open(QIODevice::ReadOnly) && info.size() <= 65536)
                json = QJsonDocument::fromJson(info.readAll()).object();
            entries.append({session.filePath(project), json.value("source").toString(),
                            json.value("title").toString(project)});
        }
    }
    return entries;
}
void RecoveryStore::discard(const RecoveryEntry &entry) {
    auto file = QFileInfo(entry.project);
    auto parent = QFileInfo(file.absolutePath());
    require(parent.absolutePath() == root_ && !parent.isSymLink() && !file.isSymLink(),
            "Recovery path is outside the recovery folder");
    QLockFile lock(QDir(parent.filePath()).filePath("session.lock"));
    lock.setStaleLockTime(0);
    require(lock.tryLock(), "Recovery session is in use");
    QDir(file.filePath()).removeRecursively();
    QFile::remove(QDir(parent.filePath()).filePath(file.completeBaseName() + ".json"));
}
void RecoveryStore::cleanStaging(const QString &parent) {
    QDir dir(QDir(parent).absolutePath());
    for (const auto &name : dir.entryList({".compositor-stage-*"}, QDir::Dirs | QDir::Hidden)) {
        QFileInfo stage(dir.filePath(name));
        // Old incomplete stages can be removed; backup projects are never deleted automatically.
        auto suffix = name.mid(QString(".compositor-stage-").size());
        // A backup with the same suffix means saving stopped between renaming the project to its
        // backup and installing the stage, so the stage may hold the only copy of the latest save.
        if (dir.exists(".compositor-backup-" + suffix))
            continue;
        if (!stage.isSymLink() && normalizedId(suffix) == suffix &&
            stage.lastModified().secsTo(QDateTime::currentDateTime()) > 86400) {
            QLockFile lock(QDir(stage.filePath()).filePath("stage.lock"));
            lock.setStaleLockTime(0);
            if (lock.tryLock()) {
                lock.unlock();
                QDir(stage.filePath()).removeRecursively();
            }
        }
    }
}
QStringList recentFiles() {
    return QSettings().value("files/recent").toStringList();
}
void rememberFile(const QString &path) {
    auto absolute = QFileInfo(path).absoluteFilePath();
    auto files = recentFiles();
    files.removeAll(absolute);
    files.prepend(absolute);
    while (files.size() > 20)
        files.removeLast();
    QSettings().setValue("files/recent", files);
}
} // namespace compositor

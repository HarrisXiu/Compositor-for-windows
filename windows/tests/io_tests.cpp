// SPDX-License-Identifier: MIT
#include "editor.h"
#include "photoshop.h"
#include "project_io.h"
#include "render.h"
#include "wic_import.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <chrono>
#include <cstdio>
#include <filesystem>
using namespace compositor;
class IoTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
    static Document photo(QColor color = Qt::red) {
        auto d = Document::create({32, 24});
        QImage image(d.size(), QImage::Format_RGBA8888_Premultiplied);
        image.fill(color);
        d.addImage("Photo", image);
        return d;
    }
    static EditorPage *current(EditorWindow &w) {
        return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget());
    }
    static std::unique_ptr<EditorWindow> makeWindow() {
        try {
            return std::make_unique<EditorWindow>();
        } catch (const std::exception &e) {
            qWarning("EditorWindow construction: %s", e.what());
            throw;
        }
    }
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qApp->setOrganizationName("IoRegression");
        qApp->setApplicationName("IoTests");
    }
    void fingerprintAndSaveConflict() {
        QTemporaryDir dir;
        auto path = dir.filePath("项目.comp");
        auto d = photo();
        saveProject(d, path);
        auto original = projectFingerprint(path);
        d.active()->image.fill(Qt::blue);
        try {
            saveProject(d, path);
        } catch (const std::exception &e) {
            qWarning("Save under monitor: %s", e.what());
            throw;
        }
        QVERIFY(projectFingerprint(path) != original);
        QVERIFY_EXCEPTION_THROWN(saveProject(photo(Qt::green), path, original), Error);
        QCOMPARE(loadProject(path).active()->image.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(
            QDir(dir.path()).entryList({".compositor-stage-*"}, QDir::Dirs | QDir::Hidden).size(),
            0);
    }
    void cleanTabReloadKeepsViewAndSelection() {
        try {
            QTemporaryDir dir;
            auto path = dir.filePath("Live.comp");
            auto d = photo();
            saveProject(d, path);
            auto instance = makeWindow();
            auto &window = *instance;
            window.openPath(path);
            auto p = current(window);
            QCoreApplication::processEvents();
            p->canvas->zoom = 3;
            p->canvas->selectAll();
            auto selection = p->session.selection;
            d.active()->image.fill(Qt::blue);
            saveProject(d, path);
            QTRY_COMPARE_WITH_TIMEOUT(p->document.active()->image.pixelColor(0, 0),
                                      QColor(Qt::blue), 8000);
            QCOMPARE(p->canvas->zoom, 3.0);
            QCOMPARE(p->session.selection, selection);
            QCOMPARE(p->history.count(), 0);
            QVERIFY(!p->isModified());
            window.openPath(path);
            QCOMPARE(window.findChild<QTabWidget *>()->count(), 2);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void imageOnlyWriteAndInvalidManifestRetry() {
        QTemporaryDir dir;
        auto path = dir.filePath("Live.comp");
        auto d = photo();
        saveProject(d, path);
        auto instance = makeWindow();
        auto &window = *instance;
        window.openPath(path);
        auto p = current(window);
        QFile file(QDir(path).filePath("manifest.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto manifest = file.readAll();
        file.close();
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{");
        file.close();
        QTest::qWait(1000);
        QCOMPARE(p->document.active()->image.pixelColor(0, 0), QColor(Qt::red));
        d.active()->image.fill(Qt::green);
        QVERIFY(d.active()->image.save(QDir(path).filePath("images/" + d.active()->id() + ".png"),
                                       "PNG"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(manifest);
        file.close();
        QTRY_COMPARE_WITH_TIMEOUT(p->document.active()->image.pixelColor(0, 0), QColor(Qt::green),
                                  8000);
    }
    void dirtyTabKeepsWorkUntilExplicitReload() {
        QTemporaryDir dir;
        auto path = dir.filePath("Conflict.comp");
        auto d = photo();
        saveProject(d, path);
        auto instance = makeWindow();
        auto &window = *instance;
        window.openPath(path);
        auto p = current(window);
        p->edit("Paint", [](Document &d) { d.active()->image.fill(Qt::yellow); });
        QTimer answer;
        connect(&answer, &QTimer::timeout, [] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->done(QMessageBox::No);
        });
        answer.start(50);
        d.active()->image.fill(Qt::blue);
        saveProject(d, path);
        QTRY_VERIFY_WITH_TIMEOUT(p->externalConflict, 8000);
        QTest::qWait(200);
        QCOMPARE(p->document.active()->image.pixelColor(0, 0), QColor(Qt::yellow));
        QCOMPARE(p->history.count(), 1);
        QVERIFY(p->isModified());
        p->history.undo();
        QVERIFY(!p->isModified());
        d.active()->image.fill(Qt::green);
        saveProject(d, path);
        QTRY_COMPARE_WITH_TIMEOUT(p->document.active()->image.pixelColor(0, 0), QColor(Qt::green),
                                  8000);
        QVERIFY(!p->externalConflict);
    }
    void recoveryLocksAndSourcePreservation() {
        QTemporaryDir dir;
        auto root = dir.filePath("recovery");
        auto source = dir.filePath("Source.comp");
        saveProject(photo(), source);
        auto id = newId();
        RecoveryStore viewer(root);
        {
            RecoveryStore running(root);
            running.write(id, photo(Qt::blue), source, "Working photo");
            QVERIFY(viewer.available().isEmpty());
        }
        auto entries = viewer.available();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().source, source);
        QCOMPARE(loadProject(entries.first().project).active()->image.pixelColor(0, 0),
                 QColor(Qt::blue));
        QCOMPARE(loadProject(source).active()->image.pixelColor(0, 0), QColor(Qt::red));
        viewer.discard(entries.first());
        QVERIFY(viewer.available().isEmpty());
        QVERIFY_EXCEPTION_THROWN(viewer.discard({source, {}, {}}), Error);
    }
    void automaticSnapshotsDoNotMarkContentSaved() {
        QTemporaryDir dir;
        QSettings().setValue("files/autosaveSeconds", 1);
        auto store = std::make_shared<RecoveryStore>(dir.path());
        RecoveryStore viewer(dir.path());
        {
            EditorWindow window(store);
            auto p = current(window);
            p->edit("Import", [](Document &d) {
                QImage image(8, 8, QImage::Format_RGBA8888_Premultiplied);
                image.fill(Qt::blue);
                d.addImage("Autosaved", image);
            });
            QTRY_VERIFY_WITH_TIMEOUT(
                QFileInfo::exists(
                    QDir(store->sessionPath()).filePath(p->recoveryId + ".comp/manifest.json")),
                8000);
            QTRY_VERIFY_WITH_TIMEOUT(!p->autosaving, 8000);
            QVERIFY(p->isModified());
            QVERIFY(p->path.isEmpty());
            QVERIFY(viewer.available().isEmpty());
        }
        store.reset();
        auto entries = viewer.available();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(loadProject(entries.first().project).layers.last().name(), QString("Autosaved"));
        viewer.discard(entries.first());
        QSettings().setValue("files/autosaveSeconds", 60);
    }
    void killedProcessRecovery() {
        QTemporaryDir dir;
        QProcess helper;
        helper.start(QCoreApplication::applicationFilePath(),
                     {"--recovery-crash-helper", dir.path()});
        QVERIFY(helper.waitForStarted(5000));
        QVERIFY(helper.waitForReadyRead(10000));
        QVERIFY(helper.readAllStandardOutput().contains("snapshot-ready"));
        helper.kill();
        QVERIFY(helper.waitForFinished(5000));
        RecoveryStore restarted(dir.path());
        auto entries = restarted.available();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(loadProject(entries.first().project).active()->image.pixelColor(0, 0),
                 QColor(Qt::blue));
        restarted.discard(entries.first());
    }
    void startupRecoveryOpensModifiedCopyAndPreservesSource() {
        QTemporaryDir dir;
        auto root = dir.filePath("recovery"), source = dir.filePath("original.comp");
        saveProject(photo(Qt::red), source);
        {
            RecoveryStore crashed(root);
            crashed.write(newId(), photo(Qt::blue), source, "Restored photo");
        }
        auto store = std::make_shared<RecoveryStore>(root);
        QTimer answer;
        connect(&answer, &QTimer::timeout, [] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->done(QMessageBox::Yes);
        });
        answer.start(50);
        EditorWindow window(store);
        QTRY_VERIFY_WITH_TIMEOUT(current(window)->isModified(), 8000);
        auto p = current(window);
        QVERIFY(p->path.isEmpty());
        QCOMPARE(p->document.active()->image.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(loadProject(source).active()->image.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(window.findChild<QTabWidget *>()->tabText(1).contains("Restored photo"));
        p->changed();
        QVERIFY(window.findChild<QTabWidget *>()->tabText(1).contains("Recovered"));
        QVERIFY(store->available().isEmpty());
        QCOMPARE(loadProject(QDir(store->sessionPath()).filePath(p->recoveryId + ".comp"))
                     .active()
                     ->image.pixelColor(0, 0),
                 QColor(Qt::blue));
    }
    void redoRecreatesDeletedRecoverySnapshot() {
        QTemporaryDir dir;
        QSettings().setValue("files/autosaveSeconds", 1);
        auto store = std::make_shared<RecoveryStore>(dir.path());
        EditorWindow window(store);
        auto p = current(window);
        p->edit("Import", [](Document &d) {
            QImage image(8, 8, QImage::Format_RGBA8888_Premultiplied);
            image.fill(Qt::blue);
            d.addImage("Redo snapshot", image);
        });
        const auto snapshot = QDir(store->sessionPath()).filePath(p->recoveryId + ".comp");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(snapshot) && !p->autosaving, 8000);
        p->history.undo();
        QVERIFY(!p->isModified());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(snapshot), 8000);
        p->history.redo();
        QVERIFY(p->isModified());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(snapshot) && !p->autosaving, 8000);
        QCOMPARE(loadProject(snapshot).active()->name(), QString("Redo snapshot"));
        QSettings().setValue("files/autosaveSeconds", 60);
    }
    void saveBaselineDoesNotHideLaterExternalWrites() {
        QTemporaryDir dir;
        const auto path = dir.filePath("race.comp");
        const auto saved = saveProject(photo(Qt::blue), path);
        QCOMPARE(saved, projectFingerprint(path));
        saveProject(photo(Qt::green), path);
        ProjectMonitor monitor;
        QSignalSpy reload(&monitor, &ProjectMonitor::projectReady);
        monitor.setPath(path, saved);
        QCOMPARE(monitor.fingerprint(), saved);
        QTRY_COMPARE_WITH_TIMEOUT(reload.count(), 1, 8000);
        QCOMPARE(qvariant_cast<Document>(reload.first().first()).active()->image.pixelColor(0, 0),
                 QColor(Qt::green));
    }
    void budgetEvictsOldestAndKeepsUndoRedoState() {
        EditorPage p(photo());
        p.history.setMemoryBudget(10000);
        for (int i = 0; i < 10; ++i)
            p.edit("Paint", [i](Document &d) { d.active()->image.fill(QColor(i * 20, 0, 0)); });
        QVERIFY(p.history.memoryUsage() <= p.history.memoryBudget());
        QVERIFY(p.history.count() > 0 && p.history.count() < 10);
        auto state = p.contentState;
        auto image = p.document.active()->image;
        p.history.undo();
        QVERIFY(p.contentState != state);
        p.history.redo();
        QCOMPARE(p.contentState, state);
        QCOMPARE(p.document.active()->image, image);
        p.markSaved(state);
        p.history.setMemoryBudget(0);
        QCOMPARE(p.history.count(), 0);
        QCOMPARE(p.document.active()->image, image);
        QVERIFY(!p.isModified());
    }
    void sharedPixelsAndSelectionBudget() {
        EditorPage p(photo());
        for (int i = 0; i < 4; ++i)
            p.edit("Name", [i](Document &d) { d.active()->metadata["name"] = QString::number(i); });
        QVERIFY(p.history.memoryUsage() < p.document.active()->image.sizeInBytes() * 4 + 20000);
        p.history.clear();
        p.markSaved(p.contentState);
        p.canvas->selectAll();
        QVERIFY(p.history.memoryUsage() >= p.session.selection.sizeInBytes());
        QVERIFY(!p.isModified());
    }
    void aDamagedProjectOnDiskCanBeReplacedBySaving() {
        QTemporaryDir dir;
        auto path = dir.filePath("Damaged.comp");
        auto d = photo();
        saveProject(d, path);
        const auto intact = projectFingerprint(path);
        auto window = makeWindow();
        window->openPath(path);
        auto p = current(*window);
        // The baseline is worked out in the background, not while opening.
        QTRY_COMPARE_WITH_TIMEOUT(p->monitor->fingerprint(), intact, 8000);
        p->edit("Paint", [](Document &doc) { doc.active()->image.fill(Qt::blue); });
        QVERIFY(QFile::remove(QDir(path).filePath("images/" + d.active()->metadata["imageFile"].toString())));
        // A missing asset is part of the fingerprint instead of an error.
        QVERIFY(projectFingerprint(path) != intact);
        QStringList asked;
        int answer = QMessageBox::No;
        QTimer boxes;
        connect(&boxes, &QTimer::timeout, [&] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                asked << box->text();
                // The button itself: QMessageBox::warning reports the button clicked.
                if (auto button = box->button(QMessageBox::StandardButton(answer)))
                    button->click();
                else
                    box->reject();
            }
        });
        boxes.start(30);
        QAction *save = nullptr;
        for (auto a : window->findChildren<QAction *>())
            if (a->property("layerAction").toString() == "Save Project")
                save = a;
        save->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!asked.isEmpty() && !p->saving, 8000);
        QVERIFY(asked.first().contains("replace", Qt::CaseInsensitive));
        QVERIFY(p->isModified()); // Declined: nothing written.
        QVERIFY_EXCEPTION_THROWN(loadProject(path), Error);
        answer = QMessageBox::Yes;
        asked.clear();
        save->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!p->isModified(), 8000);
        QCOMPARE(loadProject(path).active()->image.pixelColor(0, 0), QColor(Qt::blue));
    }
    void theSameProjectByAnotherLetterCaseReusesItsTab() {
        QTemporaryDir dir;
        auto path = dir.filePath("Case.comp");
        saveProject(photo(), path);
        auto window = makeWindow();
        auto tabs = window->findChild<QTabWidget *>();
        window->openPath(path);
        const int count = tabs->count();
        window->openPath(path.toUpper());
        window->openPath(QDir::toNativeSeparators(path.toLower()));
        QCOMPARE(tabs->count(), count);
        rememberFile(path);
        rememberFile(path.toUpper());
        QCOMPARE(recentFiles().filter("case.comp", Qt::CaseInsensitive).size(), 1);
    }
    void anEditLargerThanTheBudgetCanStillBeUndone() {
        EditorPage p(photo());
        p.history.setMemoryBudget(100);
        const auto before = p.document.active()->image;
        p.edit("Paint", [](Document &d) { d.active()->image.fill(Qt::blue); });
        QCOMPARE(p.history.count(), 1);
        p.history.undo();
        QCOMPARE(p.document.active()->image, before);
        p.history.redo();
        p.edit("Paint again", [](Document &d) { d.active()->image.fill(Qt::green); });
        QCOMPARE(p.history.count(), 1); // Only the newest is kept.
        QCOMPARE(p.history.undoText(), QString("Paint again"));
    }
    void recentFilesAreUniqueAndBounded() {
        QSettings().remove("files/recent");
        for (int i = 0; i < 25; ++i)
            rememberFile(settings_.filePath(QString::number(i) + ".comp"));
        rememberFile(settings_.filePath("10.comp"));
        auto files = recentFiles();
        QCOMPARE(files.size(), 20);
        QCOMPARE(files.first(), settings_.filePath("10.comp"));
        QCOMPARE(files.count(files.first()), 1);
    }
    void stagingCleanupPreservesActiveRecentAndBackupFolders() {
        QTemporaryDir dir;
        const auto old = dir.filePath(".compositor-stage-" + newId());
        const auto active = dir.filePath(".compositor-stage-" + newId());
        const auto recent = dir.filePath(".compositor-stage-" + newId());
        const auto backup = dir.filePath(".compositor-backup-" + newId());
        for (const auto &path : {old, active, recent, backup})
            QVERIFY(QDir().mkdir(path));
        QLockFile lock(QDir(active).filePath("stage.lock"));
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock());
        for (const auto &path : {old, active, backup}) {
#ifdef Q_OS_WIN
            auto native = std::filesystem::path(path.toStdWString());
#else
            auto native = std::filesystem::path(path.toStdString());
#endif
            std::filesystem::last_write_time(native, std::filesystem::file_time_type::clock::now() -
                                                         std::chrono::hours(48));
        }
        RecoveryStore::cleanStaging(dir.path());
        QVERIFY(!QFileInfo::exists(old));
        QVERIFY(QFileInfo::exists(active));
        QVERIFY(QFileInfo::exists(recent));
        QVERIFY(QFileInfo::exists(backup));
    }
    void wicUnicodePixelsAlphaAndErrors() {
        QTemporaryDir dir;
        auto path = dir.filePath("日本語图片.png");
        QImage image(5, 3, QImage::Format_RGBA8888);
        image.fill(QColor(20, 40, 200, 128));
        QVERIFY(image.save(path));
        auto decoded = importWicImage(path);
        QCOMPARE(decoded.size(), image.size());
        QVERIFY(std::abs(decoded.pixelColor(1, 1).blue() - 200) <= 1);
        QCOMPARE(decoded.pixelColor(1, 1).alpha(), 128);
        QVERIFY(isHeifFile("PHONE.HEIC"));
        QVERIFY(isHeifFile("photo.heif"));
        QVERIFY_EXCEPTION_THROWN(importWicImage(dir.filePath("missing.heic")), Error);
    }
    void realFormatFixtures() {
        auto folder = qEnvironmentVariable("COMPOSITOR_IO_FIXTURES");
        if (folder.isEmpty())
            QSKIP("Set COMPOSITOR_IO_FIXTURES to real PSD/PSB/HEIC samples");
        QDir dir(folder);
        QJsonArray report;
        auto names = dir.entryList({"*.psd", "*.psb", "*.heic"}, QDir::Files);
        QVERIFY(!names.isEmpty());
        for (const auto &name : names) {
            QJsonObject entry{{"file", name}};
            try {
                if (isHeifFile(name)) {
                    auto image = importWicImage(dir.filePath(name));
                    QVERIFY(!image.isNull());
                    entry["size"] = QJsonArray{image.width(), image.height()};
                } else {
                    auto result = importPhotoshop(dir.filePath(name));
                    result.document.validateAssets();
                    auto image = renderDocument(result.document);
                    QVERIFY(!image.isNull());
                    entry["size"] = QJsonArray{image.width(), image.height()};
                    entry["layers"] = result.document.layers.size();
                    int masks = 0, effects = 0;
                    for (const auto &layer : result.document.layers) {
                        masks += !layer.mask.isNull();
                        effects += !layer.metadata.value("effects").toObject().isEmpty();
                    }
                    entry["masks"] = masks;
                    entry["effects"] = effects;
                    entry["conversions"] = QJsonArray::fromStringList(result.conversions);
                    auto referenceName = name;
                    referenceName.replace("-src.psd", "-reference.png");
                    QImage reference(dir.filePath(referenceName));
                    if (!reference.isNull() && reference.size() == image.size()) {
                        double difference = 0;
                        for (int y = 0; y < image.height(); ++y)
                            for (int x = 0; x < image.width(); ++x) {
                                auto actual = image.pixelColor(x, y),
                                     expected = reference.pixelColor(x, y);
                                difference += std::abs(actual.red() - expected.red()) +
                                              std::abs(actual.green() - expected.green()) +
                                              std::abs(actual.blue() - expected.blue()) +
                                              std::abs(actual.alpha() - expected.alpha());
                            }
                        difference /= double(image.width()) * image.height() * 4;
                        entry["reference_rgba_mae_255"] = difference;
                        if (name.startsWith("vector-") || name.startsWith("winding-"))
                            QVERIFY2(difference <= 1,
                                     qPrintable(name + ": vector reference differs"));
                    }
                    QTemporaryDir roundTrip;
                    auto path = roundTrip.filePath("Real.comp");
                    saveProject(result.document, path);
                    QCOMPARE(renderDocument(loadProject(path)), image);
                    QVERIFY(image.save(dir.filePath(name + ".render.png")));
                }
                entry["status"] = "passed";
            } catch (const std::exception &e) {
                auto message = QString::fromUtf8(e.what());
                entry["error"] = message;
                if (isHeifFile(name) && message.contains("Install HEIF"))
                    entry["status"] = "codec_missing";
                else {
                    entry["status"] = "failed";
                    QTest::qFail(qPrintable(name + ": " + message), __FILE__, __LINE__);
                }
            }
            report.append(entry);
        }
        QFile output(dir.filePath("io-results.json"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write(QJsonDocument(report).toJson());
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() == 3 && args[1] == "--recovery-crash-helper") {
        RecoveryStore store(args[2]);
        auto d = Document::create({8, 8});
        QImage image(d.size(), QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::blue);
        d.addImage("Crash", image);
        store.write(newId(), d, {}, "Crash");
        std::printf("snapshot-ready\n");
        std::fflush(stdout);
        for (;;)
            QThread::msleep(100);
    }
    IoTests test;
    return QTest::qExec(&test, argc, argv);
}
#include "io_tests.moc"

// SPDX-License-Identifier: MIT
#include "editor.h"
#include "language.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QtConcurrent/QtConcurrentRun>

namespace compositor {
void EditorWindow::externalReload(EditorPage *p, Document source, QByteArray fingerprint) {
    if (p->saving || p->monitor->fingerprint() != fingerprint)
        return;
    if (p->interacting() || QApplication::activeModalWidget() || p->canvas->textEditing()) {
        QPointer<EditorPage> guard(p);
        QTimer::singleShot(500, this, [this, guard, source, fingerprint] {
            if (guard)
                externalReload(guard, source, fingerprint);
        });
        return;
    }
    if (p->isModified()) {
        p->externalConflict = true;
        auto answer = QMessageBox::warning(this, uiText("Project Changed on Disk"),
                                           uiText("The project was changed by another application. "
                                                  "Reloading discards your unsaved edits. Reload?"),
                                           QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    if (aiSelectionDialog_)
        aiSelectionDialog_->reject();
    p->replaceFromDisk(std::move(source));
    if (recovery_ && !p->autosaving) {
        recovery_->remove(p->recoveryId);
        p->recoveredState.reset();
    }
    statusBar()->showMessage(uiText("Reloaded external project changes"), 5000);
}
void EditorWindow::autosave() {
    if (!recovery_)
        return;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto p = qobject_cast<EditorPage *>(tabs_->widget(i));
        if (!p || p->saving || p->autosaving || p->interacting() || p->canvas->textEditing())
            continue;
        if (!p->isModified()) {
            recovery_->remove(p->recoveryId);
            p->recoveredState.reset();
            continue;
        }
        if (p->recoveredState == p->contentState)
            continue;
        auto snapshot = p->document;
        const auto state = p->contentState;
        const auto id = p->recoveryId, source = p->path, title = tabs_->tabText(i);
        p->autosaving = true;
        auto future = new QFutureWatcher<QString>(p);
        connect(future, &QFutureWatcher<QString>::finished, this, [this, future, p, state] {
            auto error = future->result();
            future->deleteLater();
            p->autosaving = false;
            if (error.isEmpty()) {
                p->recoveredState = state;
                if (!p->isModified()) {
                    recovery_->remove(p->recoveryId);
                    p->recoveredState.reset();
                }
            } else
                statusBar()->showMessage(uiText("Recovery snapshot failed: ") + error, 15000);
        });
        auto store = recovery_;
        future->setFuture(QtConcurrent::run([store, snapshot, id, source, title] {
            try {
                store->write(id, snapshot, source, title);
                return QString();
            } catch (const std::exception &e) {
                return QString::fromUtf8(e.what());
            }
        }));
    }
}
void EditorWindow::recoverProjects() {
    if (!recovery_)
        return;
    for (const auto &path : recentFiles())
        if (QFileInfo(path).isDir())
            RecoveryStore::cleanStaging(QFileInfo(path).absolutePath());
    for (const auto &entry : recovery_->available()) {
        auto answer = QMessageBox::question(
            this, uiText("Recover Project"),
            uiText("A recovery snapshot is available: ") + entry.title,
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer == QMessageBox::Cancel)
            continue;
        if (answer == QMessageBox::No) {
            try {
                recovery_->discard(entry);
            } catch (const std::exception &e) {
                showError(QString::fromUtf8(e.what()));
            }
            continue;
        }
        try {
            addPage(loadProject(entry.project));
            auto p = page();
            // A recovered tab is deliberately untitled: Save asks for a destination and cannot
            // overwrite a newer source project from after the crash.
            p->edit("Recover Project", [](Document &) {});
            p->history.clear();
            p->setProperty("displayName", entry.title + uiText(" (Recovered)"));
            recovery_->write(p->recoveryId, p->document, entry.source, entry.title);
            recovery_->discard(entry);
            p->changed();
        } catch (const std::exception &e) {
            showError(QString::fromUtf8(e.what()));
        }
    }
}
void EditorWindow::filePreferences() {
    QDialog dialog(this);
    dialog.setWindowTitle("File and History Preferences");
    QFormLayout layout(&dialog);
    QSpinBox seconds, memory;
    seconds.setRange(0, 3600);
    seconds.setSpecialValueText("Disabled");
    seconds.setValue(QSettings().value("files/autosaveSeconds", 60).toInt());
    memory.setRange(16, 8192);
    memory.setValue(QSettings().value("files/historyMiB", 512).toInt());
    layout.addRow("Recovery interval (seconds)", &seconds);
    layout.addRow("Undo budget per project (MiB)", &memory);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    UiLanguage::instance().translateObject(&dialog, true);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QSettings().setValue("files/autosaveSeconds", seconds.value());
    QSettings().setValue("files/historyMiB", memory.value());
    if (seconds.value())
        autosaveTimer_.start(seconds.value() * 1000);
    else
        autosaveTimer_.stop();
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto p = qobject_cast<EditorPage *>(tabs_->widget(i)))
            p->history.setMemoryBudget(qint64(memory.value()) * 1024 * 1024);
}
} // namespace compositor

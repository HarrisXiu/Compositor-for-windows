// SPDX-License-Identifier: MIT
#include "editor.h"
#include "ai_models_dialog.h"
#include "language.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
namespace compositor {
namespace {
struct JobResult {
    AiSelectionResult selection;
    QString error;
    bool canceled = false;
};
template<class Work> JobResult execute(Work work) {
    JobResult result;
    try { result.selection = work(); }
    catch (const AiCancelled &) { result.canceled = true; }
    catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
    return result;
}
class ObjectSelectionDialog final : public QDialog {
  public:
    explicit ObjectSelectionDialog(EditorPage *page, QWidget *parent)
        : QDialog(parent, Qt::Tool), page_(page), original_(page->session.selection),
          image_(page->canvas->fullComposite()), service_(page->aiSelection) {
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName("aiObjectDialog");
        setWindowTitle("Select Object");
        auto layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("Click the object on the canvas. Alt-click excludes background. Enter applies; Esc cancels."));
        auto controls = new QHBoxLayout;
        model_ = new QComboBox;
        model_->setObjectName("aiObjectModel");
        model_->addItem("SAM 2", "sam2");
        model_->addItem("MobileSAM", "mobilesam");
        controls->addWidget(new QLabel("Model"));
        controls->addWidget(model_);
        label_ = new QComboBox;
        label_->setObjectName("aiPointMode");
        label_->addItems({"Foreground", "Background"});
        controls->addWidget(new QLabel("Point type"));
        controls->addWidget(label_);
        mode_ = new QComboBox;
        mode_->setObjectName("aiSelectionMode");
        mode_->addItems({"Replace", "Add", "Subtract", "Intersect"});
        mode_->setCurrentIndex(page->session.selectionMode);
        controls->addWidget(new QLabel("Selection mode"));
        controls->addWidget(mode_);
        layout->addLayout(controls);
        auto operations = new QHBoxLayout;
        auto undo = new QPushButton("Remove Last Point");
        undo->setObjectName("aiRemovePoint");
        operations->addWidget(undo);
        auto clear = new QPushButton("Clear Points");
        clear->setObjectName("aiClearPoints");
        operations->addWidget(clear);
        auto models = new QPushButton("Manage AI Models…");
        operations->addWidget(models);
        layout->addLayout(operations);
        status_ = new QLabel("Add a foreground point first.");
        status_->setObjectName("aiObjectStatus");
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        ok_ = buttons->button(QDialogButtonBox::Ok);
        ok_->setEnabled(false);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(undo, &QPushButton::clicked, this, [this] { removePoint(); });
        connect(clear, &QPushButton::clicked, this, [this] { points_.clear(); labels_.clear(); request(); });
        connect(models, &QPushButton::clicked, this, [this] {
            AiModelsDialog dialog(this);
            dialog.exec();
            request();
        });
        connect(model_, &QComboBox::currentIndexChanged, this, [this] {
            if (cancellation_) cancellation_->cancel();
            request();
        });
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] { request(); });
        debounce_.setSingleShot(true);
        debounce_.setInterval(40);
        connect(&debounce_, &QTimer::timeout, this, [this] { start(); });
        connect(&watcher_, &QFutureWatcherBase::finished, this, [this] {
            running_ = false;
            if (completed_ || !page_)
                return;
            if (runningGeneration_ != generation_) {
                debounce_.start();
                return;
            }
            const auto result = watcher_.result();
            if (result.canceled || !result.error.isEmpty()) {
                current_ = {};
                page_->session.selection = original_;
                emit page_->canvas->selectionChanged();
                page_->canvas->update();
            }
            if (result.canceled) {
                status_->setText(uiText("Selection canceled."));
                return;
            }
            if (!result.error.isEmpty()) {
                status_->setText(uiText(result.error));
                return;
            }
            try {
                current_ = combineAiSelection(original_, result.selection.mask, mode_->currentIndex());
                appliedGeneration_ = generation_;
                page_->session.selection = current_;
                emit page_->canvas->selectionChanged();
                page_->canvas->update();
                status_->setText(uiText("Selection ready.") + " " + result.selection.backend);
                ok_->setEnabled(true);
            } catch (const std::exception &e) {
                status_->setText(QString::fromUtf8(e.what()));
            }
        });
        connect(page->canvas, &Canvas::aiPointPicked, this, [this](QPointF point, bool exclude) {
            if (points_.size() >= 1024) {
                status_->setText(uiText("The maximum is 1024 points. Remove or clear points to continue."));
                return;
            }
            const int label = exclude || label_->currentIndex() == 1 ? 0 : 1;
            if (points_.isEmpty() && !label) {
                status_->setText(uiText("Add a foreground point first."));
                return;
            }
            points_.append(point);
            labels_.append(label);
            request();
        });
        connect(page->canvas, &Canvas::aiApplyRequested, this, [this] { if (ok_->isEnabled()) accept(); });
        connect(page->canvas, &Canvas::aiCancelRequested, this, &QDialog::reject);
        connect(page->canvas, &Canvas::aiRemovePointRequested, this, [this] { removePoint(); });
        connect(page, &EditorPage::editWillStart, this, &QDialog::reject);
        connect(page->canvas, &Canvas::selectionEdited, this, [this] {
            if (!completed_) { restore_ = false; reject(); }
        });
        // External edits invalidate the snapshot. Keep the selection produced by that edit.
        connect(page, &EditorPage::documentChanged, this, [this] {
            if (!completed_) { restore_ = false; reject(); }
        });
        connect(page, &QObject::destroyed, this, &QDialog::reject);
        connect(this, &QDialog::finished, this, [this](int result) { finish(result == QDialog::Accepted); });
        page->canvas->setAiPicking(true);
        resize(670, 210);
    }
    ~ObjectSelectionDialog() override { finish(false); }
  private:
    void removePoint() {
        if (!points_.isEmpty()) { points_.removeLast(); labels_.removeLast(); request(); }
    }
    void request() {
        if (completed_ || !page_) return;
        ++generation_;
        ok_->setEnabled(false);
        page_->canvas->setAiPoints(points_, labels_);
        if (points_.isEmpty() || !labels_.contains(1)) {
            current_ = {};
            page_->session.selection = original_;
            emit page_->canvas->selectionChanged();
            page_->canvas->update();
            status_->setText(uiText("Add a foreground point first."));
        } else {
            status_->setText(uiText("Calculating selection…"));
            debounce_.start();
        }
    }
    void start() {
        if (running_ || completed_ || !page_ || points_.isEmpty() || !labels_.contains(1)) return;
        running_ = true;
        runningGeneration_ = generation_;
        cancellation_ = std::make_shared<AiCancellation>();
        const auto service = service_;
        const auto image = image_;
        const auto points = points_;
        const auto labels = labels_;
        const auto model = model_->currentData().toString();
        const auto cancel = cancellation_;
        watcher_.setFuture(QtConcurrent::run([service, image, points, labels, model, cancel] {
            return execute([&] { return service->object(image, model, points, labels, cancel); });
        }));
    }
    void finish(bool accept) {
        if (completed_) return;
        completed_ = true;
        debounce_.stop();
        if (cancellation_) cancellation_->cancel();
        disconnect(&watcher_, nullptr, this, nullptr);
        if (!page_) return;
        page_->canvas->setAiPicking(false);
        if (restore_) {
            page_->session.selection = original_;
            emit page_->canvas->selectionChanged();
            page_->canvas->update();
            if (accept && appliedGeneration_ == generation_ && !current_.isNull())
                page_->canvas->replaceSelection(current_, "AI Object Selection");
        }
    }
    QPointer<EditorPage> page_;
    QImage original_, image_, current_;
    std::shared_ptr<AiSelectionService> service_;
    std::shared_ptr<AiCancellation> cancellation_;
    QVector<QPointF> points_;
    QVector<int> labels_;
    QComboBox *model_, *label_, *mode_;
    QLabel *status_;
    QPushButton *ok_;
    QTimer debounce_;
    QFutureWatcher<JobResult> watcher_;
    quint64 generation_ = 0, runningGeneration_ = 0, appliedGeneration_ = 0;
    bool running_ = false, completed_ = false, restore_ = true;
};
}
void EditorWindow::selectObject() {
    auto p = page();
    if (!p) return;
    if (aiSelectionDialog_) aiSelectionDialog_->reject();
    p->canvas->cancelInteraction();
    auto dialog = new ObjectSelectionDialog(p, this);
    aiSelectionDialog_ = dialog;
    // Clear immediately on finish; deleteLater may run after another menu callback.
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (aiSelectionDialog_ == dialog) aiSelectionDialog_.clear();
    });
    dialog->show();
    p->canvas->setFocus();
}
void EditorWindow::selectSubject() {
    QPointer<EditorPage> p = page();
    if (!p) return;
    p->canvas->cancelInteraction();
    const auto image = p->canvas->fullComposite();
    const auto original = p->session.selection;
    const auto revision = p->revision;
    const int mode = p->session.selectionMode;
    const auto service = p->aiSelection;
    auto cancellation = std::make_shared<AiCancellation>();
    QProgressDialog progress("Calculating selection…", "Cancel", 0, 0, this);
    progress.setObjectName("aiSubjectProgress");
    progress.setWindowTitle("Select Subject");
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    QFutureWatcher<JobResult> watcher;
    connect(&progress, &QProgressDialog::canceled, &progress, [cancellation] { cancellation->cancel(); });
    connect(&watcher, &QFutureWatcherBase::finished, &progress, &QDialog::accept);
    watcher.setFuture(QtConcurrent::run([service, image, cancellation] {
        return execute([&] { return service->subject(image, cancellation); });
    }));
    const bool finished = progress.exec() == QDialog::Accepted;
    disconnect(&watcher, nullptr, &progress, nullptr);
    if (!finished || cancellation->cancelled()) { cancellation->cancel(); return; }
    const auto result = watcher.result();
    if (!p || p->revision != revision || p->session.selection != original || result.canceled) return;
    if (!result.error.isEmpty()) {
        showError(uiText(result.error));
        return;
    }
    p->canvas->replaceSelection(combineAiSelection(original, result.selection.mask, mode), "AI Subject Selection");
}
} // namespace compositor

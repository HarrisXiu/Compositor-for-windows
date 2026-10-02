// SPDX-License-Identifier: MIT
#include "raw_dialog.h"
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace compositor {
RawDevelopDialog::RawDevelopDialog(const QString &path, QWidget *parent)
    : QDialog(parent), cancelled_(std::make_shared<std::atomic_bool>(false)) {
    setWindowTitle("Develop RAW — " + QFileInfo(path).fileName());
    setObjectName("rawDevelopDialog");
    auto layout = new QVBoxLayout(this);
    preview_ = new QLabel("Decoding camera data…");
    preview_->setFixedSize(640, 360);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setStyleSheet("background:#18191c;");
    layout->addWidget(preview_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setMaximumWidth(640);
    layout->addWidget(status_);
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto spin = [&](const QString &name, double min, double max, int decimals, double value,
                    QString suffix) {
        auto s = new QDoubleSpinBox;
        s->setObjectName(name.toLower() + "Control");
        s->setRange(min, max);
        s->setDecimals(decimals);
        s->setSingleStep(decimals ? .05 : 100);
        s->setValue(value);
        s->setSuffix(suffix);
        form->addRow(name, s);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { schedule(); });
        return s;
    };
    exposure_ = spin("Exposure", -3, 3, 2, 0, " EV");
    temperature_ = spin("Temperature", 2000, 12000, 0, 5000, " K");
    tint_ = spin("Tint", -150, 150, 0, 0, "");
    tint_->setSingleStep(1);
    boost_ = spin("Boost", 0, 1, 2, 1, "");
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                    QDialogButtonBox::Reset);
    buttons_->button(QDialogButtonBox::Ok)->setText("Import");
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons_);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(80);
    connect(timer_, &QTimer::timeout, this, &RawDevelopDialog::render);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (!source_ || failed_)
            return;
        final_ = true;
        timer_->stop();
        for (auto s : {exposure_, temperature_, tint_, boost_})
            s->setEnabled(false);
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
        buttons_->button(QDialogButtonBox::Reset)->setEnabled(false);
        status_->setText("Developing full-size image…");
        render();
    });
    connect(buttons_->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this] {
        exposure_->setValue(0);
        temperature_->setValue(5000);
        tint_->setValue(0);
        boost_->setValue(1);
    });
    using Load = std::pair<std::shared_ptr<RawSource>, QString>;
    auto watcher = new QFutureWatcher<Load>(this);
    connect(watcher, &QFutureWatcher<Load>::finished, this, [this, watcher] {
        auto result = watcher->result();
        watcher->deleteLater();
        busy_ = false;
        if (!result.second.isEmpty()) {
            fail(result.second);
            return;
        }
        source_ = std::move(result.first);
        status_->setText(source_->camera() + " — " + QString::number(source_->size().width()) +
                         " × " + QString::number(source_->size().height()) +
                         " px\nReset retains camera white balance. The temperature scale is "
                         "relative to a 5000 K estimate.");
        render();
    });
    watcher->setFuture(QtConcurrent::run([path, cancelled = cancelled_] {
        try {
            return Load{RawSource::open(path, cancelled), {}};
        } catch (const std::exception &e) {
            return Load{{}, QString::fromUtf8(e.what())};
        }
    }));
}
RawDevelopDialog::~RawDevelopDialog() {
    cancelled_->store(true);
}
RawSettings RawDevelopDialog::settings() const {
    auto s = source_ ? source_->asShot() : RawSettings{};
    s.exposure = exposure_->value();
    s.temperature = temperature_->value();
    s.tint = tint_->value();
    s.boost = boost_->value();
    return s;
}
void RawDevelopDialog::schedule() {
    ++revision_;
    if (!final_ && !failed_)
        timer_->start();
}
void RawDevelopDialog::showPreview(const QImage &image) {
    preview_->setPixmap(QPixmap::fromImage(
        image.scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}
void RawDevelopDialog::fail(const QString &message) {
    failed_ = true;
    timer_->stop();
    status_->setText(message);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
}
void RawDevelopDialog::render() {
    if (!source_ || busy_ || failed_)
        return;
    busy_ = true;
    auto current = settings();
    auto revision = revision_;
    bool final = final_;
    using Result = std::pair<QImage, QString>;
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, revision, final] {
        auto result = watcher->result();
        watcher->deleteLater();
        busy_ = false;
        if (!result.second.isEmpty()) {
            fail(result.second);
            return;
        }
        if (final) {
            imported_ = std::move(result.first);
            accept();
            return;
        }
        if (revision == revision_)
            showPreview(result.first);
        if (final_ || revision != revision_)
            render();
        else
            buttons_->button(QDialogButtonBox::Ok)->setEnabled(true);
    });
    watcher->setFuture(QtConcurrent::run([source = source_, current, final] {
        try {
            return Result{source->develop(current, final ? 0 : 800), {}};
        } catch (const std::exception &e) {
            return Result{{}, QString::fromUtf8(e.what())};
        }
    }));
}
} // namespace compositor

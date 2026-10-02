// SPDX-License-Identifier: MIT
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <stdexcept>

namespace compositor {
constexpr int CurrentVersion = 11;
constexpr int MaxSide = 30000;
constexpr qint64 MaxSurfacePixels = 200000000;

class Error : public std::runtime_error {
  public:
    explicit Error(const QString &message) : std::runtime_error(message.toUtf8().constData()) {}
};
void require(bool condition, const QString &message);
QString newId();
QString normalizedId(const QString &id);

struct Layer {
    QJsonObject metadata;
    QImage image;
    QImage mask;
    QString id() const;
    QString name() const;
    QString parent() const;
    bool group() const;
    bool visible() const;
    double opacity() const;
    QString blend() const;
    QJsonObject transform() const;
    QTransform placement(const QSize &sourceSize) const;
    void move(QPointF delta);
    void setBounds(QRectF bounds);
};

struct Document {
    QJsonObject metadata;
    QVector<Layer> layers;
    QSize size() const;
    QString activeId() const;
    Layer *active();
    const Layer *active() const;
    Layer *find(const QString &id);
    const Layer *find(const QString &id) const;
    QJsonObject manifest() const;
    void validate() const;
    void validateAssets() const;
    QStringList previewLimitations() const;
    static Document create(QSize size);
    QString addImage(const QString &name, const QImage &image);
    QString addBlank(const QString &name);
    QString addGroup(const QString &name);
    QString duplicate(const QString &id);
    void remove(const QString &id);
};

Document loadProject(const QString &path);
void saveProject(const Document &document, const QString &path);
QImage importImage(const QString &path);
qint64 documentPixelBudget();
QJsonObject makeTransform(QRectF bounds);
QStringList blendModes();
} // namespace compositor

// SPDX-License-Identifier: MIT
#include "document.h"
#include "effects.h"
#include "project_io.h"
#include "wic_import.h"
#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QSvgRenderer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace compositor {
void require(bool condition, const QString &message) {
    if (!condition)
        throw Error(message);
}
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
}
QString normalizedId(const QString &id) {
    return QUuid(id).toString(QUuid::WithoutBraces).toUpper();
}
static bool validId(const QString &id) {
    return !QUuid(id).isNull();
}
static double number(const QJsonObject &o, const char *key, double fallback = 0) {
    return o.value(QLatin1String(key)).toDouble(fallback);
}
static QPointF pair(const QJsonObject &o, const char *key) {
    auto a = o.value(QLatin1String(key)).toArray();
    return a.size() == 2 ? QPointF(a[0].toDouble(), a[1].toDouble()) : QPointF();
}
QJsonObject makeTransform(QRectF r) {
    return {{"origin", QJsonArray{r.x(), r.y()}},
            {"size", QJsonArray{r.width(), r.height()}},
            {"rotation", 0},
            {"flipX", false},
            {"flipY", false},
            {"sampling", "High quality"}};
}
QStringList blendModes() {
    return {"Normal",     "Darken",     "Multiply",    "Color Burn",         "Linear Burn",
            "Lighten",    "Screen",     "Color Dodge", "Linear Dodge (Add)", "Overlay",
            "Soft Light", "Hard Light", "Vivid Light", "Linear Light",       "Pin Light",
            "Hard Mix",   "Difference", "Exclusion",   "Subtract",           "Divide",
            "Hue",        "Saturation", "Color",       "Luminosity"};
}
QString Layer::id() const {
    return normalizedId(metadata.value("id").toString());
}
QString Layer::name() const {
    return metadata.value("name").toString();
}
QString Layer::parent() const {
    auto v = metadata.value("parentID").toString();
    return v.isEmpty() ? QString() : normalizedId(v);
}
bool Layer::group() const {
    return metadata.value("isGroup").toBool();
}
bool Layer::visible() const {
    return metadata.value("isVisible").toBool();
}
double Layer::opacity() const {
    return metadata.value("opacity").toDouble(1);
}
QString Layer::blend() const {
    return metadata.value("blendMode").toString("Normal");
}
QJsonObject Layer::transform() const {
    return metadata.value("transform").toObject();
}
QTransform Layer::placement(const QSize &source) const {
    const auto t = transform();
    const auto p = pair(t, "origin"), s = pair(t, "size");
    QTransform map;
    map.translate(p.x() + s.x() / 2, p.y() + s.y() / 2);
    map.rotate(number(t, "rotation"));
    map.scale(s.x() / std::max(1, source.width()) * (t.value("flipX").toBool() ? -1 : 1),
              s.y() / std::max(1, source.height()) * (t.value("flipY").toBool() ? -1 : 1));
    map.translate(-source.width() / 2.0, -source.height() / 2.0);
    return map;
}
void Layer::move(QPointF delta) {
    auto t = transform();
    const auto p = pair(t, "origin") + delta;
    t["origin"] = QJsonArray{p.x(), p.y()};
    metadata["transform"] = t;
    if (metadata.value("maskLinked").toBool(true) && metadata.contains("maskPlacement")) {
        auto m = metadata.value("maskPlacement").toObject();
        const auto mp = pair(m, "origin") + delta;
        m["origin"] = QJsonArray{mp.x(), mp.y()};
        metadata["maskPlacement"] = m;
    }
}
void Layer::setBounds(QRectF bounds) {
    auto t = transform();
    t["origin"] = QJsonArray{bounds.x(), bounds.y()};
    t["size"] = QJsonArray{bounds.width(), bounds.height()};
    metadata["transform"] = t;
}
QSize Document::size() const {
    return {metadata.value("width").toInt(), metadata.value("height").toInt()};
}
QString Document::activeId() const {
    const auto id = metadata.value("activeLayerID").toString();
    return id.isEmpty() ? QString() : normalizedId(id);
}
Layer *Document::find(const QString &id) {
    for (auto &l : layers)
        if (l.id() == id)
            return &l;
    return nullptr;
}
const Layer *Document::find(const QString &id) const {
    for (const auto &l : layers)
        if (l.id() == id)
            return &l;
    return nullptr;
}
Layer *Document::active() {
    return find(activeId());
}
const Layer *Document::active() const {
    return find(activeId());
}
QJsonObject Document::manifest() const {
    auto result = metadata;
    QJsonArray records;
    for (const auto &layer : layers)
        records.append(layer.metadata);
    result["layers"] = records;
    return result;
}
static void validateTransform(const QJsonObject &t) {
    for (const auto &key : {"origin", "size"}) {
        auto a = t.value(QLatin1String(key)).toArray();
        require(a.size() == 2 && a[0].isDouble() && a[1].isDouble(),
                "Invalid transform coordinates");
        require(std::isfinite(a[0].toDouble()) && std::isfinite(a[1].toDouble()),
                "Non-finite transform");
    }
    const auto o = pair(t, "origin"), s = pair(t, "size");
    require(std::abs(o.x()) <= 1000000 && std::abs(o.y()) <= 1000000 && s.x() >= 1 && s.y() >= 1 &&
                s.x() <= 300000 && s.y() <= 300000,
            "Transform exceeds limits");
    require(std::isfinite(number(t, "rotation")), "Invalid rotation");
    require(QStringList{"Nearest", "Smooth", "High quality"}.contains(
                t.value("sampling").toString("High quality")),
            "Invalid sampling mode");
}
void Document::validate() const {
    const int version = metadata.value("version").toInt();
    require(metadata.value("format") == "com.compositor.project", "Not a Compositor project");
    require(version >= 1 && version <= CurrentVersion,
            QString("Unsupported format version %1 (supported: 1–11)").arg(version));
    require(metadata.value("colorSpace") == "sRGB", "Unsupported working color space");
    require(validId(metadata.value("documentID").toString()), "Invalid document UUID");
    const auto s = size();
    require(s.width() > 0 && s.height() > 0 && s.width() <= MaxSide && s.height() <= MaxSide,
            "Canvas exceeds size limits");
    require(layers.size() <= 10000, "Too many layers");
    if (metadata.contains("resolution"))
        require(number(metadata, "resolution") >= 1 && number(metadata, "resolution") <= 9600,
                "Invalid resolution");
    QSet<QString> ids;
    for (const auto &l : layers) {
        require(validId(l.metadata.value("id").toString()) && !ids.contains(l.id()),
                "Invalid or duplicate layer UUID");
        ids.insert(l.id());
        require(!l.name().trimmed().isEmpty() && l.name().toUtf8().size() <= 16384,
                "Invalid layer name");
        require(l.metadata.value("isVisible").isBool(), "Missing layer visibility");
        auto effects = l.metadata.value("effects").toObject();
        QJsonObject knownEffects;
        for (auto it = effects.begin(); it != effects.end(); ++it)
            if (QStringList{"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow",
                            "innerGlow"}
                    .contains(it.key()))
                knownEffects[it.key()] = it.value();
        validateEffects(knownEffects);
        validateTransform(l.transform());
        require(std::isfinite(l.opacity()) && l.opacity() >= 0 && l.opacity() <= 1 &&
                    blendModes().contains(l.blend()),
                "Invalid layer appearance");
        require(version >= 3 || (l.opacity() == 1 && l.blend() == "Normal"),
                "Appearance requires format version 3");
        require(version >= 2 || (!l.group() && l.parent().isEmpty()),
                "Folders require format version 2");
        require(!l.group() || (l.metadata.value("imageFile").toString().isEmpty() &&
                               l.blend() == "Normal" && (version >= 8 || l.opacity() == 1)),
                "Invalid folder");
        auto imageFile = l.metadata.value("imageFile").toString();
        auto maskFile = l.metadata.value("maskFile").toString();
        require(imageFile.isEmpty() || imageFile == l.id() + ".png", "Unsafe image filename");
        require(maskFile.isEmpty() ||
                    (maskFile == l.id() + ".mask.png" && version >= (l.group() ? 6 : 4)),
                "Invalid mask filename or version");
        require(!l.metadata.contains("maskEnabled") || !maskFile.isEmpty(),
                "Mask enabled without a mask");
        if (l.metadata.contains("maskPlacement")) {
            require(!maskFile.isEmpty(), "Mask placement without a mask");
            validateTransform(l.metadata.value("maskPlacement").toObject());
        }
        require(version >= 5 || l.metadata.value("maskSourceID").toString().isEmpty(),
                "Clipping masks require version 5");
        if (l.metadata.value("adjustment").isObject()) {
            require(version >= 7 && !l.group() && imageFile.isEmpty(), "Invalid adjustment layer");
            auto a = l.metadata.value("adjustment").toObject();
            auto kind = a.value("kind").toString();
            require(QStringList{"Hue/Saturation", "Levels", "Curves", "Exposure", "Gradient Map",
                                "Grain", "Invert", "Black & White", "Color Balance",
                                "Gaussian Blur", "Motion Blur", "Add Noise"}
                        .contains(kind),
                    "Unknown adjustment kind");
            require(version >= 9 ||
                        !QStringList{"Gaussian Blur", "Motion Blur", "Add Noise"}.contains(kind),
                    "Adjustment requires version 9");
        }
        if (l.metadata.value("text").isObject()) {
            require(!imageFile.isEmpty() && !l.group() &&
                        !l.metadata.value("adjustment").isObject(),
                    "Invalid text layer");
            auto text = l.metadata.value("text").toObject();
            require(version >= 10 || !text.value("colorRuns").isArray(),
                    "Text color runs require version 10");
            require(version >= 11 || !text.value("fontRuns").isArray(),
                    "Text font runs require version 11");
        }
    }
    require(activeId().isEmpty() || ids.contains(activeId()), "Missing active layer");
    for (const auto &l : layers) {
        QSet<QString> seen{l.id()};
        auto parent = l.parent();
        while (!parent.isEmpty()) {
            const auto *node = find(parent);
            require(node && node->group() && !seen.contains(parent) && seen.size() <= 64,
                    "Invalid folder hierarchy");
            seen.insert(parent);
            parent = node->parent();
        }
        seen = {l.id()};
        auto source = l.metadata.value("maskSourceID").toString();
        while (!source.isEmpty()) {
            auto id = normalizedId(source);
            const auto *node = find(id);
            require(!l.group() && node && !node->group() && !seen.contains(id) &&
                        seen.size() <= 256,
                    "Invalid clipping mask graph");
            seen.insert(id);
            source = node->metadata.value("maskSourceID").toString();
        }
    }
    const auto guides = metadata.value("guides").toArray();
    require(guides.size() <= 1000 && (version >= 8 || guides.isEmpty()), "Invalid guides");
    QSet<QString> guideIds;
    for (const auto &v : guides) {
        auto g = v.toObject();
        auto id = g.value("id").toString();
        require(validId(id) && !guideIds.contains(normalizedId(id)), "Invalid guide UUID");
        guideIds.insert(normalizedId(id));
        require(QStringList{"horizontal", "vertical"}.contains(g.value("axis").toString()) &&
                    g.value("position").isDouble() && std::abs(number(g, "position")) <= 1000000,
                "Invalid guide");
    }
}
qint64 documentPixelBudget() {
    qint64 memory = 8LL * 1024 * 1024 * 1024;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        memory = static_cast<qint64>(status.ullTotalPhys);
#endif
    return std::clamp(memory / 16, MaxSurfacePixels, 800000000LL);
}
void Document::validateAssets() const {
    validate();
    qint64 pixels = 0, masks = 0;
    for (const auto &l : layers) {
        if (!l.metadata.value("imageFile").toString().isEmpty()) {
            require(!l.image.isNull(), "Missing image for " + l.name());
            require(l.image.width() <= MaxSide && l.image.height() <= MaxSide,
                    "Image exceeds size limit");
            pixels += qint64(l.image.width()) * l.image.height();
        }
        if (!l.metadata.value("maskFile").toString().isEmpty()) {
            require(!l.mask.isNull() && l.mask.format() == QImage::Format_Grayscale8,
                    "Missing or invalid mask for " + l.name());
            require(l.mask.width() <= MaxSide && l.mask.height() <= MaxSide,
                    "Mask exceeds size limit");
            masks += qint64(l.mask.width()) * l.mask.height();
        }
    }
    require(pixels <= documentPixelBudget() && masks <= documentPixelBudget(),
            "Project exceeds memory budget");
}
QStringList Document::previewLimitations() const {
    QStringList result;
    for (const auto &l : layers) {
        auto effects = l.metadata.value("effects").toObject();
        for (auto it = effects.begin(); it != effects.end(); ++it)
            if (!QStringList{"stroke", "shadow", "colorOverlay", "innerShadow", "outerGlow",
                             "innerGlow"}
                     .contains(it.key()) &&
                !it.value().isNull())
                result << "Unknown layer effect: " + it.key();
    }
    result.removeDuplicates();
    return result;
}
Document Document::create(QSize s) {
    Document d;
    d.metadata = {{"format", "com.compositor.project"},
                  {"version", CurrentVersion},
                  {"colorSpace", "sRGB"},
                  {"documentID", newId()},
                  {"width", s.width()},
                  {"height", s.height()},
                  {"resolution", 72}};
    d.validate();
    return d;
}
QString Document::addImage(const QString &name, const QImage &image) {
    require(!image.isNull(), "Empty image");
    const auto id = newId();
    Layer l;
    l.metadata = {{"id", id},
                  {"name", name},
                  {"isVisible", true},
                  {"imageFile", id + ".png"},
                  {"transform", makeTransform(QRectF(QPointF(0, 0), image.size()))},
                  {"opacity", 1},
                  {"blendMode", "Normal"}};
    l.image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    layers.push_back(l);
    metadata["activeLayerID"] = id;
    metadata["version"] = CurrentVersion;
    return id;
}
QString Document::addBlank(const QString &name) {
    require(qint64(size().width()) * size().height() <= MaxSurfacePixels,
            "Canvas too large to allocate a paint layer");
    QImage image(size(), QImage::Format_RGBA8888_Premultiplied);
    require(!image.isNull(), "Not enough memory");
    image.fill(Qt::transparent);
    return addImage(name, image);
}
QString Document::addGroup(const QString &name) {
    Layer l;
    auto id = newId();
    l.metadata = {{"id", id},
                  {"name", name},
                  {"isVisible", true},
                  {"isGroup", true},
                  {"transform", makeTransform(QRectF(QPointF(0, 0), size()))},
                  {"opacity", 1}};
    layers.push_back(l);
    metadata["activeLayerID"] = id;
    metadata["version"] = CurrentVersion;
    return id;
}
QString Document::duplicate(const QString &id) {
    const auto *source = find(id);
    require(source, "Select a layer to duplicate");
    QHash<QString, QString> replacements;
    replacements[id] = newId();
    bool added;
    do {
        added = false;
        for (const auto &l : layers)
            if (replacements.contains(l.parent()) && !replacements.contains(l.id())) {
                replacements[l.id()] = newId();
                added = true;
            }
    } while (added);
    QVector<Layer> copies;
    for (const auto &l : layers)
        if (replacements.contains(l.id())) {
            auto copy = l;
            auto next = replacements.value(l.id());
            copy.metadata["id"] = next;
            if (l.id() == id)
                copy.metadata["name"] = copy.name() + " copy";
            if (replacements.contains(l.parent()))
                copy.metadata["parentID"] = replacements.value(l.parent());
            auto clip = normalizedId(l.metadata.value("maskSourceID").toString());
            if (replacements.contains(clip))
                copy.metadata["maskSourceID"] = replacements.value(clip);
            if (!copy.image.isNull())
                copy.metadata["imageFile"] = next + ".png";
            if (!copy.mask.isNull())
                copy.metadata["maskFile"] = next + ".mask.png";
            copies.push_back(copy);
        }
    layers.append(copies);
    auto next = replacements.value(id);
    metadata["activeLayerID"] = next;
    return next;
}
void Document::remove(const QString &id) {
    QSet<QString> removed{id};
    bool changed;
    do {
        changed = false;
        for (const auto &l : layers)
            if (removed.contains(l.parent()) && !removed.contains(l.id())) {
                removed.insert(l.id());
                changed = true;
            }
    } while (changed);
    for (auto &l : layers)
        if (removed.contains(normalizedId(l.metadata.value("maskSourceID").toString())))
            l.metadata.remove("maskSourceID");
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                [&](const Layer &l) { return removed.contains(l.id()); }),
                 layers.end());
    if (layers.isEmpty())
        metadata.remove("activeLayerID");
    else
        metadata["activeLayerID"] = layers.back().id();
}
static QByteArray readSafe(const QString &path, const QString &root, qint64 limit) {
    QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    require(info.isFile() && !info.isSymLink() && !canonical.isEmpty() &&
                canonical.startsWith(root + '/', Qt::CaseInsensitive),
            "Unsafe or missing project asset");
    require(info.size() <= limit, "Project asset exceeds file-size limit");
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "Cannot read project asset");
    return f.readAll();
}
Document loadProject(const QString &path) {
    QFileInfo package(path);
    require(package.isDir(), "A .comp project is a folder containing manifest.json");
    const auto root = package.canonicalFilePath();
    QJsonParseError error;
    auto json = QJsonDocument::fromJson(
        readSafe(QDir(path).filePath("manifest.json"), root, 4 * 1024 * 1024), &error);
    require(error.error == QJsonParseError::NoError && json.isObject(), "Damaged project manifest");
    Document d;
    d.metadata = json.object();
    require(d.metadata.value("layers").isArray(), "Missing layer list");
    for (const auto &v : d.metadata.value("layers").toArray()) {
        require(v.isObject(), "Invalid layer record");
        Layer l;
        l.metadata = v.toObject();
        d.layers.push_back(l);
    }
    d.validate();
    qint64 pixels = 0, masks = 0;
    for (auto &l : d.layers) {
        for (bool isMask : {false, true}) {
            const auto filename = l.metadata.value(isMask ? "maskFile" : "imageFile").toString();
            if (filename.isEmpty())
                continue;
            const auto bytes =
                readSafe(QDir(path).filePath("images/" + filename), root, 512LL * 1024 * 1024);
            require(bytes.size() > 26 &&
                        bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) &&
                        uchar(bytes[24]) <= 8,
                    "Project asset must be an 8-bit PNG");
            if (isMask)
                require(uchar(bytes[25]) == 0, "Mask must be grayscale without alpha");
            QImage image;
            QImageReader reader;
            QBuffer buffer;
            buffer.setData(bytes);
            buffer.open(QIODevice::ReadOnly);
            reader.setDevice(&buffer);
            reader.setFormat("png");
            const auto s = reader.size();
            require(s.width() > 0 && s.height() > 0 && s.width() <= MaxSide &&
                        s.height() <= MaxSide,
                    "Asset exceeds dimension limit");
            auto &used = isMask ? masks : pixels;
            used += qint64(s.width()) * s.height();
            require(used <= documentPixelBudget(), "Project exceeds memory budget");
            image = reader.read();
            require(!image.isNull(), "Damaged project PNG");
            if (isMask)
                l.mask = image.convertToFormat(QImage::Format_Grayscale8);
            else
                l.image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        }
    }
    d.validateAssets();
    return d;
}
QByteArray saveProject(const Document &d, const QString &path,
                       const QByteArray &expectedFingerprint) {
    d.validateAssets();
    const auto manifest = QJsonDocument(d.manifest()).toJson();
    require(manifest.size() <= 4 * 1024 * 1024, "Manifest exceeds 4 MiB");
    QFileInfo destination(QDir::cleanPath(path));
    require(destination.fileName().endsWith(".comp", Qt::CaseInsensitive) &&
                !destination.isSymLink(),
            "Choose a .comp folder");
    QDir parent(destination.absolutePath());
    require(parent.exists(), "Destination parent does not exist");
    require(!destination.exists() || destination.isDir(), "Destination is not a project folder");
    if (destination.exists()) {
        QFile header(QDir(path).filePath("manifest.json"));
        require(header.open(QIODevice::ReadOnly),
                "Refusing to replace a folder that is not a Compositor project");
        auto obj = QJsonDocument::fromJson(header.read(4 * 1024 * 1024)).object();
        require(obj.value("format") == "com.compositor.project",
                "Destination is not a Compositor project");
    }
    const auto nonce = newId();
    const auto stageName = ".compositor-stage-" + nonce, backupName = ".compositor-backup-" + nonce;
    require(parent.mkdir(stageName), "Cannot create staging folder");
    QDir stage(parent.filePath(stageName));
    QLockFile stageLock(stage.filePath("stage.lock"));
    stageLock.setStaleLockTime(0);
    require(stageLock.tryLock(), "Cannot lock staging folder");
    bool backedUp = false;
    try {
        require(stage.mkdir("images"), "Cannot create image directory");
        for (const auto &l : d.layers)
            for (bool isMask : {false, true}) {
                auto filename = l.metadata.value(isMask ? "maskFile" : "imageFile").toString();
                if (filename.isEmpty())
                    continue;
                QSaveFile file(stage.filePath("images/" + filename));
                require(file.open(QIODevice::WriteOnly), "Cannot stage layer asset");
                require((isMask ? l.mask : l.image).save(&file, "PNG") && file.commit(),
                        "Cannot save layer PNG");
            }
        QSaveFile file(stage.filePath("manifest.json"));
        require(file.open(QIODevice::WriteOnly) && file.write(manifest) == manifest.size() &&
                    file.commit(),
                "Cannot stage manifest");
        const auto savedFingerprint = projectFingerprint(stage.path());
        if (!expectedFingerprint.isEmpty())
            require(projectFingerprint(path) == expectedFingerprint,
                    "Project changed on disk during saving; external changes were preserved");
        stageLock.unlock();
        if (destination.exists()) {
            require(parent.rename(destination.fileName(), backupName),
                    "Project is in use; cannot replace it");
            backedUp = true;
        }
        if (!parent.rename(stageName, destination.fileName())) {
            if (backedUp && !parent.rename(backupName, destination.fileName()))
                throw Error("Save failed. Previous project preserved at " +
                            parent.filePath(backupName));
            throw Error("Cannot install staged project; previous project restored");
        }
        if (backedUp)
            QDir(parent.filePath(backupName)).removeRecursively();
        return savedFingerprint;
    } catch (...) {
        stageLock.unlock();
        if (stage.exists())
            stage.removeRecursively();
        throw;
    }
}
QImage importImage(const QString &path) {
    if (isHeifFile(path))
        return importWicImage(path);
    QImageReader reader(path);
    reader.setAutoTransform(true);
    auto size = reader.size();
    require(size.width() > 0 && size.height() > 0 && size.width() <= MaxSide &&
                size.height() <= MaxSide &&
                qint64(size.width()) * size.height() <= documentPixelBudget(),
            "Unsupported image or image exceeds size limits");
    auto image = reader.read();
    require(!image.isNull(), reader.errorString());
    if (image.colorSpace().isValid())
        image.convertToColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}
} // namespace compositor

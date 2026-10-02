// SPDX-License-Identifier: MIT
#include "demo.h"
#include "editor.h"
#include "language.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImageReader>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Compositor");
    app.setApplicationVersion("0.3.0");
    app.setOrganizationName("Compositor");
    compositor::UiLanguage::instance().initialize();
    app.setStyle(QStyleFactory::create("Fusion"));
    QImageReader::setAllocationLimit(1024);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(39, 42, 48));
    palette.setColor(QPalette::WindowText, QColor(225, 228, 235));
    palette.setColor(QPalette::Base, QColor(29, 31, 36));
    palette.setColor(QPalette::AlternateBase, QColor(45, 48, 55));
    palette.setColor(QPalette::Text, QColor(225, 228, 235));
    palette.setColor(QPalette::Button, QColor(48, 52, 60));
    palette.setColor(QPalette::ButtonText, QColor(225, 228, 235));
    palette.setColor(QPalette::Highlight, QColor(45, 114, 195));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    app.setPalette(palette);
    app.setFont(QFont("Segoe UI", 10));
    const auto arguments = QCoreApplication::arguments();
    for (qsizetype i = 1; i + 1 < arguments.size(); ++i)
        if (arguments[i] == "--language")
            compositor::UiLanguage::instance().setLanguage(arguments[++i], false);
    if (arguments.size() == 3 && arguments[1] == "--make-demo") {
        try {
            compositor::saveProject(compositor::createDemoDocument(), arguments[2]);
            return 0;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    compositor::EditorWindow window;
    window.show();
    QString screenshot;
    int smokeMs = 0;
    QStringList paths;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        auto arg = arguments[i];
        if (arg == "--smoke-test") {
            smokeMs = 1500;
        } else if (arg == "--screenshot" && i + 1 < arguments.size()) {
            screenshot = arguments[++i];
            smokeMs = 2000;
        } else if (arg == "--language" && i + 1 < arguments.size()) {
            ++i;
        } else
            paths.append(arg);
    }
    if (smokeMs)
        QTimer::singleShot(smokeMs, &app, [&] {
            if (!screenshot.isEmpty())
                (QApplication::activeModalWidget() ? QApplication::activeModalWidget() : &window)
                    ->grab()
                    .save(screenshot);
            app.quit();
        });
    QTimer::singleShot(0, &window, [&window, paths] {
        for (const auto &path : paths)
            window.openPath(path);
    });
    return app.exec();
}

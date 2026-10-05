// SPDX-License-Identifier: MIT
#pragma once
#include <QApplication>
#include <QWidget>
// The dialog an edit command has open: a filter dialog stays open beside the window rather than
// blocking it, so it is found among the top-level widgets; anything else is the modal one.
inline QWidget *activeLiveDialog() {
    for (auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "filterDialog" && widget->isVisible())
            return widget;
    return QApplication::activeModalWidget();
}

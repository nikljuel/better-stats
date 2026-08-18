#pragma once

#include <QString>

struct ScreenSize {
    int width = 0;
    int height = 0;
    int panelHeight = 0;
};

/* Initializes InkView (TASK_MAKEACTIVE) and returns the panel metrics.
 * inkview.h stays in this one TU; its macros collide with Qt. */
ScreenSize openInkViewScreen();
QString inkViewFontFamily();
QString inkViewLang(); /* e.g. "de", "en" */

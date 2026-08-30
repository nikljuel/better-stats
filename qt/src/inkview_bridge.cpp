#include "inkview_bridge.h"

#include <inkview.h>

ScreenSize openInkViewScreen()
{
    InitInkview(TASK_MAKEACTIVE);
    return {ScreenWidth(), ScreenHeight(), PanelHeight()};
}

QString inkViewFontFamily()
{
    const char *family = iv_get_default_font(FONT_FAMILY);
    return family != nullptr ? QString::fromUtf8(family) : QString();
}

QString inkViewLang()
{
    const char *lang = currentLang();
    return lang != nullptr ? QString::fromUtf8(lang) : QString();
}

void rebootPocketBook()
{
    iv_ipc_request(MSG_REBOOT, 1, nullptr, 0, 0);
}

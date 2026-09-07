#include "inkview_bridge.h"

#include <dlfcn.h>

#include <inkview.h>

namespace {

void *inkViewLibrary()
{
    static void *library = dlopen("libinkview.so", RTLD_LAZY | RTLD_LOCAL);
    return library;
}

} // namespace

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

bool isScreenInverted()
{
    using GetScreenModeInversion = bool (*)();
    auto getInversion = reinterpret_cast<GetScreenModeInversion>(
        dlsym(inkViewLibrary(), "IvGetScreenModeInversion"));
    if (getInversion)
        return getInversion();

    iconfig *cfg = OpenConfig("/mnt/ext1/system/config/global.cfg", nullptr);
    if (!cfg)
        return false;
    const bool inverted = ReadInt(cfg, "screen_mode_inverted", 0) != 0;
    CloseConfigNoSave(cfg);
    return inverted;
}

void enableScreenInversion()
{
    using SetAppCapability = void (*)(int);
    auto setCapability = reinterpret_cast<SetAppCapability>(
        dlsym(inkViewLibrary(), "IvSetAppCapability"));
    if (setCapability)
        setCapability(1); // APP_CAPABILITY_SUPPORT_SCREEN_INVERSION
}

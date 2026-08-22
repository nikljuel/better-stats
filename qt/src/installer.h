#pragma once

#include <QString>

struct AutostartStatus {
    bool enabled = false;
    bool available = false;
    QString message;
};

/* Registers the app's launcher icon on first run, so distribution stays a
 * single-file copy: the icon travels inside the binary as a Qt resource and is
 * written to the device plus wired into the launcher config on first launch. */
void ensureRegistered();

/* Optional EPUB proxy setup. Disabled by default; all writes stay below
 * /mnt/ext1 and preserve the firmware's existing handler list. */
AutostartStatus autostartStatus();
AutostartStatus setAutostartEnabled(bool enabled);

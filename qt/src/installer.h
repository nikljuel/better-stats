#pragma once

/* Registers the app's launcher icon on first run, so distribution stays a
 * single-file copy: the icon travels inside the binary as a Qt resource and is
 * written to the device plus wired into the launcher config on first launch. */
void ensureRegistered();

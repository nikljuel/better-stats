#ifndef AUTOSTART_H
#define AUTOSTART_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int enabled;
    int available;
    char message[192];
} bs_autostart_status;

void bs_autostart_get(bs_autostart_status *out);
void bs_autostart_set(int enabled, bs_autostart_status *out);

#ifdef __cplusplus
}
#endif

#endif

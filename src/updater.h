#ifndef UPDATER_H
#define UPDATER_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BS_UPDATE_CURRENT = 0,
    BS_UPDATE_AVAILABLE = 1,
    BS_UPDATE_ERR_NETWORK = -1,
    BS_UPDATE_ERR_DOWNLOAD = -2,
    BS_UPDATE_ERR_RESPONSE = -3,
    BS_UPDATE_ERR_ASSET = -4,
    BS_UPDATE_ERR_CORRUPT = -5,
    BS_UPDATE_ERR_INSTALL = -6,
    BS_UPDATE_ERR_UNSUPPORTED = -7
};

typedef struct {
    char current_version[64];
    char latest_version[64];
    char asset_url[1024];
    char digest[65];
    long long asset_size;
    int error;
    char detail[256];
} bs_update_info;

int bs_update_auto_enabled(void);
int bs_update_set_auto_enabled(int enabled);
int bs_update_network_connected(void);
int bs_update_read_current(bs_update_info *info);
int bs_update_check(bs_update_info *info, int connect_if_needed);
int bs_update_install(bs_update_info *info);
int bs_update_restart(void);

/* Kept public for the small host-side parser check. */
int bs_update_parse_release(const char *json, bs_update_info *info);
int bs_update_version_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif

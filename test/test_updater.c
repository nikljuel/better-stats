#include "updater.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void stop_daemon(void) {}

static int network_state = 2;
static int failures_remaining;
static int download_count;
static int disconnect_count;
static int silent_connect_count;
static int prompt_connect_count;
static int silent_result;

int GetNetState(void)
{
    return network_state;
}

int NetDisconnect(void)
{
    ++disconnect_count;
    network_state = 0;
    return 0;
}

int NetConnectSilent(const char *name)
{
    assert(name == NULL);
    ++silent_connect_count;
    if (silent_result == 0)
        network_state = 2;
    return silent_result;
}

int NetConnect2(const char *name, int flags)
{
    assert(name == NULL);
    assert(flags == 0);
    ++prompt_connect_count;
    return -1;
}

int NetConnect(const char *name)
{
    assert(name == NULL);
    ++prompt_connect_count;
    return -1;
}

void *QuickDownloadExt3(const char *url, int *size, int timeout,
                        char *cookie, char *post, int *error)
{
    static const char response[] =
        "{\"tag_name\":\"v1.2.4\",\"draft\":false,\"prerelease\":false,"
        "\"assets\":[{\"name\":\"BetterStats-v1.2.4.zip\","
        "\"browser_download_url\":\"https://github.com/nikljuel/better-stats/"
        "releases/download/v1.2.4/BetterStats-v1.2.4.zip\",\"size\":123}]}";
    assert(strstr(url, "releases/latest") != NULL);
    assert(timeout == 20);
    assert(cookie == NULL);
    assert(post == NULL);
    ++download_count;
    *error = 0;
    if (failures_remaining > 0) {
        --failures_remaining;
        *size = 0;
        return NULL;
    }
    *size = (int)strlen(response);
    char *data = malloc((size_t)*size);
    assert(data != NULL);
    memcpy(data, response, (size_t)*size);
    return data;
}

int main(void)
{
    assert(bs_update_version_compare("v1.2.3", "v1.2.4") < 0);
    assert(bs_update_version_compare("v2.0.0", "v1.99.99") > 0);
    assert(bs_update_version_compare("v1.2.3", "v1.2.3") == 0);

    char json[2048];
    snprintf(json, sizeof(json),
        "{\"tag_name\":\"v1.1.0\",\"draft\":false,\"prerelease\":false,"
        "\"assets\":[{\"name\":\"BetterStats-v1.1.0.zip\","
        "\"browser_download_url\":\"https://github.com/nikljuel/better-stats/"
        "releases/download/v1.1.0/BetterStats-v1.1.0.zip\","
        "\"size\":123}]}");
    bs_update_info info = {0};
    assert(bs_update_parse_release(json, &info) == 0);
    assert(strcmp(info.latest_version, "v1.1.0") == 0);
    assert(info.asset_size == 123);

    memset(&info, 0, sizeof(info));
    assert(bs_update_parse_release("{\"tag_name\":\"v1.1.0\",\"assets\":[]}",
                                   &info) == BS_UPDATE_ERR_ASSET);

    mkdir("/tmp/bs_update_test", 0755);
    unlink("/tmp/bs_update_test/updates-disabled");
    assert(bs_update_auto_enabled());
    assert(bs_update_set_auto_enabled(0) == 0);
    assert(!bs_update_auto_enabled());
    assert(bs_update_set_auto_enabled(1) == 0);
    assert(bs_update_auto_enabled());

    setenv("BETTERSTATS_INSTALL_ROOT", "/tmp/bs_update_install", 1);
    mkdir("/tmp/bs_update_install", 0755);
    mkdir("/tmp/bs_update_install/applications", 0755);
    mkdir("/tmp/bs_update_install/applications/betterstats", 0755);
    FILE *current = fopen(
        "/tmp/bs_update_install/applications/betterstats/current", "w");
    assert(current != NULL);
    assert(fputs("v1.2.3\n", current) >= 0);
    assert(fclose(current) == 0);

    failures_remaining = 1;
    memset(&info, 0, sizeof(info));
    assert(bs_update_check(&info, BS_UPDATE_CONNECT_NONE)
           == BS_UPDATE_AVAILABLE);
    assert(download_count == 2);
    assert(disconnect_count == 1);
    assert(silent_connect_count == 1);
    assert(strcmp(info.latest_version, "v1.2.4") == 0);

    network_state = 0;
    download_count = 0;
    disconnect_count = 0;
    silent_connect_count = 0;
    memset(&info, 0, sizeof(info));
    assert(bs_update_check(&info, BS_UPDATE_CONNECT_SILENT)
           == BS_UPDATE_AVAILABLE);
    assert(download_count == 1);
    assert(disconnect_count == 0);
    assert(silent_connect_count == 1);
    assert(prompt_connect_count == 0);

    network_state = 0;
    download_count = 0;
    silent_connect_count = 0;
    silent_result = -1;
    memset(&info, 0, sizeof(info));
    assert(bs_update_check(&info, BS_UPDATE_CONNECT_SILENT)
           == BS_UPDATE_ERR_NETWORK);
    assert(download_count == 0);
    assert(silent_connect_count == 1);
    assert(prompt_connect_count == 0);

    puts("all updater tests ok");
    return 0;
}

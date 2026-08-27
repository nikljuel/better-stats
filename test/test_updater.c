#include "updater.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void stop_daemon(void) {}

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

    puts("all updater tests ok");
    return 0;
}

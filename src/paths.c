#include "daemon.h"

#include <stdlib.h>

const char *stats_db_path(void)
{
    const char *path = getenv("BETTERSTATS_DB");
    return path ? path : STATS_DB;
}

const char *explorer_db_path(void)
{
    const char *path = getenv("BETTERSTATS_EXPLORER_DB");
    return path ? path : EXPLORER_DB;
}

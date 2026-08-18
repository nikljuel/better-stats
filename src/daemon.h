#ifndef DAEMON_H
#define DAEMON_H

#define EXPLORER_DB "/mnt/ext1/system/explorer-3/explorer-3.db"
#define STATS_DIR "/mnt/ext1/system/pbreadstats"
#define STATS_DB STATS_DIR "/betterstats.db"
#define PIDFILE STATS_DIR "/betterstats.pid"
#define COVER_DIR "/mnt/ext1/system/cover_chache/hashed"

#ifdef __cplusplus
extern "C" {
#endif

int run_daemon(void);
void spawn_daemon(const char *self);
const char *stats_db_path(void);
const char *explorer_db_path(void);

#ifdef __cplusplus
}
#endif

#endif

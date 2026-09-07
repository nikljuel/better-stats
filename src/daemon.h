#ifndef DAEMON_H
#define DAEMON_H

#ifndef EXPLORER_DB
#define EXPLORER_DB "/mnt/ext1/system/explorer-3/explorer-3.db"
#endif
#ifndef STATS_DIR
#define STATS_DIR "/mnt/ext1/system/pbreadstats"
#endif
#define STATS_DB STATS_DIR "/betterstats.db"
#ifndef PIDFILE
#define PIDFILE "/tmp/betterstats.pid"
#endif
#ifndef LEGACY_PIDFILE
#define LEGACY_PIDFILE STATS_DIR "/betterstats.pid"
#endif
#ifndef COVER_DIR
#define COVER_DIR "/mnt/ext1/system/cover_chache/hashed"
#endif
/* Our own cover cache, keyed by the same "<storageid><hex fast_hash>" string
 * that books.cover keeps -- so a cached image stays reachable after the
 * firmware forgets the book. */
#ifndef COVER_CACHE_DIR
#define COVER_CACHE_DIR STATS_DIR "/covers"
#endif

#ifdef __cplusplus
extern "C" {
#endif

int run_daemon(void);
int stop_daemon(void);
const char *stats_db_path(void);
const char *explorer_db_path(void);

#ifdef __cplusplus
}
#endif

#endif

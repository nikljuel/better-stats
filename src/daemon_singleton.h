#ifndef DAEMON_SINGLETON_H
#define DAEMON_SINGLETON_H

#ifdef __cplusplus
extern "C" {
#endif

/* Claim the daemon singleton.
 *
 * Returns 1 and stores a lifetime lock descriptor in *lockfd when this
 * process owns the singleton, 0 when a validated current daemon already
 * owns it, and -1 on an unsafe or unrecoverable state. Keep the default
 * termination handlers while this may wait for a legacy daemon. */
int daemon_claim(int *lockfd);

/* Remove this process' compatibility hint and release the lifetime lock.
 * The primary pid file deliberately remains in place as stale content. */
void daemon_release(int lockfd);

/* Stop one validated daemon and wait until that exact process is gone. */
int stop_daemon(void);

#ifdef __cplusplus
}
#endif

#endif

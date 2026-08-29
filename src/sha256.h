#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buf[64];
} bs_sha256;

void bs_sha256_init(bs_sha256 *ctx);
void bs_sha256_update(bs_sha256 *ctx, const void *data, size_t len);
void bs_sha256_final(bs_sha256 *ctx, uint8_t out[32]);

/* Hash a file and write the 64-char lowercase hex digest to out[65]. */
int bs_sha256_file(const char *path, char out[65]);

/* Verify SHA256SUMS in `directory`: returns 1 on success, 0 on failure. */
int bs_sha256_check(const char *directory);

#endif

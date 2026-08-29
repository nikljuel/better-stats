#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (RR(x, 2) ^ RR(x, 13) ^ RR(x, 22))
#define EP1(x) (RR(x, 6) ^ RR(x, 11) ^ RR(x, 25))
#define SIG0(x) (RR(x, 7) ^ RR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (RR(x, 17) ^ RR(x, 19) ^ ((x) >> 10))

static void transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)block[i * 4] << 24
             | (uint32_t)block[i * 4 + 1] << 16
             | (uint32_t)block[i * 4 + 2] << 8
             | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; ++i)
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void bs_sha256_init(bs_sha256 *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void bs_sha256_update(bs_sha256 *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t buffered = (size_t)(ctx->count & 63);
    ctx->count += len;
    if (buffered && buffered + len >= 64) {
        size_t fill = 64 - buffered;
        memcpy(ctx->buf + buffered, p, fill);
        transform(ctx->state, ctx->buf);
        p += fill;
        len -= fill;
        buffered = 0;
    }
    while (len >= 64) {
        transform(ctx->state, p);
        p += 64;
        len -= 64;
    }
    if (len)
        memcpy(ctx->buf + buffered, p, len);
}

void bs_sha256_final(bs_sha256 *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    bs_sha256_update(ctx, &pad, 1);
    pad = 0;
    while ((ctx->count & 63) != 56)
        bs_sha256_update(ctx, &pad, 1);
    uint8_t len_be[8];
    for (int i = 7; i >= 0; --i) {
        len_be[i] = (uint8_t)bits;
        bits >>= 8;
    }
    bs_sha256_update(ctx, len_be, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

int bs_sha256_file(const char *path, char out[65])
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    bs_sha256 ctx;
    bs_sha256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
        bs_sha256_update(&ctx, buf, n);
    int ok = !ferror(file);
    fclose(file);
    if (!ok)
        return 0;
    uint8_t hash[32];
    bs_sha256_final(&ctx, hash);
    for (int i = 0; i < 32; ++i)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    return 1;
}

int bs_sha256_check(const char *directory)
{
    char sums_path[1024];
    int n = snprintf(sums_path, sizeof(sums_path), "%s/SHA256SUMS", directory);
    if (n < 0 || (size_t)n >= sizeof(sums_path))
        return 0;
    FILE *sums = fopen(sums_path, "r");
    if (!sums)
        return 0;
    char line[512];
    int checked = 0;
    while (fgets(line, (int)sizeof(line), sums)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!*line)
            continue;
        if (strlen(line) < 66 || line[64] != ' ')
            goto fail;
        char *filename = line + 65;
        if (*filename == ' ')
            ++filename;
        if (!*filename)
            goto fail;
        char file_path[1024];
        n = snprintf(file_path, sizeof(file_path), "%s/%s", directory, filename);
        if (n < 0 || (size_t)n >= sizeof(file_path))
            goto fail;
        char actual[65];
        if (!bs_sha256_file(file_path, actual))
            goto fail;
        line[64] = '\0';
        if (strcmp(line, actual) != 0)
            goto fail;
        ++checked;
    }
    fclose(sums);
    return checked > 0;
fail:
    fclose(sums);
    return 0;
}

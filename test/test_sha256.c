#include "sha256.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    /* NIST test vector: SHA-256("abc") */
    bs_sha256 ctx;
    bs_sha256_init(&ctx);
    bs_sha256_update(&ctx, "abc", 3);
    uint8_t hash[32];
    bs_sha256_final(&ctx, hash);
    char hex[65];
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    assert(strcmp(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    /* NIST test vector: SHA-256("") */
    bs_sha256_init(&ctx);
    bs_sha256_update(&ctx, "", 0);
    bs_sha256_final(&ctx, hash);
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    assert(strcmp(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    /* NIST test vector: SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") */
    bs_sha256_init(&ctx);
    bs_sha256_update(&ctx,
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    bs_sha256_final(&ctx, hash);
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    assert(strcmp(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    /* Test bs_sha256_file */
    char tmp[] = "/tmp/bs_sha256_test_XXXXXX";
    int fd = mkstemp(tmp);
    assert(fd >= 0);
    assert(write(fd, "abc", 3) == 3);
    close(fd);
    char file_hex[65];
    assert(bs_sha256_file(tmp, file_hex));
    assert(strcmp(file_hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    /* Test bs_sha256_check */
    char dir[] = "/tmp/bs_sha256_check_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char path[256];
    snprintf(path, sizeof(path), "%s/testfile", dir);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fprintf(f, "abc");
    fclose(f);
    snprintf(path, sizeof(path), "%s/SHA256SUMS", dir);
    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  testfile\n");
    fclose(f);
    assert(bs_sha256_check(dir) == 1);

    /* Tamper with the file and verify check fails */
    snprintf(path, sizeof(path), "%s/testfile", dir);
    f = fopen(path, "wb");
    assert(f != NULL);
    fprintf(f, "xyz");
    fclose(f);
    assert(bs_sha256_check(dir) == 0);

    unlink(tmp);
    snprintf(path, sizeof(path), "%s/testfile", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/SHA256SUMS", dir);
    unlink(path);
    rmdir(dir);

    puts("all sha256 tests ok");
    return 0;
}

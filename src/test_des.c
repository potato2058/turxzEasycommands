#include "des.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int expect_hex(const char *name, const uint8_t *got, size_t n, const char *hex)
{
    char buf[1024];
    if (n * 2 >= sizeof buf)
        return 1;
    for (size_t i = 0; i < n; i++)
        sprintf(buf + 2 * i, "%02x", got[i]);
    if (strcmp(buf, hex) != 0) {
        fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", name, buf, hex);
        return 1;
    }
    printf("ok %s\n", name);
    return 0;
}

int main(void)
{
    const uint8_t key[8] = { 's', 'l', 'v', '3', 't', 'u', 'z', 'x' };
    int fails = 0;

    uint8_t in8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t out8[8];
    des_cbc_encrypt(key, key, in8, out8, 8);
    fails += expect_hex("8-byte", out8, 8, "1fdddb31f898a83b");

    uint8_t in16[16];
    for (int i = 0; i < 16; i++)
        in16[i] = (uint8_t)i;
    uint8_t out16[16];
    des_cbc_encrypt(key, key, in16, out16, 16);
    fails += expect_hex("16-byte", out16, 16, "f8859f15bc14d88bb5601645ab4d7401");

    uint8_t in500[504];
    memset(in500, 0, sizeof in500);
    uint8_t out500[504];
    des_cbc_encrypt(key, key, in500, out500, 504);
    char head[33], tail[17];
    for (int i = 0; i < 16; i++)
        sprintf(head + 2 * i, "%02x", out500[i]);
    for (int i = 0; i < 8; i++)
        sprintf(tail + 2 * i, "%02x", out500[496 + i]);
    if (strcmp(head, "6e4c1df6aab4fde40d80344715d09168") != 0 ||
        strcmp(tail, "a98045f4bbcfa0bc") != 0) {
        fprintf(stderr, "FAIL 500-pad\n  head %s\n  tail %s\n", head, tail);
        fails++;
    } else {
        printf("ok 500-pad\n");
    }
    return fails ? 1 : 0;
}

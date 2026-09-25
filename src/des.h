#ifndef TURZX_DES_H
#define TURZX_DES_H

#include <stddef.h>
#include <stdint.h>

/* DES-CBC, no PKCS padding. Caller supplies a multiple of 8 bytes.
 * IV is provided separately (TURZX uses the key as the IV). */
void des_cbc_encrypt(const uint8_t key[8], const uint8_t iv[8],
                     const uint8_t *in, uint8_t *out, size_t len);

#endif

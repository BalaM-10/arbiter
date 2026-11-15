/*
 * The contract's source SHA-256 as 64 lowercase hex characters, for the one
 * startup line each trusted process prints (judge-review M1-J17: the bench
 * compares the full hash from both the guardian and the arm). Startup only;
 * pure, no OS call.
 */
#ifndef ARB_HEX_H
#define ARB_HEX_H

#include <stdint.h>

#define ARB_SHA256_HEX_LEN 65              /* 64 characters and the terminator */

static inline void arb_sha256_hex(const uint8_t sha[32], char out[ARB_SHA256_HEX_LEN])
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {          /* bound: 32 bytes */
        out[2 * i] = digits[sha[i] >> 4];
        out[2 * i + 1] = digits[sha[i] & 0x0fu];
    }
    out[64] = '\0';
}

#endif /* ARB_HEX_H */

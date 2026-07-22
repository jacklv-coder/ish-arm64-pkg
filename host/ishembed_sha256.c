/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ishembed_sha256.h"

#include <stdint.h>
#include <string.h>

#define SHA256_BLOCK_BYTES 64u
#define SHA256_DIGEST_BYTES 32u
#define SHA256_HEX_BYTES (SHA256_DIGEST_BYTES * 2u)

static const uint32_t sha256_round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t load_u32_be(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t words[64];
    for (size_t i = 0; i < 16; ++i)
        words[i] = load_u32_be(block + i * 4u);
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotate_right(words[i - 15], 7) ^
                      rotate_right(words[i - 15], 18) ^
                      (words[i - 15] >> 3);
        uint32_t s1 = rotate_right(words[i - 2], 17) ^
                      rotate_right(words[i - 2], 19) ^
                      (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (size_t i = 0; i < 64; ++i) {
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + sum1 + choose + sha256_round_constants[i] +
                         words[i];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256_digest(const uint8_t *bytes, size_t len,
                          uint8_t digest[SHA256_DIGEST_BYTES]) {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    size_t original_len = len;

    while (len >= SHA256_BLOCK_BYTES) {
        sha256_transform(state, bytes);
        bytes += SHA256_BLOCK_BYTES;
        len -= SHA256_BLOCK_BYTES;
    }

    uint8_t tail[SHA256_BLOCK_BYTES] = {0};
    if (len != 0)
        memcpy(tail, bytes, len);
    tail[len] = 0x80u;
    if (len >= 56u) {
        sha256_transform(state, tail);
        memset(tail, 0, sizeof(tail));
    }

    uint64_t bit_len = (uint64_t)original_len * 8u;
    for (size_t i = 0; i < 8; ++i)
        tail[63u - i] = (uint8_t)(bit_len >> (i * 8u));
    sha256_transform(state, tail);

    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4u] = (uint8_t)(state[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(state[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(state[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)state[i];
    }
}

static int sha256_hex_metadata_valid(const char *expected_hex) {
    if (expected_hex == NULL ||
        strnlen(expected_hex, SHA256_HEX_BYTES + 1u) != SHA256_HEX_BYTES)
        return 0;

    for (size_t i = 0; i < SHA256_HEX_BYTES; ++i) {
        unsigned char digit = (unsigned char)expected_hex[i];
        if (!((digit >= (unsigned char)'0' && digit <= (unsigned char)'9') ||
              (digit >= (unsigned char)'a' && digit <= (unsigned char)'f')))
            return 0;
    }
    return expected_hex[SHA256_HEX_BYTES] == '\0';
}

static int supervisor_path_matches_digest(const char *guest_path,
                                          const char *expected_hex) {
    static const char path_prefix[] = "/sbin/.ishsv-ishembed-sha256-";
    const size_t prefix_len = sizeof(path_prefix) - 1u;
    const size_t path_len = prefix_len + SHA256_HEX_BYTES;

    if (guest_path == NULL ||
        strnlen(guest_path, path_len + 1u) != path_len)
        return 0;
    for (size_t i = 0; i < prefix_len; ++i) {
        if (guest_path[i] != path_prefix[i])
            return 0;
    }
    for (size_t i = 0; i < SHA256_HEX_BYTES; ++i) {
        if (guest_path[prefix_len + i] != expected_hex[i])
            return 0;
    }
    return guest_path[path_len] == '\0';
}

int ish_embed_sha256_matches_hex(const uint8_t *bytes, size_t len,
                                 const char *expected_hex) {
    static const char hex_digits[] = "0123456789abcdef";
    if ((bytes == NULL && len != 0) ||
        !sha256_hex_metadata_valid(expected_hex))
        return 0;

    uint8_t digest[SHA256_DIGEST_BYTES];
    sha256_digest(bytes, len, digest);

    unsigned difference = 0;
    for (size_t i = 0; i < SHA256_DIGEST_BYTES; ++i) {
        difference |= (unsigned)(uint8_t)expected_hex[i * 2u] ^
                      (unsigned)(uint8_t)hex_digits[digest[i] >> 4];
        difference |= (unsigned)(uint8_t)expected_hex[i * 2u + 1u] ^
                      (unsigned)(uint8_t)hex_digits[digest[i] & 0x0fu];
    }
    return difference == 0;
}

int ish_embed_supervisor_metadata_valid(const uint8_t *bytes, size_t len,
                                        const char *expected_hex,
                                        const char *guest_path) {
    if (bytes == NULL || len == 0 ||
        !sha256_hex_metadata_valid(expected_hex) ||
        !supervisor_path_matches_digest(guest_path, expected_hex))
        return 0;
    return ish_embed_sha256_matches_hex(bytes, len, expected_hex);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/ishembed_sha256.h"

static int require_match(const char *name, const uint8_t *bytes, size_t len,
                         const char *expected) {
    if (ish_embed_sha256_matches_hex(bytes, len, expected))
        return 1;
    fprintf(stderr, "SHA-256 vector failed: %s\n", name);
    return 0;
}

int main(void) {
    static const uint8_t abc[] = "abc";
    static const uint8_t elf[] = {0x7f, 'E', 'L', 'F'};
    static const uint8_t multi_block[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    int ok = 1;

    ok &= require_match(
        "empty", NULL, 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ok &= require_match(
        "abc", abc, sizeof(abc) - 1u,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ok &= require_match(
        "multi-block", multi_block, sizeof(multi_block) - 1u,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    uint8_t padding_boundaries[65];
    for (size_t i = 0; i < sizeof(padding_boundaries); ++i)
        padding_boundaries[i] = (uint8_t)i;
    ok &= require_match(
        "55-byte-padding-boundary", padding_boundaries, 55u,
        "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59");
    ok &= require_match(
        "56-byte-padding-boundary", padding_boundaries, 56u,
        "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562");
    ok &= require_match(
        "63-byte-padding-boundary", padding_boundaries, 63u,
        "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488");
    ok &= require_match(
        "64-byte-block-boundary", padding_boundaries, 64u,
        "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108");
    ok &= require_match(
        "65-byte-block-boundary", padding_boundaries, 65u,
        "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781");

    uint8_t *million_a = malloc(1000000u);
    if (!million_a) {
        perror("malloc");
        return 1;
    }
    memset(million_a, 'a', 1000000u);
    ok &= require_match(
        "million-a", million_a, 1000000u,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(million_a);

    if (ish_embed_sha256_matches_hex(
            abc, sizeof(abc) - 1u,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae") ||
        ish_embed_sha256_matches_hex(
            abc, sizeof(abc) - 1u,
            "za7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ||
        ish_embed_sha256_matches_hex(abc, sizeof(abc) - 1u, "short") ||
        ish_embed_sha256_matches_hex(
            abc, sizeof(abc) - 1u,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad0") ||
        ish_embed_sha256_matches_hex(NULL, 1u,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) {
        fprintf(stderr, "SHA-256 malformed/mismatched metadata accepted\n");
        ok = 0;
    }

    static const char elf_digest[] =
        "3bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359";
    static const char elf_path[] =
        "/sbin/.ishsv-ishembed-sha256-3bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359";
    uint8_t tampered_elf[sizeof(elf)];
    memcpy(tampered_elf, elf, sizeof(tampered_elf));
    tampered_elf[0] ^= 0x01u;

    char uppercase_digest[sizeof(elf_digest)];
    memcpy(uppercase_digest, elf_digest, sizeof(uppercase_digest));
    uppercase_digest[1] = 'B';

    char overlong_digest[sizeof(elf_digest) + 1u];
    memcpy(overlong_digest, elf_digest, sizeof(elf_digest) - 1u);
    overlong_digest[sizeof(elf_digest) - 1u] = '0';
    overlong_digest[sizeof(elf_digest)] = '\0';

    if (ish_embed_sha256_matches_hex(
            elf, sizeof(elf), uppercase_digest) ||
        !ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), elf_digest, elf_path) ||
        ish_embed_supervisor_metadata_valid(
            tampered_elf, sizeof(tampered_elf), elf_digest, elf_path) ||
        ish_embed_supervisor_metadata_valid(
            elf, 0u, elf_digest, elf_path) ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), uppercase_digest, elf_path) ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), overlong_digest, elf_path) ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), "short", elf_path) ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), elf_digest, "/short") ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), elf_digest,
            "/tmp/.ishsv-ishembed-sha256-3bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359") ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), elf_digest,
            "/sbin/.ishsv-ishembed-sha256-0bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359") ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), elf_digest,
            "/sbin/.ishsv-ishembed-sha256-3bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359x") ||
        ish_embed_supervisor_metadata_valid(
            elf, sizeof(elf), elf_digest, NULL)) {
        fprintf(stderr, "supervisor digest/path binding test failed\n");
        ok = 0;
    }

    if (ok)
        fprintf(stderr, "SHA-256 known-answer and rejection tests: OK\n");
    return ok ? 0 : 1;
}

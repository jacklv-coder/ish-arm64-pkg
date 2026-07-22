/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ISHEMBED_SHA256_H
#define ISHEMBED_SHA256_H

#include <stddef.h>
#include <stdint.h>

#if defined(__clang__) || defined(__GNUC__)
#define ISH_EMBED_INTERNAL __attribute__((visibility("hidden")))
#else
#define ISH_EMBED_INTERNAL
#endif

/* Internal, dependency-free verifier used before installing generated blobs. */
ISH_EMBED_INTERNAL int ish_embed_sha256_matches_hex(
    const uint8_t *bytes, size_t len, const char *expected_hex);
ISH_EMBED_INTERNAL int ish_embed_supervisor_metadata_valid(
    const uint8_t *bytes, size_t len, const char *expected_hex,
    const char *guest_path);

#endif /* ISHEMBED_SHA256_H */

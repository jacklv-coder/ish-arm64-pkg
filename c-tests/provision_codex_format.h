/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ISH_PROVISION_CODEX_FORMAT_H
#define ISH_PROVISION_CODEX_FORMAT_H

#include <stddef.h>

/* The validated upper bound is currently 323 bytes:
 *   @ + 64-byte scope + / + 128-byte package + @ + 128-byte version/tag.
 * Keep headroom for future validation-policy changes, but still reject any
 * truncation instead of silently installing a different npm target. */
#define ISH_PROVISION_NPM_TARGET_CAPACITY 512u

int ish_provision_format_npm_target(char *dst, size_t capacity,
                                    const char *package,
                                    const char *version);

#endif /* ISH_PROVISION_CODEX_FORMAT_H */

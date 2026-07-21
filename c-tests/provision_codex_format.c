/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "provision_codex_format.h"

#include <stdio.h>

int ish_provision_format_npm_target(char *dst, size_t capacity,
                                    const char *package,
                                    const char *version) {
    if (!dst || capacity == 0 || !package || !version) return -1;

    int written = version[0]
        ? snprintf(dst, capacity, "%s@%s", package, version)
        : snprintf(dst, capacity, "%s", package);
    if (written < 0 || (size_t) written >= capacity) {
        dst[capacity - 1] = '\0';
        return -1;
    }
    return written;
}

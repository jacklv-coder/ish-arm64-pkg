/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "provision_codex_format.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char package[195];
    char version[129];
    char expected[324];
    char target[ISH_PROVISION_NPM_TARGET_CAPACITY];

    package[0] = '@';
    memset(package + 1, 's', 64);
    package[65] = '/';
    memset(package + 66, 'p', 128);
    package[194] = '\0';
    memset(version, 'v', 128);
    version[128] = '\0';

    int expected_length = snprintf(expected, sizeof(expected), "%s@%s",
                                   package, version);
    assert(expected_length == 323);
    assert(ish_provision_format_npm_target(target, sizeof(target), package,
                                           version) == expected_length);
    assert(strcmp(target, expected) == 0);

    /* 323 payload bytes require a 324-byte buffer including the NUL. */
    char truncated[323];
    memset(truncated, 'x', sizeof(truncated));
    assert(ish_provision_format_npm_target(truncated, sizeof(truncated),
                                           package, version) == -1);
    assert(truncated[sizeof(truncated) - 1] == '\0');

    assert(ish_provision_format_npm_target(target, sizeof(target), package,
                                           "") == 194);
    assert(strcmp(target, package) == 0);
    assert(ish_provision_format_npm_target(NULL, sizeof(target), package,
                                           version) == -1);
    assert(ish_provision_format_npm_target(target, 0, package, version) == -1);

    puts("provision Codex npm target formatting: OK");
    return 0;
}

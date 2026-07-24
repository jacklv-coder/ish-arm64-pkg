#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=release-version-policy.sh
source "$PKG_ROOT/scripts/release-version-policy.sh"

ish_release_stage1_version_allowed v0.4.0-abi.4 || {
    printf 'error: authorized Stage1 maintenance tag was rejected\n' >&2
    exit 1
}
for forbidden in v0.4.0 v0.4.0-abi.1 v0.4.0-abi.2 v0.4.0-abi.3 v0.4.0-abi.5 v0.4.0-rc.1 v1.2.3; do
    if ish_release_stage1_version_allowed "$forbidden"; then
        printf 'error: Stage1 policy unexpectedly allowed %s\n' "$forbidden" >&2
        exit 1
    fi
done

assert_prerelease_flag() {
    local version="$1"
    local expected="$2"
    local actual
    actual="$(ish_release_github_prerelease "$version")"
    [[ "$actual" == "$expected" ]] || {
        printf 'error: %s: expected prerelease=%s, found %s\n' \
            "$version" "$expected" "$actual" >&2
        exit 1
    }
}

assert_prerelease_flag v0.4.0-abi.4 true
assert_prerelease_flag v1.2.3-rc.1 true
assert_prerelease_flag v0.4.0 false
assert_prerelease_flag v1.2.3 false

abi_notes="$(ish_release_abi_transition_notes v0.4.0-abi.4)"
for expected in \
    '这不是稳定 v0.4' \
    'This is not stable v0.4' \
    'retain/release' \
    'joinable kernel thread' \
    'soft-halt' \
    'wire protocol v4' \
    'SIGUSR1' \
    'guest task threads' \
    'blocking host syscalls' \
    'Swift source remains' \
    'Stage2' \
    'does not contain a RootFS'; do
    [[ "$abi_notes" == *"$expected"* ]] || {
        printf 'error: ABI-transition notes are missing %s\n' "$expected" >&2
        exit 1
    }
done

for version in v0.4.0 v0.4.0-rc.1 v1.2.3-beta.2; do
    [[ -z "$(ish_release_abi_transition_notes "$version")" ]] || {
        printf 'error: %s unexpectedly received ABI-transition notes\n' "$version" >&2
        exit 1
    }
done

printf 'Release SemVer/GitHub prerelease policy tests passed.\n'

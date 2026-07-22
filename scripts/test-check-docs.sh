#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-docs-test.XXXXXX")"
trap 'rm -rf -- "$TEST_ROOT"' EXIT

make_fixture() {
    local fixture_root="$1"

    mkdir -p "$fixture_root/docs"
    printf '%s\n' \
        '# 中文说明' \
        '' \
        '[English](README.en.md)' \
        '' \
        '[变更记录][changes]' \
        '' \
        '[changes]: CHANGELOG.md' \
        >"$fixture_root/README.md"
    printf '%s\n' \
        '# English README' \
        '' \
        '[简体中文](README.md)' \
        >"$fixture_root/README.en.md"
    printf '%s\n' '# Notice' >"$fixture_root/NOTICE.md"
    printf '%s\n' \
        '# 变更日志' \
        '' \
        '[English](CHANGELOG.en.md)' \
        >"$fixture_root/CHANGELOG.md"
    printf '%s\n' \
        '# Changelog' \
        '' \
        '[简体中文](CHANGELOG.md)' \
        >"$fixture_root/CHANGELOG.en.md"
    printf '%s\n' \
        '# 指南' \
        '' \
        '[English](guide.en.md)' \
        >"$fixture_root/docs/guide.md"
    printf '%s\n' \
        '# Guide' \
        '' \
        '[简体中文](guide.md)' \
        >"$fixture_root/docs/guide.en.md"
}

expect_failure() {
    local fixture_root="$1"
    local expected_path="$2"
    local expected_error="$3"
    local output
    local status

    set +e
    output="$("$PKG_ROOT/scripts/check-docs.sh" --root "$fixture_root" 2>&1)"
    status=$?
    set -e

    if (( status == 0 )); then
        printf 'test-check-docs: expected failure for %s\n' "$expected_path" >&2
        exit 1
    fi
    if [[ "$output" != *"$expected_path"* || "$output" != *"$expected_error"* ]]; then
        printf 'test-check-docs: unexpected diagnostic:\n%s\n' "$output" >&2
        exit 1
    fi
}

baseline="$TEST_ROOT/baseline"
broken_link="$TEST_ROOT/broken-link"
undefined_reference="$TEST_ROOT/undefined-reference"
missing_changelog_mirror="$TEST_ROOT/missing-changelog-mirror"

make_fixture "$baseline"
"$PKG_ROOT/scripts/check-docs.sh" --root "$baseline" >/dev/null

mkdir -p "$broken_link"
cp -R "$baseline/." "$broken_link/"
printf '\n[missing](missing.md)\n' >>"$broken_link/NOTICE.md"
expect_failure "$broken_link" \
    'NOTICE.md' \
    'relative link target is missing'

mkdir -p "$undefined_reference"
cp -R "$baseline/." "$undefined_reference/"
printf '\n[missing release notes][not-defined]\n' \
    >>"$undefined_reference/CHANGELOG.en.md"
expect_failure "$undefined_reference" \
    'CHANGELOG.en.md' \
    'undefined reference-style link'

mkdir -p "$missing_changelog_mirror"
cp -R "$baseline/." "$missing_changelog_mirror/"
rm -f "$missing_changelog_mirror/CHANGELOG.en.md"
expect_failure "$missing_changelog_mirror" \
    'CHANGELOG.en.md' \
    'missing English document'

printf 'Documentation negative-fixture tests passed.\n'

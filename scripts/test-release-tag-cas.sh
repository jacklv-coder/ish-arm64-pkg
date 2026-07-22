#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_SCRIPT="$PKG_ROOT/scripts/release.sh"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

for command_name in git mktemp awk python3; do
    command -v "$command_name" >/dev/null 2>&1 \
        || fail "required command not found: $command_name"
done

# A GitHub draft's temporary tag_name is metadata, not proof that this release
# transaction owns a same-named Git ref. Guard against ever passing that name to
# the tag-deletion helper, including a call split across shell line continuations.
python3 - "$RELEASE_SCRIPT" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
unsafe_call = re.compile(
    r'''delete_owned_remote_tag\s+(?:\\\s*)?["']?\$(?:\{TEMP_RELEASE_TAG\}|TEMP_RELEASE_TAG)'''
)
if unsafe_call.search(source):
    raise SystemExit(
        "release.sh must not delete TEMP_RELEASE_TAG without an owned raw OID"
    )
PY

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-tag-cas.XXXXXX")"
REMOTE="$TEST_ROOT/remote.git"
LOCAL="$TEST_ROOT/local"
TAG="v9.9.9-release-cas-test"
TAG_REF="refs/tags/$TAG"

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    rm -rf "$TEST_ROOT"
    exit "$rc"
}
trap cleanup EXIT INT TERM HUP

git init --quiet --bare "$REMOTE"
git init --quiet "$LOCAL"
git -C "$LOCAL" config user.name "IshEmbed release test"
git -C "$LOCAL" config user.email "release-test@example.invalid"
git -C "$LOCAL" commit --quiet --allow-empty -m "release test"
git -C "$LOCAL" remote add origin "$REMOTE"

COMMIT="$(git -C "$LOCAL" rev-parse HEAD)"

make_tag_object() {
    local transaction="$1"
    {
        printf 'object %s\n' "$COMMIT"
        printf 'type commit\n'
        printf 'tag %s\n' "$TAG"
        printf 'tagger IshEmbed release test <release-test@example.invalid> 1700000000 +0000\n\n'
        printf '%s\n\n' "$TAG"
        printf 'IshEmbed release transaction: %s\n' "$transaction"
    } | git -C "$LOCAL" mktag
}

raw_remote_tag() {
    git -C "$LOCAL" ls-remote origin "$TAG_REF" \
        | awk -v wanted="$TAG_REF" '$2 == wanted { print $1; exit }'
}

peeled_remote_tag() {
    git -C "$LOCAL" ls-remote --tags origin "$TAG_REF*" \
        | awk -v direct="$TAG_REF" -v peeled="$TAG_REF^{}" '
            $2 == direct { direct_sha = $1 }
            $2 == peeled { peeled_sha = $1 }
            END {
                if (peeled_sha != "") print peeled_sha
                else if (direct_sha != "") print direct_sha
            }
        '
}

TAG_OBJECT_A="$(make_tag_object transaction-a)"
TAG_OBJECT_B="$(make_tag_object transaction-b)"
[[ "$TAG_OBJECT_A" != "$TAG_OBJECT_B" ]] \
    || fail "transaction-specific annotated tag objects are not unique"
[[ "$(git -C "$LOCAL" rev-parse "$TAG_OBJECT_A^{}")" == "$COMMIT" && \
   "$(git -C "$LOCAL" rev-parse "$TAG_OBJECT_B^{}")" == "$COMMIT" ]] \
    || fail "annotated tag objects do not peel to the release commit"

REF_A="refs/ishembed-release/test/object-a"
REF_B="refs/ishembed-release/test/object-b"
git -C "$LOCAL" update-ref "$REF_A" "$TAG_OBJECT_A" ""
git -C "$LOCAL" update-ref "$REF_B" "$TAG_OBJECT_B" ""

git -C "$LOCAL" push --quiet \
    --force-with-lease="$TAG_REF:" origin "$REF_A:$TAG_REF"
[[ "$(raw_remote_tag)" == "$TAG_OBJECT_A" && \
   "$(peeled_remote_tag)" == "$COMMIT" ]] \
    || fail "the first absent-ref CAS did not publish the exact owned tag object"

if git -C "$LOCAL" push --quiet \
    --force-with-lease="$TAG_REF:" origin "$REF_B:$TAG_REF" 2>/dev/null; then
    fail "a competing absent-ref CAS unexpectedly succeeded"
fi
[[ "$(raw_remote_tag)" == "$TAG_OBJECT_A" && \
   "$(peeled_remote_tag)" == "$COMMIT" ]] \
    || fail "a rejected competing CAS changed the existing tag"

if git -C "$LOCAL" push --quiet \
    --force-with-lease="$TAG_REF:$TAG_OBJECT_B" origin ":$TAG_REF" 2>/dev/null; then
    fail "rollback with the wrong raw tag-object lease unexpectedly succeeded"
fi
[[ "$(raw_remote_tag)" == "$TAG_OBJECT_A" ]] \
    || fail "rollback with the wrong raw tag-object lease changed the tag"

git -C "$LOCAL" push --quiet \
    --force-with-lease="$TAG_REF:$TAG_OBJECT_A" origin ":$TAG_REF"
[[ -z "$(raw_remote_tag)" ]] \
    || fail "rollback with the exact raw tag-object lease did not delete the tag"

printf 'Release tag CAS and rollback ownership tests passed.\n'

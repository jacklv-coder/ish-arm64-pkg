#!/usr/bin/env bash
# Build, stage, verify, and publish an IshEmbed GitHub Release.
#
# The final SemVer tag is reserved with an absent-ref Git CAS only after every
# draft asset is verified, then the draft is published immediately. This leaves
# a short tag-visible/draft-private interval but prevents attachment to a
# concurrently created tag that points at a different commit.

set -euo pipefail

readonly DEFAULT_RELEASE_REPO="jacklv-coder/ish-arm64-pkg"
readonly XCF_ASSET_NAME="libIshKernel.xcframework.zip"
readonly SOURCE_ASSET_NAME="IshEmbed-corresponding-source.tar.gz"
readonly GITHUB_HOST="github.com"
readonly VERIFY_ATTEMPTS=7

usage() {
    cat <<'EOF'
Usage: scripts/release.sh [options] vX.Y.Z

Options:
  --repo OWNER/REPO    GitHub repository (default: jacklv-coder/ish-arm64-pkg)
  --remote NAME        SSH Git remote (default: origin)
  -h, --help           Show this help

Environment equivalents:
  ISH_RELEASE_REPO     OWNER/REPO
  ISH_RELEASE_REMOTE   Git remote name

Every release contains libIshKernel.xcframework.zip and its matching
IshEmbed-corresponding-source.tar.gz. This script never publishes a RootFS;
RootFS provenance, hashing, licensing, and distribution remain a separate flow.
The Stage1 policy accepts only v0.4.0-abi.14 and publishes it as a prerelease.
A suffix-free stable v0.4.0 remains blocked until the Stage2 policy replaces
this transition gate.
EOF
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

retry_delay() {
    local attempt="$1"
    local delay=$((1 << (attempt - 1)))
    ((delay <= 16)) || delay=16
    sleep "$delay"
}

resolve_zig_lib_dir() {
    zig env | python3 -c '
import json
import re
import sys

raw = sys.stdin.read()
try:
    value = json.loads(raw)["lib_dir"]
except (json.JSONDecodeError, KeyError, TypeError):
    match = re.search(r"(?:[.]lib_dir|\"lib_dir\")\s*[:=]\s*\"([^\"]+)\"", raw)
    if match is None:
        raise SystemExit("could not locate lib_dir in zig env output")
    value = match.group(1)
print(value)
'
}

VERSION=""
RELEASE_REPO="${ISH_RELEASE_REPO:-$DEFAULT_RELEASE_REPO}"
GIT_REMOTE="${ISH_RELEASE_REMOTE:-origin}"

while (($# > 0)); do
    case "$1" in
        --repo)
            (($# >= 2)) || fail "--repo requires OWNER/REPO"
            RELEASE_REPO="$2"
            shift 2
            ;;
        --remote)
            (($# >= 2)) || fail "--remote requires a remote name"
            GIT_REMOTE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            fail "unknown option: $1"
            ;;
        *)
            [[ -z "$VERSION" ]] || fail "only one version may be specified"
            VERSION="$1"
            shift
            ;;
    esac
done

[[ -n "$VERSION" ]] || { usage >&2; exit 2; }
[[ "$VERSION" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$ ]] \
    || fail "version must be strict SemVer with a v prefix, such as v1.2.3 or v1.2.3-rc.1"
VERSION_POLICY="$(cd "$(dirname "$0")" && pwd)/release-version-policy.sh"
[[ -r "$VERSION_POLICY" ]] \
    || fail "release version policy is missing: $VERSION_POLICY"
# shellcheck source=release-version-policy.sh
source "$VERSION_POLICY"
ish_release_stage1_version_allowed "$VERSION" \
    || fail "Stage1 release policy allows only v0.4.0-abi.14; stable v0.4.0 requires Stage2"
GITHUB_PRERELEASE="$(ish_release_github_prerelease "$VERSION")"
if [[ "$VERSION" == *-* ]]; then
    PRERELEASE="${VERSION#*-}"
    IFS='.' read -r -a PRERELEASE_IDENTIFIERS <<< "$PRERELEASE"
    for identifier in "${PRERELEASE_IDENTIFIERS[@]}"; do
        if [[ "$identifier" =~ ^0[0-9]+$ ]]; then
            fail "numeric prerelease identifiers must not contain leading zeroes: $identifier"
        fi
    done
fi
[[ "$RELEASE_REPO" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] \
    || fail "repository must use OWNER/REPO form"

for command_name in git gh swift zip shasum mktemp python3 curl awk sed zig tar cmp; do
    require_command "$command_name"
done
git check-ref-format "refs/tags/$VERSION" >/dev/null \
    || fail "version is not a valid Git tag name: $VERSION"

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GH_REPO="$GITHUB_HOST/$RELEASE_REPO"

git -C "$PKG_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || fail "$PKG_ROOT is not a Git worktree"
[[ -z "$(git -C "$PKG_ROOT" status --porcelain --untracked-files=normal)" ]] \
    || fail "worktree must be clean before releasing"

github_repo_from_ssh_url() {
    local url="$1"
    local path=""
    case "$url" in
        git@github.com:*)
            path="${url#git@github.com:}"
            ;;
        ssh://git@github.com/*)
            path="${url#ssh://git@github.com/}"
            ;;
        ssh://git@github.com:*/*)
            path="${url#ssh://git@github.com:}"
            path="${path#*/}"
            ;;
        ssh://git@ssh.github.com/*)
            path="${url#ssh://git@ssh.github.com/}"
            ;;
        ssh://git@ssh.github.com:*/*)
            path="${url#ssh://git@ssh.github.com:}"
            path="${path#*/}"
            ;;
        *)
            return 1
            ;;
    esac
    printf '%s\n' "${path%.git}"
}

validate_remote_url() {
    local kind="$1"
    local url="$2"
    local repo
    repo="$(github_repo_from_ssh_url "$url")" \
        || fail "$kind URL must use GitHub SSH, got: $url"
    [[ "$repo" == "$RELEASE_REPO" ]] \
        || fail "$kind URL targets '$repo', not configured repo '$RELEASE_REPO'"
}

FETCH_URLS="$(git -C "$PKG_ROOT" remote get-url --all "$GIT_REMOTE")" \
    || fail "Git remote does not exist: $GIT_REMOTE"
PUSH_URLS="$(git -C "$PKG_ROOT" remote get-url --push --all "$GIT_REMOTE")" \
    || fail "Git remote has no push URL: $GIT_REMOTE"
[[ -n "$FETCH_URLS" ]] || fail "Git remote has no fetch URL: $GIT_REMOTE"
[[ -n "$PUSH_URLS" ]] || fail "Git remote has no push URL: $GIT_REMOTE"
while IFS= read -r remote_url; do
    [[ -n "$remote_url" ]] && validate_remote_url "fetch" "$remote_url"
done <<< "$FETCH_URLS"
while IFS= read -r remote_url; do
    [[ -n "$remote_url" ]] && validate_remote_url "push" "$remote_url"
done <<< "$PUSH_URLS"

gh auth status --hostname "$GITHUB_HOST" >/dev/null
DEFAULT_BRANCH="$(gh api --hostname "$GITHUB_HOST" \
    "repos/$RELEASE_REPO" --jq '.default_branch')"
[[ -n "$DEFAULT_BRANCH" ]] || fail "could not determine the repository default branch"
CURRENT_BRANCH="$(git -C "$PKG_ROOT" symbolic-ref --quiet --short HEAD)" \
    || fail "release from a branch, not a detached HEAD"
[[ "$CURRENT_BRANCH" == "$DEFAULT_BRANCH" ]] \
    || fail "release must run from default branch '$DEFAULT_BRANCH' (current: '$CURRENT_BRANCH')"

# Resolve either a lightweight tag or the commit peeled from an annotated tag.
# `git ls-remote <exact-ref>^{}` does not match reliably, so query the prefix
# and select the exact direct/peeled ref names from the returned records.
remote_tag_commit() {
    local tag="$1"
    local refs direct peeled
    direct="refs/tags/$tag"
    peeled="$direct^{}"
    refs="$(git -C "$PKG_ROOT" ls-remote --tags "$GIT_REMOTE" "$direct*")" \
        || return 1
    printf '%s\n' "$refs" | awk -v direct="$direct" -v peeled="$peeled" '
        $2 == direct { direct_sha = $1 }
        $2 == peeled { peeled_sha = $1 }
        END {
            if (peeled_sha != "") print peeled_sha
            else if (direct_sha != "") print direct_sha
        }
    '
}

remote_ref_commit() {
    local ref="$1"
    local refs
    refs="$(git -C "$PKG_ROOT" ls-remote "$GIT_REMOTE" "$ref")" || return 1
    printf '%s\n' "$refs" | awk -v wanted="$ref" '$2 == wanted { print $1; exit }'
}

FINAL_TAG_COMMIT="$(remote_tag_commit "$VERSION")" \
    || fail "could not query remote tag $VERSION"
[[ -z "$FINAL_TAG_COMMIT" ]] || fail "remote tag already exists: $VERSION"
if git -C "$PKG_ROOT" show-ref --verify --quiet "refs/tags/$VERSION"; then
    fail "local tag already exists: $VERSION"
fi

EXISTING_RELEASE_ID="$(gh api --hostname "$GITHUB_HOST" --paginate \
    "repos/$RELEASE_REPO/releases?per_page=100" \
    --jq ".[] | select(.tag_name == \"$VERSION\") | .id")"
[[ -z "$EXISTING_RELEASE_ID" ]] || fail "GitHub release already exists: $VERSION"

git -C "$PKG_ROOT" fetch "$GIT_REMOTE" \
    "refs/heads/$DEFAULT_BRANCH:refs/remotes/$GIT_REMOTE/$DEFAULT_BRANCH"
START_HEAD="$(git -C "$PKG_ROOT" rev-parse HEAD)"
REMOTE_HEAD="$(git -C "$PKG_ROOT" rev-parse "refs/remotes/$GIT_REMOTE/$DEFAULT_BRANCH")"
[[ "$START_HEAD" == "$REMOTE_HEAD" ]] \
    || fail "local $DEFAULT_BRANCH must exactly match $GIT_REMOTE/$DEFAULT_BRANCH"

[[ -x "$PKG_ROOT/scripts/build-ios.sh" ]] \
    || fail "scripts/build-ios.sh is missing or not executable"
[[ -x "$PKG_ROOT/scripts/package-source.sh" ]] \
    || fail "scripts/package-source.sh is missing or not executable"
[[ -x "$PKG_ROOT/scripts/verify-ios-artifact.sh" ]] \
    || fail "scripts/verify-ios-artifact.sh is missing or not executable"
[[ -x "$PKG_ROOT/scripts/test-swift-ios.sh" ]] \
    || fail "scripts/test-swift-ios.sh is missing or not executable"

printf '==> Testing current Swift API against the published manifest binary\n'
"$PKG_ROOT/scripts/test-swift-ios.sh" --manifest-binary

LOCAL_ISH_REPO="$PKG_ROOT/third_party/ish"
ISH_ENTRY="$(git -C "$PKG_ROOT" ls-tree "$START_HEAD" -- third_party/ish)"
read -r ISH_MODE ISH_TYPE PINNED_ISH_COMMIT ISH_PATH <<< "$ISH_ENTRY"
[[ "$ISH_MODE" == 160000 && "$ISH_TYPE" == commit && \
   "$ISH_PATH" == third_party/ish && "$PINNED_ISH_COMMIT" =~ ^[0-9a-f]{40}$ ]] \
    || fail "third_party/ish is not a pinned gitlink at $START_HEAD"
[[ -e "$LOCAL_ISH_REPO/.git" ]] \
    || fail "third_party/ish is not initialized"
git -C "$LOCAL_ISH_REPO" cat-file -e "$PINNED_ISH_COMMIT^{commit}" 2>/dev/null \
    || fail "pinned iSH commit is missing locally: $PINNED_ISH_COMMIT"

STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ishembed-release.XXXXXX")"
# Git canonicalizes macOS /var paths to /private/var in worktree metadata.
# Canonicalize once so registration checks and cleanup compare the same path.
STAGING_DIR="$(cd "$STAGING_DIR" && pwd -P)"
STAGING_TOKEN="$(basename "$STAGING_DIR" | sed 's/^ishembed-release\.//')"
TEMP_RELEASE_TAG="ishembed-staging-${VERSION#v}-$STAGING_TOKEN"
STAGING_REF="refs/heads/release-staging/${VERSION#v}-$STAGING_TOKEN"
RELEASE_WORKTREE="$STAGING_DIR/worktree"
RELEASE_BUILD_DIR="$STAGING_DIR/build"
RELEASE_OUT_DIR="$STAGING_DIR/xcframework"
XCF="$RELEASE_OUT_DIR/libIshKernel.xcframework"

RELEASE_COMMIT=""
STAGING_PUSH_ATTEMPTED=0
STAGING_PUSH_CONFIRMED=0
DRAFT_CREATE_ATTEMPTED=0
DRAFT_ID=""
DRAFT_VERIFIED=0
FINAL_TAG_OBJECT=""
FINAL_TAG_RAW_OID=""
LOCAL_TAG_OBJECT_REF="refs/ishembed-release/$STAGING_TOKEN/final-tag-object"
LOCAL_TAG_OBJECT_REF_CREATED=0
FINAL_TAG_PUSH_ATTEMPTED=0
FINAL_TAG_PUSHED=0
PUBLISH_ATTEMPTED=0
PUBLISHED_VERIFIED=0
SUCCESS=0

remove_release_worktree() {
    local registered
    registered="$(git -C "$PKG_ROOT" worktree list --porcelain | \
        awk -v wanted="$RELEASE_WORKTREE" '
            $1 == "worktree" {
                path = $0
                sub(/^worktree /, "", path)
                if (path == wanted) print path
            }
        ')" || return 1
    [[ -z "$registered" ]] && return 0
    git -C "$PKG_ROOT" worktree remove --force --force "$RELEASE_WORKTREE"
}

early_rollback() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    if remove_release_worktree; then
        rm -rf "$STAGING_DIR"
    else
        printf 'Release preparation failed; temporary worktree and assets were retained at %s.\n' \
            "$STAGING_DIR" >&2
        ((rc != 0)) || rc=1
    fi
    exit "$rc"
}
trap early_rollback EXIT INT TERM HUP

BUILD_ZIG_EXECUTABLE="$(command -v zig)"
BUILD_ZIG_EXECUTABLE="$(cd "$(dirname "$BUILD_ZIG_EXECUTABLE")" && pwd)/$(basename "$BUILD_ZIG_EXECUTABLE")"
BUILD_ZIG_SHA256="$(shasum -a 256 "$BUILD_ZIG_EXECUTABLE" | awk '{print $1}')"
BUILD_ZIG_VERSION="$(zig version)"
BUILD_ZIG_LIB_DIR="$(resolve_zig_lib_dir)" \
    || fail "could not determine Zig library directory"
BUILD_ZIG_LIB_DIR="$(cd "$BUILD_ZIG_LIB_DIR" 2>/dev/null && pwd)" \
    || fail "Zig library directory does not exist: $BUILD_ZIG_LIB_DIR"
BUILD_MUSL_SOURCE="$BUILD_ZIG_LIB_DIR/libc/musl"
[[ -f "$BUILD_MUSL_SOURCE/COPYRIGHT" ]] \
    || fail "Zig musl source tree is incomplete: $BUILD_MUSL_SOURCE"
BUILD_MUSL_SHA256="$("$PKG_ROOT/scripts/package-source.sh" \
    --print-musl-sha256 "$BUILD_MUSL_SOURCE")"

printf '==> Rebuilding XCFramework from clean commit %s\n' "$START_HEAD"
printf '    Zig %s (%s)\n' "$BUILD_ZIG_VERSION" "$BUILD_ZIG_SHA256"
printf '    musl %s\n' "$BUILD_MUSL_SHA256"
git -C "$PKG_ROOT" worktree add --detach "$RELEASE_WORKTREE" "$START_HEAD" >/dev/null
git -C "$RELEASE_WORKTREE" \
    -c protocol.file.allow=always \
    -c "submodule.third_party/ish.url=$LOCAL_ISH_REPO" \
    submodule update --init --checkout -- third_party/ish
[[ "$(git -C "$RELEASE_WORKTREE/third_party/ish" rev-parse HEAD)" == "$PINNED_ISH_COMMIT" ]] \
    || fail "isolated build worktree checked out the wrong iSH revision"
[[ -z "$(git -C "$RELEASE_WORKTREE" status --porcelain --untracked-files=normal)" ]] \
    || fail "isolated build worktree is not clean before rebuilding"

ZIG="$BUILD_ZIG_EXECUTABLE" \
BUILD_DIR="$RELEASE_BUILD_DIR" \
OUT_DIR="$RELEASE_OUT_DIR" \
    "$RELEASE_WORKTREE/scripts/build-ios.sh"

[[ "$(git -C "$PKG_ROOT" rev-parse HEAD)" == "$START_HEAD" && \
   -z "$(git -C "$PKG_ROOT" status --porcelain --untracked-files=normal)" ]] \
    || fail "source worktree changed while rebuilding the XCFramework"
[[ "$(git -C "$RELEASE_WORKTREE" rev-parse HEAD)" == "$START_HEAD" && \
   -z "$(git -C "$RELEASE_WORKTREE" status --porcelain --untracked-files=normal)" ]] \
    || fail "isolated source worktree changed while rebuilding the XCFramework"
POSTBUILD_REMOTE_HEAD="$(remote_ref_commit "refs/heads/$DEFAULT_BRANCH")" \
    || fail "could not recheck the remote default branch after rebuilding"
[[ "$POSTBUILD_REMOTE_HEAD" == "$START_HEAD" ]] \
    || fail "remote default branch changed while rebuilding"

CURRENT_ZIG_EXECUTABLE="$(command -v zig)"
CURRENT_ZIG_EXECUTABLE="$(cd "$(dirname "$CURRENT_ZIG_EXECUTABLE")" && pwd)/$(basename "$CURRENT_ZIG_EXECUTABLE")"
CURRENT_ZIG_SHA256="$(shasum -a 256 "$CURRENT_ZIG_EXECUTABLE" | awk '{print $1}')"
CURRENT_ZIG_VERSION="$(zig version)"
CURRENT_ZIG_LIB_DIR="$(resolve_zig_lib_dir)" \
    || fail "could not recheck Zig library directory after rebuilding"
CURRENT_ZIG_LIB_DIR="$(cd "$CURRENT_ZIG_LIB_DIR" 2>/dev/null && pwd)" \
    || fail "Zig library directory disappeared after rebuilding"
CURRENT_MUSL_SHA256="$("$PKG_ROOT/scripts/package-source.sh" \
    --print-musl-sha256 "$CURRENT_ZIG_LIB_DIR/libc/musl")"
[[ "$CURRENT_ZIG_EXECUTABLE" == "$BUILD_ZIG_EXECUTABLE" && \
   "$CURRENT_ZIG_SHA256" == "$BUILD_ZIG_SHA256" && \
   "$CURRENT_ZIG_VERSION" == "$BUILD_ZIG_VERSION" && \
   "$CURRENT_ZIG_LIB_DIR" == "$BUILD_ZIG_LIB_DIR" && \
   "$CURRENT_MUSL_SHA256" == "$BUILD_MUSL_SHA256" ]] \
    || fail "Zig or musl changed while rebuilding the XCFramework"

[[ -d "$XCF" ]] || fail "scripts/build-ios.sh did not create $XCF"
[[ -f "$XCF/Info.plist" ]] || fail "rebuilt XCFramework is missing Info.plist"
BUILD_DIR="$RELEASE_BUILD_DIR" XCF="$XCF" \
    "$RELEASE_WORKTREE/scripts/verify-ios-artifact.sh"
"$PKG_ROOT/scripts/test-swift-ios.sh" "$XCF"
[[ "$(git -C "$PKG_ROOT" rev-parse HEAD)" == "$START_HEAD" && \
   -z "$(git -C "$PKG_ROOT" status --porcelain --untracked-files=normal)" && \
   "$(git -C "$RELEASE_WORKTREE" rev-parse HEAD)" == "$START_HEAD" && \
   -z "$(git -C "$RELEASE_WORKTREE" status --porcelain --untracked-files=normal)" ]] \
    || fail "source worktree changed while validating the isolated XCFramework"

delete_owned_remote_ref() {
    local ref="$1"
    local expected_commit="$2"
    local raw_oid
    raw_oid="$(remote_ref_commit "$ref")" || return 1
    [[ -z "$raw_oid" ]] && return 0
    [[ "$raw_oid" == "$expected_commit" ]] || {
        printf 'refusing to delete %s: expected %s, found %s\n' \
            "$ref" "$expected_commit" "$raw_oid" >&2
        return 1
    }
    git -C "$PKG_ROOT" push \
        --force-with-lease="$ref:$raw_oid" "$GIT_REMOTE" ":$ref" >/dev/null \
        || return 1
    raw_oid="$(remote_ref_commit "$ref")" || return 1
    [[ -z "$raw_oid" ]]
}

delete_owned_remote_tag() {
    [[ "$#" == 3 && -n "${3:-}" ]] || {
        printf 'refusing to delete tag without an expected raw object OID\n' >&2
        return 1
    }
    local tag="$1"
    local expected_commit="$2"
    local expected_raw_oid="$3"
    local ref="refs/tags/$tag"
    local raw_oid peeled_commit
    raw_oid="$(remote_ref_commit "$ref")" || return 1
    [[ -z "$raw_oid" ]] && return 0
    if [[ "$raw_oid" != "$expected_raw_oid" ]]; then
        printf 'refusing to delete tag %s: expected raw object %s, found %s\n' \
            "$tag" "$expected_raw_oid" "$raw_oid" >&2
        return 1
    fi
    peeled_commit="$(remote_tag_commit "$tag")" || return 1
    [[ "$peeled_commit" == "$expected_commit" ]] || {
        printf 'refusing to delete tag %s: expected %s, found %s\n' \
            "$tag" "$expected_commit" "$peeled_commit" >&2
        return 1
    }
    git -C "$PKG_ROOT" push \
        --force-with-lease="$ref:$raw_oid" "$GIT_REMOTE" ":$ref" >/dev/null \
        || return 1
    raw_oid="$(remote_ref_commit "$ref")" || return 1
    [[ -z "$raw_oid" ]]
}

delete_owned_local_tag_object_ref() {
    local raw_oid
    [[ "$LOCAL_TAG_OBJECT_REF_CREATED" == 1 ]] || return 0
    raw_oid="$(git -C "$PKG_ROOT" rev-parse --verify "$LOCAL_TAG_OBJECT_REF" 2>/dev/null)" \
        || return 1
    [[ "$raw_oid" == "$FINAL_TAG_OBJECT" ]] || {
        printf 'refusing to delete local ref %s: expected %s, found %s\n' \
            "$LOCAL_TAG_OBJECT_REF" "$FINAL_TAG_OBJECT" "$raw_oid" >&2
        return 1
    }
    git -C "$PKG_ROOT" update-ref -d \
        "$LOCAL_TAG_OBJECT_REF" "$FINAL_TAG_OBJECT" || return 1
    LOCAL_TAG_OBJECT_REF_CREATED=0
}

delete_owned_draft() {
    local id="$1"
    local metadata actual_id actual_tag actual_draft actual_target
    metadata="$(gh api --hostname "$GITHUB_HOST" \
        "repos/$RELEASE_REPO/releases/$id" \
        --jq '[.id, .tag_name, .draft, .target_commitish] | @tsv')" || return 1
    IFS=$'\t' read -r actual_id actual_tag actual_draft actual_target <<< "$metadata"
    [[ "$actual_id" == "$id" && "$actual_tag" == "$TEMP_RELEASE_TAG" && \
       "$actual_draft" == true && "$actual_target" == "$RELEASE_COMMIT" ]] || {
        printf 'refusing to delete release id %s: ownership metadata does not match\n' "$id" >&2
        return 1
    }
    gh api --hostname "$GITHUB_HOST" --method DELETE \
        "repos/$RELEASE_REPO/releases/$id" >/dev/null
}

rollback() {
    local rc=$?
    local cleanup_failed=0
    trap - EXIT INT TERM HUP

    if ! delete_owned_local_tag_object_ref; then
        printf 'Owned local tag-object ref could not be cleaned: %s\n' \
            "$LOCAL_TAG_OBJECT_REF" >&2
        cleanup_failed=1
    fi

    if [[ "$SUCCESS" == 1 ]]; then
        rm -rf "$STAGING_DIR"
        exit "$rc"
    fi

    # Never delete after publication may have started. The PATCH that attaches
    # the draft to the reserved final tag and publishes it is server-side; a
    # lost response is ambiguous.
    if [[ "$PUBLISH_ATTEMPTED" == 1 ]]; then
        if [[ "$PUBLISHED_VERIFIED" == 1 ]]; then
            printf 'The published release is valid, but a later step failed.\n' >&2
            printf 'The default branch may still need a manual fast-forward to %s.\n' \
                "$RELEASE_COMMIT" >&2
        else
            printf 'Publication was attempted; its result may be uncertain.\n' >&2
        fi
        printf 'No release or final tag was deleted.\n' >&2
        printf 'Inspect release id %s and tag %s before taking manual action.\n' \
            "${DRAFT_ID:-unknown}" "$VERSION" >&2
        printf 'Local staged assets were retained at %s.\n' "$STAGING_DIR" >&2
        ((rc != 0)) || rc=1
        exit "$rc"
    fi

    # Before publication starts, a tag whose successful CAS push was confirmed
    # still belongs to this run. Delete it only while its peeled commit and raw
    # OID continue to match; otherwise preserve every recovery object.
    if [[ "$FINAL_TAG_PUSHED" == 1 ]]; then
        set +e
        if delete_owned_remote_tag \
            "$VERSION" "$RELEASE_COMMIT" "$FINAL_TAG_OBJECT"; then
            FINAL_TAG_PUSHED=0
        else
            printf 'Final tag %s could not be safely removed before publication.\n' \
                "$VERSION" >&2
            printf 'Verified draft %s, staging ref %s, and local assets were retained.\n' \
                "$DRAFT_ID" "$STAGING_REF" >&2
            printf 'Inspect local staged assets at %s.\n' "$STAGING_DIR" >&2
            ((rc != 0)) || rc=1
            exit "$rc"
        fi
        set -e
    elif [[ "$FINAL_TAG_PUSH_ATTEMPTED" == 1 ]]; then
        printf 'Final tag push result is uncertain; no remote object was deleted.\n' >&2
        printf 'Inspect tag %s, draft %s, and staging ref %s manually.\n' \
            "$VERSION" "$DRAFT_ID" "$STAGING_REF" >&2
        printf 'Local staged assets were retained at %s.\n' "$STAGING_DIR" >&2
        ((rc != 0)) || rc=1
        exit "$rc"
    fi

    # A verified draft uses a unique non-SemVer tag_name plus the staging ref.
    # Retain it after a later failure so the verified bytes can be resumed.
    if [[ "$DRAFT_VERIFIED" == 1 ]]; then
        printf 'Verified staging draft %s and release commit were retained.\n' "$DRAFT_ID" >&2
        printf 'No final SemVer tag was published; inspect the default branch and resume manually.\n' >&2
        printf 'Local staged assets were retained at %s.\n' "$STAGING_DIR" >&2
        ((rc != 0)) || rc=1
        exit "$rc"
    fi

    # If draft creation was attempted but no exact API id was returned, object
    # ownership is uncertain. Preserve the unique staging ref rather than
    # deleting anything by name.
    if [[ "$DRAFT_CREATE_ATTEMPTED" == 1 && -z "$DRAFT_ID" ]]; then
        printf 'Draft creation result is uncertain; no remote object was deleted.\n' >&2
        printf 'Inspect draft tag name %s and staging ref %s manually.\n' \
            "$TEMP_RELEASE_TAG" "$STAGING_REF" >&2
        printf 'Local staged assets were retained at %s.\n' "$STAGING_DIR" >&2
        ((rc != 0)) || rc=1
        exit "$rc"
    fi

    if [[ "$STAGING_PUSH_ATTEMPTED" == 1 && "$STAGING_PUSH_CONFIRMED" == 0 ]]; then
        printf 'Staging push result is uncertain; no remote object was deleted.\n' >&2
        printf 'Inspect staging ref %s and local commit %s manually.\n' \
            "$STAGING_REF" "${RELEASE_COMMIT:-unknown}" >&2
        printf 'Local staged assets were retained at %s.\n' "$STAGING_DIR" >&2
        ((rc != 0)) || rc=1
        exit "$rc"
    fi

    set +e
    if [[ -n "$DRAFT_ID" ]]; then
        if delete_owned_draft "$DRAFT_ID"; then
            # TEMP_RELEASE_TAG is GitHub Release metadata. This run never
            # creates or records the raw OID of refs/tags/TEMP_RELEASE_TAG, so
            # it must never infer ownership of or delete that Git ref.
            if [[ "$STAGING_PUSH_CONFIRMED" == 1 ]]; then
                delete_owned_remote_ref "$STAGING_REF" "$RELEASE_COMMIT" \
                    || cleanup_failed=1
            fi
        else
            cleanup_failed=1
        fi
    elif [[ -n "$RELEASE_COMMIT" && "$STAGING_PUSH_CONFIRMED" == 1 ]]; then
        delete_owned_remote_ref "$STAGING_REF" "$RELEASE_COMMIT" || cleanup_failed=1
    fi

    if [[ "$cleanup_failed" == 0 ]]; then
        remove_release_worktree || cleanup_failed=1
    fi
    if [[ "$cleanup_failed" == 0 ]]; then
        if rm -rf "$STAGING_DIR"; then
            printf 'Release preparation failed; confirmed staging objects were rolled back.\n' >&2
        else
            cleanup_failed=1
        fi
    fi
    set -e

    if [[ "$cleanup_failed" != 0 ]]; then
        printf 'Release preparation failed and cleanup was incomplete.\n' >&2
        printf 'No uncertain object was deleted; inspect release id %s, draft tag name %s, and ref %s.\n' \
            "${DRAFT_ID:-none}" "$TEMP_RELEASE_TAG" "$STAGING_REF" >&2
        if [[ -d "$STAGING_DIR" ]]; then
            printf 'Local staged assets were retained at %s.\n' "$STAGING_DIR" >&2
        fi
    fi
    ((rc != 0)) || rc=1
    exit "$rc"
}
trap rollback EXIT INT TERM HUP

ZIP="$STAGING_DIR/$XCF_ASSET_NAME"
printf '==> Packaging %s\n' "$XCF_ASSET_NAME"
(
    cd "$RELEASE_OUT_DIR"
    zip -qry "$ZIP" libIshKernel.xcframework
)
[[ -s "$ZIP" ]] || fail "xcframework archive is empty"

SWIFTPM_SUM="$(swift package compute-checksum "$ZIP")"
XCF_SHA256="$(shasum -a 256 "$ZIP" | awk '{print $1}')"
[[ "$SWIFTPM_SUM" == "$XCF_SHA256" ]] || fail "SwiftPM and SHA-256 checksums disagree"
printf '    SwiftPM checksum: %s\n' "$SWIFTPM_SUM"

FINAL_URL="https://github.com/$RELEASE_REPO/releases/download/$VERSION/$XCF_ASSET_NAME"
FINAL_SOURCE_URL="https://github.com/$RELEASE_REPO/releases/download/$VERSION/$SOURCE_ASSET_NAME"
python3 - "$RELEASE_WORKTREE/Package.swift" "$FINAL_URL" "$SWIFTPM_SUM" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
url = sys.argv[2]
checksum = sys.argv[3]
source = path.read_text(encoding="utf-8")
pattern = re.compile(
    r'(\.binaryTarget\(\s*name:\s*"IshKernel",\s*url:\s*)"[^"]+"'
    r'(\s*,\s*checksum:\s*)"[^"]+"(\s*\))',
    re.DOTALL,
)
updated, count = pattern.subn(
    lambda match: f'{match.group(1)}"{url}"{match.group(2)}"{checksum}"{match.group(3)}',
    source,
    count=1,
)
if count != 1:
    raise SystemExit("could not uniquely update the IshKernel binary target")
path.write_text(updated, encoding="utf-8")
print(f"    url:      {url}")
print(f"    checksum: {checksum}")
PY

git -C "$RELEASE_WORKTREE" diff --check
swift package --package-path "$RELEASE_WORKTREE" dump-package >/dev/null
CHANGED_FILES="$(git -C "$RELEASE_WORKTREE" status --porcelain --untracked-files=no | sed 's/^...//')"
[[ "$CHANGED_FILES" == "Package.swift" ]] \
    || fail "release preparation changed unexpected files: $CHANGED_FILES"

git -C "$RELEASE_WORKTREE" add -- Package.swift
git -C "$RELEASE_WORKTREE" commit \
    -m "Release $VERSION" \
    -m "url: $FINAL_URL" \
    -m "checksum: $SWIFTPM_SUM"
RELEASE_COMMIT="$(git -C "$RELEASE_WORKTREE" rev-parse HEAD)"
COMMIT_FILES="$(git -C "$PKG_ROOT" diff-tree --no-commit-id --name-only -r "$RELEASE_COMMIT")"
[[ "$COMMIT_FILES" == "Package.swift" ]] || fail "release commit contains unexpected files: $COMMIT_FILES"
[[ -z "$(git -C "$RELEASE_WORKTREE" status --porcelain --untracked-files=normal)" ]] \
    || fail "temporary release worktree is not clean after creating the release commit"
[[ "$(git -C "$PKG_ROOT" rev-parse HEAD)" == "$START_HEAD" && \
   -z "$(git -C "$PKG_ROOT" status --porcelain --untracked-files=normal)" ]] \
    || fail "primary worktree changed while preparing the release; it was left untouched"

SOURCE_STAGED="$STAGING_DIR/$SOURCE_ASSET_NAME"
printf '==> Packaging %s\n' "$SOURCE_ASSET_NAME"
"$PKG_ROOT/scripts/package-source.sh" \
    --commit "$RELEASE_COMMIT" \
    --musl-source "$BUILD_MUSL_SOURCE" \
    --expected-zig-version "$BUILD_ZIG_VERSION" \
    --expected-zig-sha256 "$BUILD_ZIG_SHA256" \
    --expected-musl-sha256 "$BUILD_MUSL_SHA256" \
    --version "$VERSION" \
    --output "$SOURCE_STAGED"
[[ -s "$SOURCE_STAGED" ]] || fail "Corresponding Source archive is empty"
SOURCE_SHA256="$(shasum -a 256 "$SOURCE_STAGED" | awk '{print $1}')"

EXISTING_STAGING_REF="$(remote_ref_commit "$STAGING_REF")" \
    || fail "could not query staging ref $STAGING_REF"
[[ -z "$EXISTING_STAGING_REF" ]] || fail "staging ref already exists: $STAGING_REF"
EXISTING_STAGING_TAG="$(remote_tag_commit "$TEMP_RELEASE_TAG")" \
    || fail "could not query staging tag $TEMP_RELEASE_TAG"
[[ -z "$EXISTING_STAGING_TAG" ]] || fail "staging tag already exists: $TEMP_RELEASE_TAG"

STAGING_PUSH_ATTEMPTED=1
if git -C "$PKG_ROOT" push "$GIT_REMOTE" "$RELEASE_COMMIT:$STAGING_REF"; then
    STAGING_PUSH_CONFIRMED=1
else
    STAGING_REMOTE_COMMIT="$(remote_ref_commit "$STAGING_REF")" \
        || fail "staging push failed and its remote state could not be queried"
    [[ "$STAGING_REMOTE_COMMIT" == "$RELEASE_COMMIT" ]] \
        || fail "staging branch push failed"
    STAGING_PUSH_CONFIRMED=1
fi

RELEASE_NOTES="$(gh api --hostname "$GITHUB_HOST" --method POST \
    "repos/$RELEASE_REPO/releases/generate-notes" \
    -f tag_name="$VERSION" \
    -f target_commitish="$RELEASE_COMMIT" \
    --jq '.body')"
ABI_TRANSITION_NOTES="$(ish_release_abi_transition_notes "$VERSION")"
RELEASE_NOTES="$RELEASE_NOTES$ABI_TRANSITION_NOTES

## 对应源码 / Corresponding Source

本版本的 XCFramework 与 \`$SOURCE_ASSET_NAME\` 成对发布；源码归档记录父仓库、
固定的 iSH revision、Zig 版本，并包含静态 guest supervisor 所用的 musl 源码。
RootFS 不属于该归档；本发布脚本不会上传或发布任何 RootFS。

The XCFramework is accompanied by \`$SOURCE_ASSET_NAME\`, which records the
parent and pinned iSH revisions, the Zig version, and the musl source used by
the statically linked guest supervisor. RootFS remains outside this archive;
this release script never uploads or publishes a RootFS.

Zig executable SHA-256: \`$BUILD_ZIG_SHA256\`
musl source-tree SHA-256: \`$BUILD_MUSL_SHA256\`
Corresponding Source SHA-256: \`$SOURCE_SHA256\`"
DRAFT_CREATE_ATTEMPTED=1
DRAFT_ID="$(gh api --hostname "$GITHUB_HOST" --method POST \
    "repos/$RELEASE_REPO/releases" \
    -f tag_name="$TEMP_RELEASE_TAG" \
    -f target_commitish="$RELEASE_COMMIT" \
    -f name="$VERSION" \
    -f body="$RELEASE_NOTES" \
    -F draft=true \
    -F prerelease="$GITHUB_PRERELEASE" \
    --jq '.id')"
[[ "$DRAFT_ID" =~ ^[0-9]+$ ]] || fail "GitHub did not return a valid draft release id"

[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.tag_name')" == "$TEMP_RELEASE_TAG" ]] \
    || fail "draft release tag name mismatch"
[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.target_commitish')" == "$RELEASE_COMMIT" ]] \
    || fail "draft release target mismatch"
[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.draft')" == true ]] \
    || fail "release is not a draft"
[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.prerelease')" == "$GITHUB_PRERELEASE" ]] \
    || fail "draft prerelease flag does not match the SemVer version"

ASSETS=(
    "$ZIP#$XCF_ASSET_NAME"
    "$SOURCE_STAGED#$SOURCE_ASSET_NAME"
)
EXPECTED_ASSET_COUNT=2
gh release upload "$TEMP_RELEASE_TAG" \
    --repo "$GH_REPO" \
    "${ASSETS[@]}"

verify_asset_metadata() {
    local release_id="$1"
    local asset_name="$2"
    local asset_path="$3"
    local expected_url="${4:-}"
    local expected_size expected_digest actual_size actual_digest asset_state actual_url attempt metadata
    expected_size="$(wc -c < "$asset_path" | tr -d '[:space:]')"
    expected_digest="sha256:$(shasum -a 256 "$asset_path" | awk '{print $1}')"
    actual_size=""
    actual_digest=""
    asset_state=""
    actual_url=""
    for ((attempt = 1; attempt <= VERIFY_ATTEMPTS; attempt++)); do
        metadata=""
        if metadata="$(gh api --hostname "$GITHUB_HOST" \
            "repos/$RELEASE_REPO/releases/$release_id" \
            --jq ".assets[] | select(.name == \"$asset_name\") | [.size, .digest, .state, .browser_download_url] | @tsv")"; then
            IFS=$'\t' read -r actual_size actual_digest asset_state actual_url <<< "$metadata"
            if [[ "$actual_size" == "$expected_size" && \
                  "$actual_digest" == "$expected_digest" && \
                  "$asset_state" == uploaded && \
                  ( -z "$expected_url" || "$actual_url" == "$expected_url" ) ]]; then
                return 0
            fi
        fi
        ((attempt == VERIFY_ATTEMPTS)) || retry_delay "$attempt"
    done
    printf '%s verification failed (size=%s, digest=%s, state=%s, url=%s)\n' \
        "$asset_name" "$actual_size" "$actual_digest" "$asset_state" "$actual_url" >&2
    return 1
}

ASSET_COUNT="$(gh api --hostname "$GITHUB_HOST" \
    "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.assets | length')"
[[ "$ASSET_COUNT" == "$EXPECTED_ASSET_COUNT" ]] \
    || fail "draft contains $ASSET_COUNT assets; expected $EXPECTED_ASSET_COUNT"
verify_asset_metadata "$DRAFT_ID" "$XCF_ASSET_NAME" "$ZIP"
verify_asset_metadata "$DRAFT_ID" "$SOURCE_ASSET_NAME" "$SOURCE_STAGED"
DRAFT_VERIFIED=1

# Recheck publication preconditions after the potentially long upload. This
# narrows the race window and prevents knowingly reusing a final tag/release.
[[ "$(git -C "$PKG_ROOT" rev-parse HEAD)" == "$START_HEAD" && \
   -z "$(git -C "$PKG_ROOT" status --porcelain --untracked-files=normal)" && \
   "$(git -C "$RELEASE_WORKTREE" rev-parse HEAD)" == "$RELEASE_COMMIT" && \
   -z "$(git -C "$RELEASE_WORKTREE" status --porcelain --untracked-files=normal)" ]] \
    || fail "source worktree changed during release preparation"
FINAL_TAG_COMMIT="$(remote_tag_commit "$VERSION")" \
    || fail "could not recheck remote tag $VERSION before publication"
[[ -z "$FINAL_TAG_COMMIT" ]] || fail "remote tag appeared during preparation: $VERSION"
CONFLICTING_RELEASE_ID="$(gh api --hostname "$GITHUB_HOST" --paginate \
    "repos/$RELEASE_REPO/releases?per_page=100" \
    --jq ".[] | select(.tag_name == \"$VERSION\") | .id")"
[[ -z "$CONFLICTING_RELEASE_ID" ]] \
    || fail "a release for $VERSION appeared during preparation"
PREPUBLISH_REMOTE_HEAD="$(remote_ref_commit "refs/heads/$DEFAULT_BRANCH")" \
    || fail "could not recheck the remote default branch before publication"
[[ "$PREPUBLISH_REMOTE_HEAD" == "$START_HEAD" ]] \
    || fail "remote default branch changed during preparation"

# Build a unique annotated tag object before the absent-ref CAS.  A lightweight
# tag at RELEASE_COMMIT would let an identical concurrent push become a no-op,
# falsely implying ownership.  The transaction token makes this raw tag object
# unique, so an existing final ref always has a different OID and the lease must
# reject it even when both tags peel to the same commit.
TAGGER_IDENT="$(git -C "$PKG_ROOT" var GIT_COMMITTER_IDENT)" \
    || fail "could not determine tagger identity"
FINAL_TAG_OBJECT="$({
    printf 'object %s\n' "$RELEASE_COMMIT"
    printf 'type commit\n'
    printf 'tag %s\n' "$VERSION"
    printf 'tagger %s\n\n' "$TAGGER_IDENT"
    printf '%s\n\n' "$VERSION"
    printf 'IshEmbed release transaction: %s\n' "$STAGING_TOKEN"
} | git -C "$PKG_ROOT" mktag)" \
    || fail "could not create the annotated final-tag object"
[[ "$(git -C "$PKG_ROOT" cat-file -t "$FINAL_TAG_OBJECT")" == tag && \
   "$(git -C "$PKG_ROOT" rev-parse "$FINAL_TAG_OBJECT^{}")" == "$RELEASE_COMMIT" ]] \
    || fail "annotated final-tag object does not peel to the release commit"
git -C "$PKG_ROOT" update-ref \
    "$LOCAL_TAG_OBJECT_REF" "$FINAL_TAG_OBJECT" "" \
    || fail "could not reserve the private local tag-object ref"
LOCAL_TAG_OBJECT_REF_CREATED=1

# Reserve the final remote ref only while it is absent.  Afterwards both its
# raw annotated-tag OID and its peeled commit must match this transaction.
FINAL_TAG_PUSH_ATTEMPTED=1
git -C "$PKG_ROOT" push \
    --force-with-lease="refs/tags/$VERSION:" \
    "$GIT_REMOTE" "$LOCAL_TAG_OBJECT_REF:refs/tags/$VERSION" \
    || fail "final tag CAS push failed; no publication was attempted"
delete_owned_local_tag_object_ref \
    || fail "final tag was pushed but the private local tag-object ref could not be removed"
FINAL_TAG_RAW_OID="$(remote_ref_commit "refs/tags/$VERSION")" \
    || fail "final tag was pushed but its raw object could not be verified"
FINAL_TAG_COMMIT="$(remote_tag_commit "$VERSION")" \
    || fail "final tag was pushed but its remote state could not be verified"
[[ "$FINAL_TAG_RAW_OID" == "$FINAL_TAG_OBJECT" && \
   "$FINAL_TAG_COMMIT" == "$RELEASE_COMMIT" ]] \
    || fail "final tag CAS did not preserve the owned tag object and release commit"
FINAL_TAG_PUSHED=1

# The final tag is now briefly visible while the verified draft remains private.
# If this PATCH is attempted, a lost response is ambiguous and rollback keeps
# the final tag, release, staging ref, and assets for manual recovery.
PUBLISH_ATTEMPTED=1
PUBLISHED_ID="$(gh api --hostname "$GITHUB_HOST" --method PATCH \
    "repos/$RELEASE_REPO/releases/$DRAFT_ID" \
    -f tag_name="$VERSION" \
    -f name="$VERSION" \
    -F draft=false \
    -F prerelease="$GITHUB_PRERELEASE" \
    --jq '.id')"
[[ "$PUBLISHED_ID" == "$DRAFT_ID" ]] || fail "published release id changed unexpectedly"

[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.draft')" == false ]] \
    || fail "release is still a draft"
[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.prerelease')" == "$GITHUB_PRERELEASE" ]] \
    || fail "published prerelease flag does not match the SemVer version"
[[ "$(gh api --hostname "$GITHUB_HOST" "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.tag_name')" == "$VERSION" ]] \
    || fail "published release tag name mismatch"

for ((attempt = 1; attempt <= VERIFY_ATTEMPTS; attempt++)); do
    FINAL_TAG_RAW_OID="$(remote_ref_commit "refs/tags/$VERSION")" \
        || fail "could not query final tag object after publication"
    FINAL_TAG_COMMIT="$(remote_tag_commit "$VERSION")" \
        || fail "could not query final tag after publication"
    [[ "$FINAL_TAG_RAW_OID" == "$FINAL_TAG_OBJECT" && \
       "$FINAL_TAG_COMMIT" == "$RELEASE_COMMIT" ]] && break
    ((attempt == VERIFY_ATTEMPTS)) || retry_delay "$attempt"
done
[[ "$FINAL_TAG_RAW_OID" == "$FINAL_TAG_OBJECT" && \
   "$FINAL_TAG_COMMIT" == "$RELEASE_COMMIT" ]] \
    || fail "final tag object or peeled commit changed after publication"

verify_asset_metadata "$DRAFT_ID" "$XCF_ASSET_NAME" "$ZIP" "$FINAL_URL"
verify_asset_metadata "$DRAFT_ID" "$SOURCE_ASSET_NAME" "$SOURCE_STAGED" \
    "$FINAL_SOURCE_URL"

verify_public_download() {
    local url="$1"
    local expected_path="$2"
    local output_path="$3"
    local expected_digest actual_digest attempt
    expected_digest="$(shasum -a 256 "$expected_path" | awk '{print $1}')"
    for ((attempt = 1; attempt <= VERIFY_ATTEMPTS; attempt++)); do
        rm -f "$output_path"
        if curl --disable --fail --location --silent --show-error \
            "$url" --output "$output_path"; then
            actual_digest="$(shasum -a 256 "$output_path" | awk '{print $1}')"
            [[ "$actual_digest" == "$expected_digest" ]] && return 0
        fi
        ((attempt == VERIFY_ATTEMPTS)) || retry_delay "$attempt"
    done
    return 1
}

verify_public_download "$FINAL_URL" "$ZIP" "$STAGING_DIR/download-$XCF_ASSET_NAME" \
    || fail "published XCFramework URL is not publicly downloadable with the expected checksum"
verify_public_download "$FINAL_SOURCE_URL" "$SOURCE_STAGED" \
    "$STAGING_DIR/download-$SOURCE_ASSET_NAME" \
    || fail "published Corresponding Source URL is not publicly downloadable with the expected checksum"
PUBLISHED_VERIFIED=1

RELEASE_URL="$(gh api --hostname "$GITHUB_HOST" \
    "repos/$RELEASE_REPO/releases/$DRAFT_ID" --jq '.html_url')"
[[ -n "$RELEASE_URL" ]] || fail "published release URL is empty"

# Publish first, then expose the new manifest on the default branch. This keeps
# branch consumers from ever observing a Package.swift URL that still returns
# 404. The push is a normal fast-forward and cannot overwrite concurrent work.
if git -C "$PKG_ROOT" push \
    "$GIT_REMOTE" "$RELEASE_COMMIT:refs/heads/$DEFAULT_BRANCH"; then
    :
else
    DEFAULT_REMOTE_COMMIT="$(remote_ref_commit "refs/heads/$DEFAULT_BRANCH")" \
        || fail "default-branch push failed and its remote state could not be queried"
    [[ "$DEFAULT_REMOTE_COMMIT" == "$RELEASE_COMMIT" ]] \
        || fail "published release is valid, but the default-branch fast-forward failed"
fi

# TEMP_RELEASE_TAG was only a GitHub draft tag_name. Because this run never
# created or recorded the raw OID of a matching Git tag, do not delete that
# name as a Git ref. The staging branch does have exact commit/lease ownership.
delete_owned_remote_ref "$STAGING_REF" "$RELEASE_COMMIT" \
    || fail "release is valid, but the staging branch could not be safely cleaned up"
remove_release_worktree \
    || fail "release is valid, but the temporary worktree could not be removed"
rm -rf "$STAGING_DIR" \
    || fail "release is valid, but the temporary asset directory could not be removed"

SUCCESS=1
trap - EXIT INT TERM HUP

printf '\nPublished GitHub Release verified successfully.\n'
printf '  Release:  %s\n' "$RELEASE_URL"
printf '  Commit:   %s\n' "$RELEASE_COMMIT"
printf '  Checksum: %s\n' "$SWIFTPM_SUM"
printf '  Source:   %s\n' "$SOURCE_SHA256"
printf '  Local branch: not moved; run git pull --ff-only %s %s\n' \
    "$GIT_REMOTE" "$DEFAULT_BRANCH"

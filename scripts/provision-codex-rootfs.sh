#!/usr/bin/env bash
# Idempotently produce $REPO/build/fs-codex/ — a copy of the clean
# fakefs rootfs ($REPO/build/fs) with nodejs+npm+@openai/codex
# installed inside.
#
# Safe to re-run: if build/fs-codex/.codex.installed exists and is
# newer than build/fs/meta.db, the script exits 0 immediately.
#
# Env knobs:
#   CODEX_VERSION   pin a specific version (default: latest)
#   CODEX_PKG       package name (default: @openai/codex)
#   FORCE=1         wipe build/fs-codex and reprovision
#
# Requires that build-host/provision_codex has been built. The wrapper
# scripts/run-host-tests.sh handles that.

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
src="$repo/build/fs"
dst="$repo/build/fs-codex"
stamp="$dst/.codex.installed"
log="$repo/build/fs-codex.provision.log"
provision_bin="$repo/build-host/provision_codex"

if [[ ! -d "$src" || ! -f "$src/meta.db" ]]; then
    echo "[provision] expected clean rootfs at $src — run scripts/build-rootfs.sh or unpack fs.tar.gz first" >&2
    exit 2
fi
if [[ ! -x "$provision_bin" ]]; then
    echo "[provision] $provision_bin missing — build via meson first (scripts/run-host-tests.sh does this for you)" >&2
    exit 2
fi

if [[ "${FORCE:-0}" == "1" && -d "$dst" ]]; then
    echo "[provision] FORCE=1: wiping $dst"
    rm -rf "$dst"
fi

if [[ -f "$stamp" && "$stamp" -nt "$src/meta.db" ]]; then
    echo "[provision] $dst already provisioned (skip; FORCE=1 to redo)"
    exit 0
fi

echo "[provision] populating $dst from $src"
rm -rf "$dst"
mkdir -p "$dst"
# Need the underlying fakefs files (data/, meta.db, meta.db-shm, meta.db-wal).
# Use cp -a to preserve perms; --reflink=auto if available (macOS lacks it).
if cp -a --reflink=auto "$src/." "$dst/" 2>/dev/null; then
    :
else
    cp -a "$src/." "$dst/"
fi

# Reset WAL so SQLite consolidates state into the main DB. Otherwise iSH
# may see a stale snapshot.
rm -f "$dst/meta.db-shm" "$dst/meta.db-wal"

echo "[provision] running guest apk + npm install (this can take several minutes)"
echo "[provision] tail -f $log to watch"
mkdir -p "$(dirname "$log")"
: > "$log"

set +e
ISH_EMBED_ROOTFS="$dst" \
    CODEX_PKG="${CODEX_PKG:-@openai/codex}" \
    CODEX_VERSION="${CODEX_VERSION:-}" \
    "$provision_bin" 2>&1 | tee -a "$log"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -ne 0 ]]; then
    echo "[provision] FAILED (rc=$rc). See $log" >&2
    # Remove the partially-provisioned tree so a retry starts clean.
    rm -rf "$dst"
    exit $rc
fi

touch "$stamp"
echo "[provision] OK — $dst is ready"

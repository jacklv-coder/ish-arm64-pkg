/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * provision_codex — one-time host-driven install of nodejs+npm+codex into
 * a fakefs rootfs. Boots iSH against $ISH_EMBED_ROOTFS, runs:
 *
 *     apk add --no-cache nodejs npm
 *     npm install -g --loglevel=error @openai/codex@$CODEX_VERSION
 *
 * Streams guest stdout+stderr to stderr in real time so a slow npm
 * install is observable. Exits 0 if both commands return 0.
 *
 * The caller (scripts/provision-codex-rootfs.sh) is responsible for
 * making a copy of the clean rootfs before running this, since the
 * install mutates the fakefs.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ishembed.h"
#include "proto.h"  /* ISH_STREAM_* */

/* All install work happens inside this VM, mirroring how iClaw isolates
 * each environment via chroot. The clean rootfs at /srv/vms/.template
 * is cloned into here on first run. */
#define VM_NAME "codex"
#define VM_ROOT "/srv/vms/" VM_NAME

static const char *getenv_def(const char *k, const char *def) {
    const char *v = getenv(k);
    return (v && *v) ? v : def;
}

static int stream_session(ish_embed_session_t *s, const char *label,
                          uint32_t silence_warn_ms) {
    int32_t xc = 0, sg = 0;
    int got_exit = 0;
    uint32_t silence_acc = 0;
    /* Use a short per-read timeout so we still drain output promptly
     * if something is mid-flight, but ISH_ERR_TIMEOUT is a normal
     * "nothing to read right now" event for slow npm/node-gyp steps —
     * we keep looping until run_oneshot's overall timeout terminates
     * the session for us. */
    for (;;) {
        uint8_t *b = NULL; size_t L = 0; int k = 0; uint64_t seq = 0;
        int rc = ish_embed_session_read(s, 5000, &b, &L, &k, &seq, &xc, &sg);
        if (rc == -12 /* ISH_ERR_TIMEOUT */) {
            silence_acc += 5000;
            if (silence_warn_ms && silence_acc >= silence_warn_ms) {
                fprintf(stderr, "[%s] still working (no output for %us)...\n",
                        label, silence_acc / 1000);
                silence_acc = 0;
            }
            continue;
        }
        if (rc != 0) {
            fprintf(stderr, "[%s] session_read rc=%d (%s)\n",
                    label, rc, ish_embed_strerror(rc));
            return -1;
        }
        silence_acc = 0;
        if (k == ISH_STREAM_STDOUT || k == ISH_STREAM_STDERR) {
            if (L) fwrite(b, 1, L, stderr);
            ish_embed_free(b);
        } else if (k == ISH_STREAM_EXITED) {
            got_exit = 1;
            ish_embed_free(b);
            break;
        } else {
            ish_embed_free(b);
        }
    }
    if (!got_exit) {
        fprintf(stderr, "[%s] no EXITED frame\n", label);
        return -1;
    }
    if (sg != 0) {
        fprintf(stderr, "\n[%s] killed by signal %d\n", label, sg);
        return 128 + sg;
    }
    if (xc != 0) {
        fprintf(stderr, "\n[%s] exit=%d\n", label, xc);
        return xc;
    }
    fprintf(stderr, "\n[%s] OK\n", label);
    return 0;
}

static int run_streaming(ish_embed_instance_t *inst, const char *label,
                         const char *script, uint32_t timeout_ms,
                         const char *chroot_path) {
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    const char *envp[] = {
        "PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin",
        "HOME=/root",
        "TERM=xterm-256color",
        "SHELL=/bin/sh",
        "LANG=C.UTF-8",
        NULL
    };
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.envp = envp;
    opts.cwd = chroot_path ? "/root" : "/";
    opts.chroot_path = chroot_path;
    opts.timeout_ms = timeout_ms;
    ish_embed_session_t *s = NULL;
    int rc = ish_embed_spawn(inst, &opts, &s);
    if (rc != 0) {
        fprintf(stderr, "[%s] spawn rc=%d (%s)\n",
                label, rc, ish_embed_strerror(rc));
        return -1;
    }
    ish_embed_session_close_stdin(s);
    int r = stream_session(s, label, 30000 /* warn after 30s of silence */);
    ish_embed_session_close(s);
    return r;
}

int main(void) {
    const char *root = getenv("ISH_EMBED_ROOTFS");
    if (!root) {
        fprintf(stderr, "ISH_EMBED_ROOTFS not set\n");
        return 2;
    }
    const char *codex_pkg = getenv_def("CODEX_PKG", "@openai/codex");
    /* CODEX_VERSION="" means latest. */
    const char *codex_ver = getenv_def("CODEX_VERSION", "");
    char npm_target[256];
    if (codex_ver[0])
        snprintf(npm_target, sizeof(npm_target), "%s@%s", codex_pkg, codex_ver);
    else
        snprintf(npm_target, sizeof(npm_target), "%s", codex_pkg);

    ish_embed_boot_opts_t bopts = {0};
    bopts.rootfs_path = root;
    bopts.workdir = "/";
    bopts.kernel_log_fd = -1;
    ish_embed_instance_t *inst = NULL;
    int rc = ish_embed_boot(&bopts, &inst);
    if (rc != 0) {
        fprintf(stderr, "boot failed: %s (%d)\n", ish_embed_strerror(rc), rc);
        return 1;
    }

    /* Step 0: clone the stock template into the codex VM tree if not
     * already. We can do this in the root (no chroot) — busybox cp -a
     * is happy to copy across the synthetic fakefs. */
    fprintf(stderr, "[provision] cloning /srv/vms/.template -> %s "
                    "(skip if already cloned)\n", VM_ROOT);
    int clone_rc = run_streaming(inst, "clone-vm",
        "if [ ! -e " VM_ROOT "/.cloned ]; then "
        "  rm -rf " VM_ROOT " && "
        "  mkdir -p " VM_ROOT " && "
        "  cp -a /srv/vms/.template/. " VM_ROOT "/ && "
        "  touch " VM_ROOT "/.cloned && "
        "  echo cloned ; "
        "else "
        "  echo already-cloned ; "
        "fi",
        300000, NULL);
    if (clone_rc != 0) {
        fprintf(stderr, "\nprovision aborted: clone step failed\n");
        ish_embed_shutdown(inst, 2000);
        return 1;
    }

    int rc2 = ish_embed_setup_vm_root(inst, VM_ROOT);
    if (rc2 != 0) {
        fprintf(stderr, "setup_vm_root(%s) failed: %s\n",
                VM_ROOT, ish_embed_strerror(rc2));
        ish_embed_shutdown(inst, 2000);
        return 1;
    }

    int fails = 0;

    /* Optional cache refresh; ignore failure (network may be sluggish). */
    run_streaming(inst, "apk-update",
                  "apk update 2>&1 | tail -20 || true",
                  120000, VM_ROOT);

    /* Step 1: nodejs + npm via apk. */
    fails += (run_streaming(inst, "apk-add-node",
                            "apk add --no-cache nodejs npm 2>&1",
                            600000, VM_ROOT) != 0);

    if (fails) {
        fprintf(stderr, "\nprovision aborted: apk step failed\n");
        ish_embed_shutdown(inst, 2000);
        return 1;
    }

    /* Step 2: codex. Use --no-fund --no-audit to keep output sane.
     * --loglevel=info gives progress without firehose. */
    char script[512];
    snprintf(script, sizeof(script),
             "npm config set fund false; "
             "npm config set audit false; "
             "npm install -g --loglevel=info %s 2>&1",
             npm_target);
    fails += (run_streaming(inst, "npm-install-codex", script, 1200000,
                            VM_ROOT) != 0);

    /* Sanity: where did codex land? */
    run_streaming(inst, "which-codex",
                  "which codex || true; ls -la $(which codex) 2>/dev/null || true",
                  10000, VM_ROOT);

    ish_embed_shutdown(inst, 5000);
    fprintf(stderr, "\nprovision_codex: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}

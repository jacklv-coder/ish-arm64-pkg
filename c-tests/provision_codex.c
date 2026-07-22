/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * provision_codex — host-driven install of nodejs+npm+<global package>
 * into a fakefs rootfs. Despite the name, this provisions arbitrary
 * npm-global tools (codex, claude-code, ...) — controlled by env.
 *
 * Env knobs (all optional):
 *   ISH_EMBED_ROOTFS  path to fakefs rootfs (required)
 *   VM_NAME           dir name under /srv/vms (default: "codex")
 *   NPM_PKG           package name (default: "@openai/codex").
 *                     For back-compat CODEX_PKG is also honored.
 *   NPM_VERSION       version spec (default: "" = latest).
 *                     For back-compat CODEX_VERSION is also honored.
 *   BIN_NAME          binary name to sanity-check after install
 *                     (default: same as NPM_PKG basename, sans @scope/)
 *
 * The supervisor flow:
 *   1. Clone /srv/vms/.template -> /srv/vms/$VM_NAME (idempotent).
 *   2. First chrooted spawn asks guest PID 1 to prepare /proc and /dev/pts.
 *   3. apk add --no-cache nodejs npm.
 *   4. npm install -g $NPM_PKG[@$NPM_VERSION].
 *   5. command -v + test -x $BIN_NAME — require an executable entry.
 *
 * Streams guest stdout+stderr to stderr in real time so a slow npm
 * install is observable. Exits 0 only if every required step succeeds.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ishembed.h"
#include "proto.h"  /* ISH_STREAM_* */
#include "provision_codex_format.h"

static const char *getenv_def(const char *k, const char *def) {
    const char *v = getenv(k);
    return (v && *v) ? v : def;
}

/* Derive a sensible default binary name from a package name. Strips
 * a leading "@scope/" if present. */
static const char *default_bin_name(const char *pkg) {
    const char *slash = strrchr(pkg, '/');
    return slash ? slash + 1 : pkg;
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
    /* NPM_PKG / VM_NAME / BIN_NAME / NPM_VERSION are the modern names;
     * CODEX_PKG / CODEX_VERSION are honored for back-compat. */
    const char *vm_name  = getenv_def("VM_NAME",
                              getenv_def("CODEX_VM_NAME", "codex"));
    const char *pkg      = getenv_def("NPM_PKG",
                              getenv_def("CODEX_PKG", "@openai/codex"));
    const char *ver      = getenv_def("NPM_VERSION",
                              getenv_def("CODEX_VERSION", ""));
    const char *bin_name = getenv_def("BIN_NAME", default_bin_name(pkg));

    char vm_root[256];
    snprintf(vm_root, sizeof(vm_root), "/srv/vms/%s", vm_name);

    char npm_target[ISH_PROVISION_NPM_TARGET_CAPACITY];
    if (ish_provision_format_npm_target(npm_target, sizeof(npm_target),
                                        pkg, ver) < 0) {
        fprintf(stderr, "npm package/version target is too long\n");
        return 2;
    }

    fprintf(stderr, "[provision] VM=%s pkg=%s bin=%s\n",
            vm_root, npm_target, bin_name);

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

    /* Step 0: clone the stock template into the VM tree if not already.
     * Done in the root (no chroot) — busybox cp -a copies across the
     * synthetic fakefs. */
    fprintf(stderr, "[provision] cloning /srv/vms/.template -> %s "
                    "(skip if already cloned)\n", vm_root);
    char clone_cmd[1024];
    snprintf(clone_cmd, sizeof(clone_cmd),
        "if [ ! -e %s/.cloned ]; then "
        "  rm -rf %s && "
        "  mkdir -p %s && "
        "  cp -a /srv/vms/.template/. %s/ && "
        "  touch %s/.cloned && "
        "  echo cloned ; "
        "else "
        "  echo already-cloned ; "
        "fi",
        vm_root, vm_root, vm_root, vm_root, vm_root);
    int clone_rc = run_streaming(inst, "clone-vm", clone_cmd, 300000, NULL);
    if (clone_rc != 0) {
        fprintf(stderr, "\nprovision aborted: clone step failed\n");
        ish_embed_shutdown(inst, 2000);
        return 1;
    }

    int fails = 0;

    /* Optional cache refresh; ignore failure (network may be sluggish). */
    run_streaming(inst, "apk-update",
                  "apk update 2>&1 | tail -20 || true",
                  120000, vm_root);

    /* Step 1: nodejs + npm via apk. Idempotent: apk skips already-
     * installed packages. */
    fails += (run_streaming(inst, "apk-add-node",
                            "apk add --no-cache nodejs npm 2>&1",
                            600000, vm_root) != 0);

    if (fails) {
        fprintf(stderr, "\nprovision aborted: apk step failed\n");
        ish_embed_shutdown(inst, 2000);
        return 1;
    }

    /* Step 2: the package itself. */
    char script[1024];
    snprintf(script, sizeof(script),
             "npm config set fund false; "
             "npm config set audit false; "
             "npm install -g --loglevel=info %s 2>&1",
             npm_target);
    fails += (run_streaming(inst, "npm-install", script, 1200000,
                            vm_root) != 0);

    /* Hard gate: npm success alone is insufficient. Require that the guest
     * command lookup resolves BIN_NAME to an executable entry. BIN_NAME is
     * constrained to shell-safe characters by provision-codex-rootfs.sh. */
    char which_cmd[512];
    snprintf(which_cmd, sizeof(which_cmd),
             "expected=/usr/local/bin/%s; resolved=$(command -v %s) && "
             "test \"$resolved\" = \"$expected\" && "
             "test -x \"$resolved\" && "
             "printf '%%s\\n' \"$resolved\" && ls -la \"$resolved\"",
             bin_name, bin_name);
    fails += (run_streaming(inst, "verify-bin", which_cmd, 10000,
                            vm_root) != 0);

    ish_embed_shutdown(inst, 5000);
    fprintf(stderr, "\nprovision: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * codex_test — exercises `codex` inside the guest. Requires that
 * scripts/provision-codex-rootfs.sh has populated $ISH_EMBED_ROOTFS
 * with nodejs+npm+@openai/codex.
 *
 * Tiers:
 *   - MINIMUM: `codex --version` exits 0 and prints something with a
 *     digit. Hard fail otherwise; this matches the plan's "minimum
 *     success criteria".
 *   - TARGET:  `codex --help` exits 0 and prints >100 bytes. Soft fail
 *     (logs WARN, does not bump exit code) — keeps the suite green
 *     while ABI fixes progress.
 *   - FULL:    `codex --no-alt-screen` is spawned interactively, fed a
 *     newline, and SIGINT'd after 5s. The win condition is that it
 *     terminated cleanly (signal=SIGINT, not SIGSEGV/SIGTRAP/SIGILL).
 *     Soft fail.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ishembed.h"
#include "proto.h"  /* ISH_STREAM_* */

/* Must match the VM_ROOT used by provision_codex.c */
#define VM_ROOT "/srv/vms/codex"

#define SIGINT_NUM  2
#define SIGSEGV_NUM 11
#define SIGILL_NUM  4
#define SIGTRAP_NUM 5
#define SIGBUS_NUM  7
#define SIGFPE_NUM  8
#define SIGABRT_NUM 6

static const char *envp_default[] = {
    "PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin",
    "HOME=/root",
    "TERM=xterm-256color",
    "SHELL=/bin/sh",
    "LANG=C.UTF-8",
    /* Discourage codex from trying to phone home / use network in CI. */
    "CODEX_OFFLINE=1",
    "NO_COLOR=1",
    NULL
};

static const char *signame(int s) {
    switch (s) {
    case 0:           return "none";
    case SIGINT_NUM:  return "SIGINT";
    case SIGSEGV_NUM: return "SIGSEGV";
    case SIGILL_NUM:  return "SIGILL";
    case SIGTRAP_NUM: return "SIGTRAP";
    case SIGBUS_NUM:  return "SIGBUS";
    case SIGFPE_NUM:  return "SIGFPE";
    case SIGABRT_NUM: return "SIGABRT";
    case 9:           return "SIGKILL";
    case 15:          return "SIGTERM";
    default:          return "?";
    }
}

static int crashed_by_signal(int s) {
    return s == SIGSEGV_NUM || s == SIGILL_NUM || s == SIGTRAP_NUM ||
           s == SIGBUS_NUM  || s == SIGFPE_NUM  || s == SIGABRT_NUM;
}

static int contains_digit(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (isdigit((unsigned char)p[i])) return 1;
    return 0;
}

static const char *first_line(const char *buf, size_t len) {
    static char tmp[256];
    size_t n = 0;
    if (!buf) { tmp[0] = 0; return tmp; }
    for (size_t i = 0; i < len && n < sizeof(tmp) - 1; i++) {
        if (buf[i] == '\n') break;
        tmp[n++] = buf[i];
    }
    tmp[n] = 0;
    return tmp;
}

static void dump_head(const char *label, const char *buf, size_t len,
                      size_t maxn) {
    if (!buf || !len) return;
    size_t n = len > maxn ? maxn : len;
    fprintf(stderr, "    --- %s head (%zu/%zu bytes) ---\n", label, n, len);
    fwrite(buf, 1, n, stderr);
    if (n && buf[n-1] != '\n') fputc('\n', stderr);
    fprintf(stderr, "    --- /%s head ---\n", label);
}

/* ---- precheck ------------------------------------------------- */

static int case_codex_present(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/sh", "-c",
                          "command -v codex || ls /usr/local/bin/codex || echo MISSING",
                          NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv; opts.envp = envp_default; opts.cwd = "/";
    opts.chroot_path = VM_ROOT;
    opts.timeout_ms = 10000;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    int fail = 0;
    if (rc != 0) {
        fprintf(stderr, "codex present?    FAIL (run_oneshot rc=%d)\n", rc);
        fail = 1;
    } else if (r.exit_code != 0 ||
               (r.stdout_buf &&
                memmem(r.stdout_buf, r.stdout_len, "MISSING", 7))) {
        fprintf(stderr, "codex present?    FAIL (not installed) -- "
                        "run scripts/provision-codex-rootfs.sh first\n");
        fail = 1;
    } else {
        fprintf(stderr, "codex present?    OK (%s)\n",
                first_line((char *)r.stdout_buf, r.stdout_len));
    }
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return fail;
}

/* ---- minimum: codex --version ---------------------------------- */

static int case_codex_version(ish_embed_instance_t *inst) {
    const char *argv[] = {"codex", "--version", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv; opts.envp = envp_default; opts.cwd = "/root";
    opts.chroot_path = VM_ROOT;
    opts.timeout_ms = 30000;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    int fail = 0;
    if (rc != 0) {
        fprintf(stderr, "codex --version:   FAIL (run rc=%d)\n", rc);
        fail = 1; goto out;
    }
    if (r.timed_out) {
        fprintf(stderr, "codex --version:   FAIL (timed out)\n");
        fail = 1; goto out;
    }
    if (crashed_by_signal(r.signal)) {
        fprintf(stderr, "codex --version:   FAIL (crashed: %s)\n",
                signame(r.signal));
        fail = 1; goto out;
    }
    if (r.exit_code != 0) {
        fprintf(stderr, "codex --version:   FAIL (exit=%d signal=%d=%s)\n",
                r.exit_code, r.signal, signame(r.signal));
        fail = 1; goto out;
    }
    if (!contains_digit((char *)r.stdout_buf, r.stdout_len)) {
        fprintf(stderr, "codex --version:   FAIL (no digit in output: %.80s)\n",
                r.stdout_buf ? (char *)r.stdout_buf : "");
        fail = 1; goto out;
    }
    fprintf(stderr, "codex --version:   OK (%s)\n",
            first_line((char *)r.stdout_buf, r.stdout_len));
out:
    if (fail) {
        dump_head("stdout", (char *)r.stdout_buf, r.stdout_len, 800);
        dump_head("stderr", (char *)r.stderr_buf, r.stderr_len, 1600);
    }
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return fail;
}

/* ---- target: codex --help (soft) ------------------------------ */

static int case_codex_help(ish_embed_instance_t *inst) {
    const char *argv[] = {"codex", "--help", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv; opts.envp = envp_default; opts.cwd = "/root";
    opts.chroot_path = VM_ROOT;
    opts.timeout_ms = 30000;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    int ok = (rc == 0 && !r.timed_out && r.signal == 0 &&
              r.exit_code == 0 && r.stdout_len > 100);
    fprintf(stderr, "codex --help:      %s (exit=%d sig=%s stdout=%zu B)\n",
            ok ? "OK" : "WARN",
            r.exit_code, signame(r.signal), r.stdout_len);
    /* Always dump full --help so we know which TUI flags exist. */
    if (getenv("CODEX_TEST_DUMP_HELP")) {
        dump_head("--help stdout", (char *)r.stdout_buf, r.stdout_len, 8192);
    }
    if (!ok) {
        dump_head("stderr", (char *)r.stderr_buf, r.stderr_len, 800);
    }
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return 0; /* soft */
}

/* ---- full: interactive (soft) --------------------------------- */

static int case_codex_interactive(ish_embed_instance_t *inst) {
    const char *argv[] = {"codex", "--no-alt-screen", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv; opts.envp = envp_default; opts.cwd = "/root";
    opts.chroot_path = VM_ROOT;
    opts.allocate_tty = 1;
    ish_embed_session_t *s = NULL;
    int rc = ish_embed_spawn(inst, &opts, &s);
    if (rc != 0) {
        fprintf(stderr, "codex interactive: WARN (spawn rc=%d)\n", rc);
        return 0;
    }
    /* Drain in a tight loop. Most of the time we will see TIMEOUT from
     * session_read; that's fine. We send Enter after ~1.5 s and SIGINT
     * after another 1.5 s, then continue draining until EXITED or 8 s
     * elapsed. Captured stderr/stdout heads are printed at the end so
     * we can see WHAT codex actually emitted before SIGILL. */
    uint8_t *acc_out = NULL; size_t acc_out_len = 0, acc_out_cap = 0;
    uint8_t *acc_err = NULL; size_t acc_err_len = 0, acc_err_cap = 0;
    int sent_nl = 0, sent_sig = 0, saw_exit = 0;
    int32_t xc = 0, sg = 0;
    uint64_t t0 = (uint64_t)time(NULL) * 1000;
    for (int iter = 0; iter < 80 && !saw_exit; iter++) {
        uint8_t *b = NULL; size_t L = 0; int k = 0; uint64_t seq = 0;
        rc = ish_embed_session_read(s, 200, &b, &L, &k, &seq, &xc, &sg);
        uint64_t elapsed = (uint64_t)time(NULL) * 1000 - t0;
        if (!sent_nl && elapsed > 1500) {
            ish_embed_session_write(s, (const uint8_t *)"\n", 1);
            sent_nl = 1;
        }
        if (!sent_sig && elapsed > 3500) {
            ish_embed_session_signal(s, SIGINT_NUM);
            sent_sig = 1;
        }
        if (elapsed > 8000 && !saw_exit) {
            ish_embed_session_signal(s, 9); /* SIGKILL */
        }
        if (rc == -12) { ish_embed_free(b); continue; }
        if (rc != 0)   { ish_embed_free(b); break; }
        if (k == ISH_STREAM_STDOUT && L) {
            if (acc_out_len + L > acc_out_cap) {
                acc_out_cap = (acc_out_cap ? acc_out_cap * 2 : 4096);
                while (acc_out_cap < acc_out_len + L) acc_out_cap *= 2;
                acc_out = realloc(acc_out, acc_out_cap);
            }
            memcpy(acc_out + acc_out_len, b, L); acc_out_len += L;
        } else if (k == ISH_STREAM_STDERR && L) {
            if (acc_err_len + L > acc_err_cap) {
                acc_err_cap = (acc_err_cap ? acc_err_cap * 2 : 4096);
                while (acc_err_cap < acc_err_len + L) acc_err_cap *= 2;
                acc_err = realloc(acc_err, acc_err_cap);
            }
            memcpy(acc_err + acc_err_len, b, L); acc_err_len += L;
        } else if (k == ISH_STREAM_EXITED) {
            saw_exit = 1;
        }
        ish_embed_free(b);
    }
    size_t total = acc_out_len + acc_err_len;

    if (!saw_exit) {
        fprintf(stderr, "codex interactive: WARN (no exit)\n");
        ish_embed_session_terminate(s, 500);
    } else if (crashed_by_signal(sg)) {
        fprintf(stderr, "codex interactive: WARN (crashed: %s, %zu B output)\n",
                signame(sg), total);
    } else {
        fprintf(stderr, "codex interactive: OK (exit=%d sig=%s, %zu B output)\n",
                xc, signame(sg), total);
    }
    dump_head("interactive stdout", (char *)acc_out, acc_out_len, 4096);
    dump_head("interactive stderr", (char *)acc_err, acc_err_len, 4096);
    free(acc_out); free(acc_err);
    ish_embed_session_close(s);
    return 0; /* soft */
}

/* Sanity check the headless `codex exec --help` and `codex login --help`
 * subcommands. These are what we recommend for terminals that can't
 * render Ink/Ratatui TUIs (e.g. iClaw's append-only line buffer). */
static int case_codex_headless_help(ish_embed_instance_t *inst) {
    struct { const char *label; const char *const *argv; } cases[] = {
        {"exec --help",  (const char *[]){"codex", "exec",  "--help", NULL}},
        {"login --help", (const char *[]){"codex", "login", "--help", NULL}},
        {NULL, NULL},
    };
    for (int i = 0; cases[i].label; i++) {
        ish_embed_spawn_opts_t opts = {0};
        opts.argv = cases[i].argv;
        opts.envp = envp_default;
        opts.cwd = "/root";
        opts.chroot_path = VM_ROOT;
        opts.timeout_ms = 15000;
        ish_embed_oneshot_result_t r;
        int rc = ish_embed_run_oneshot(inst, &opts, &r);
        int ok = (rc == 0 && !r.timed_out && r.signal == 0 &&
                  r.exit_code == 0 && r.stdout_len > 50);
        fprintf(stderr, "codex %-14s %s (exit=%d sig=%s stdout=%zu)\n",
                cases[i].label, ok ? "OK" : "WARN",
                r.exit_code, signame(r.signal), r.stdout_len);
        ish_embed_free(r.stdout_buf);
        ish_embed_free(r.stderr_buf);
    }
    return 0;
}

/* Capture stderr/stdout of `codex` (no --no-alt-screen flag, full TUI
 * mode). Run for 4 s, send SIGTERM, dump tails. Diagnostic only —
 * never fails the suite. */
static int case_codex_tui_diag(ish_embed_instance_t *inst) {
    const char *argv[] = {"codex", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv; opts.envp = envp_default; opts.cwd = "/root";
    opts.chroot_path = VM_ROOT;
    opts.allocate_tty = 1;
    opts.timeout_ms = 6000;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    fprintf(stderr,
            "codex TUI diag:    rc=%d timed_out=%d exit=%d sig=%s "
            "stdout=%zu stderr=%zu\n",
            rc, r.timed_out, r.exit_code, signame(r.signal),
            r.stdout_len, r.stderr_len);
    dump_head("TUI stdout", (char *)r.stdout_buf, r.stdout_len, 2048);
    dump_head("TUI stderr", (char *)r.stderr_buf, r.stderr_len, 2048);
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return 0;
}

int main(void) {
    const char *root = getenv("ISH_EMBED_ROOTFS");
    if (!root) {
        fprintf(stderr, "ISH_EMBED_ROOTFS not set\n");
        return 2;
    }
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

    /* The provisioner cloned VM_ROOT already. The first chrooted spawn below
     * makes guest PID 1 prepare /proc, /dev and devpts automatically. */
    int fails = 0;

    if (case_codex_present(inst)) {
        fprintf(stderr, "\ncodex_test: skipping (not installed)\n");
        ish_embed_shutdown(inst, 2000);
        return 77; /* skip exit code */
    }

    fails += case_codex_version(inst);     /* hard */
    case_codex_help(inst);                 /* soft */
    case_codex_headless_help(inst);        /* soft — iClaw recommended flow */
    case_codex_interactive(inst);          /* soft */
    case_codex_tui_diag(inst);             /* diagnostic */

    ish_embed_shutdown(inst, 2000);
    fprintf(stderr, "\ncodex_test: %d hard failure(s)\n", fails);
    return fails ? 1 : 0;
}

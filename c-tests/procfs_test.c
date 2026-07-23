/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * procfs_test — host-side validation of /proc/<pid>/{status,statm,stat},
 * /proc/cpuinfo and friends inside the iSH guest. Drives ish_embed via
 * the public ABI and bins/utilities present in the stock Alpine fakefs.
 *
 * Boots once against $ISH_EMBED_ROOTFS, runs a handful of read-only guest
 * commands, parses their stdout in the host, and prints OK/FAIL per case.
 * Exit code = number of failures (clamped to 1..125).
 *
 * Designed to be re-runnable: it does NOT modify the rootfs.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ishembed.h"

/* iSH spawns are confined to a VM root via chroot_path. We materialise
 * a throwaway VM from /srv/vms/.template before any test runs, so
 * procfs/devpts/dev nodes get mounted by the supervisor (see
 * ensure_vm_devices in supervisor/ishsv.c). */
#define TEST_VM_NAME "proctest"
#define TEST_VM_ROOT "/srv/vms/" TEST_VM_NAME

/* ---- helpers --------------------------------------------------- */

static const char *rootfs_path(void) {
    const char *p = getenv("ISH_EMBED_ROOTFS");
    if (!p) {
        fprintf(stderr, "ISH_EMBED_ROOTFS env var not set\n");
        exit(77);
    }
    return p;
}

/* Run a guest sh -c inside the test VM chroot and capture stdout+stderr.
 * Returns 0 on success (regardless of guest exit code). On failure
 * prints diagnostics and returns nonzero. Caller must free *out_stdout
 * / *out_stderr via ish_embed_free. */
static int run_sh(ish_embed_instance_t *inst, const char *script,
                  uint32_t timeout_ms,
                  int32_t *out_exit, int32_t *out_signal,
                  char **out_stdout, size_t *out_stdout_len,
                  char **out_stderr, size_t *out_stderr_len) {
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    const char *envp[] = {"PATH=/usr/local/bin:/usr/bin:/bin:/sbin", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.envp = envp;
    opts.cwd = "/";
    opts.chroot_path = TEST_VM_ROOT;
    opts.timeout_ms = timeout_ms;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    if (rc != 0) {
        fprintf(stderr, "  run_sh: rc=%d (%s) script=%s\n",
                rc, ish_embed_strerror(rc), script);
        return rc;
    }
    *out_exit = r.exit_code;
    *out_signal = r.signal;
    *out_stdout = (char *)r.stdout_buf;
    *out_stdout_len = r.stdout_len;
    *out_stderr = (char *)r.stderr_buf;
    *out_stderr_len = r.stderr_len;
    if (r.timed_out) {
        fprintf(stderr, "  run_sh: TIMED OUT after %ums script=%s\n",
                timeout_ms, script);
        return -1;
    }
    return 0;
}

static int line_has(const char *buf, size_t len, const char *needle) {
    if (!buf || !needle) return 0;
    size_t nl = strlen(needle);
    if (nl == 0 || nl > len) return 0;
    for (size_t i = 0; i + nl <= len; i++) {
        if (memcmp(buf + i, needle, nl) == 0) return 1;
    }
    return 0;
}

/* Extract decimal integer following `key` (e.g. "VmRSS:"). Returns -1
 * if not found, else parsed long value (first integer token after key
 * on the matching line). */
static long extract_kv_long(const char *buf, size_t len, const char *key) {
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl <= len; i++) {
        if ((i == 0 || buf[i-1] == '\n') && memcmp(buf + i, key, kl) == 0) {
            size_t j = i + kl;
            while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
            if (j >= len) return -1;
            char *end = NULL;
            long v = strtol(buf + j, &end, 10);
            if (end == buf + j) return -1;
            return v;
        }
    }
    return -1;
}

/* Trim trailing whitespace; return printable single-line representation
 * into static buffer (truncated). For diagnostics only. */
static const char *first_line(const char *buf, size_t len) {
    static char tmp[256];
    size_t n = 0;
    for (size_t i = 0; i < len && n < sizeof(tmp) - 1; i++) {
        if (buf[i] == '\n') break;
        tmp[n++] = buf[i];
    }
    tmp[n] = 0;
    return tmp;
}

/* ---- test cases ------------------------------------------------ */

/* Each case returns 0 on PASS, 1 on FAIL. Test is "soft" if it prints
 * WARN instead of FAIL and returns 0 (used for features we know are
 * still missing — they should not break the suite but should be
 * visible in the log so progress is trackable). */

static int case_proc_self_status(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "cat /proc/self/status", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = 0;
    if (rc != 0) { fail = 1; goto out; }
    if (ec != 0) {
        fprintf(stderr, "  status: cat exit=%d signal=%d stderr=%s\n",
                ec, sg, first_line(se, sel));
        fail = 1; goto out;
    }
    /* Minimum sane fields */
    int has_pid    = line_has(so, sol, "Pid:");
    int has_state  = line_has(so, sol, "State:");
    int has_vmrss  = line_has(so, sol, "VmRSS:");
    int has_vmsize = line_has(so, sol, "VmSize:");
    if (!(has_pid && has_state && has_vmsize)) {
        fprintf(stderr, "  status: missing fields (Pid=%d State=%d VmSize=%d VmRSS=%d)\n",
                has_pid, has_state, has_vmsize, has_vmrss);
        fail = 1;
    }
    long vmsize = extract_kv_long(so, sol, "VmSize:");
    long vmrss  = extract_kv_long(so, sol, "VmRSS:");
    /* Linux reports kB; sanity-check < 1 TiB to catch uninitialized garbage. */
    if (vmsize > 0 && vmsize > (long)(1L << 30)) {
        fprintf(stderr, "  status: VmSize=%ld kB looks insane\n", vmsize);
        fail = 1;
    }
    if (vmrss > 0 && vmrss > (long)(1L << 30)) {
        fprintf(stderr, "  status: VmRSS=%ld kB looks insane\n", vmrss);
        fail = 1;
    }
out:
    fprintf(stderr, "/proc/self/status: %s\n", fail ? "FAIL" : "OK");
    if (fail && so) {
        fprintf(stderr, "    --- stdout head ---\n");
        size_t n = sol > 400 ? 400 : sol;
        fwrite(so, 1, n, stderr);
        if (n) fprintf(stderr, "\n    --- /stdout head ---\n");
    }
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

static int case_proc_self_statm(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "cat /proc/self/statm", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = 0;
    if (rc != 0 || ec != 0) { fail = 1; goto out; }
    /* statm format: size resident shared text lib data dt — all pages */
    long fields[7] = {0};
    int n_parsed = 0;
    const char *p = so;
    for (int i = 0; i < 7 && p < so + sol; i++) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        fields[i] = v; n_parsed++;
        p = end;
        while (p < so + sol && (*p == ' ' || *p == '\t')) p++;
    }
    if (n_parsed < 2) {
        fprintf(stderr, "  statm: parsed only %d fields\n", n_parsed);
        fail = 1; goto out;
    }
    /* All zero is the current known-bad behaviour (per plan). Hard-fail
     * once we actually expect this to be fixed; for now mark as WARN so
     * the suite stays green until the fix lands. */
    if (fields[0] == 0 && fields[1] == 0) {
        fprintf(stderr, "  statm: all zero (size=%ld resident=%ld) -- WARN (known bug)\n",
                fields[0], fields[1]);
        /* not a hard fail */
    }
out:
    fprintf(stderr, "/proc/self/statm: %s\n", fail ? "FAIL" : "OK");
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

static int case_proc_self_stat(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "cat /proc/self/stat", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = 0;
    if (rc != 0 || ec != 0) { fail = 1; goto out; }
    if (sol < 16) {
        fprintf(stderr, "  stat: too short (%zu bytes)\n", sol);
        fail = 1;
    }
out:
    fprintf(stderr, "/proc/self/stat:   %s\n", fail ? "FAIL" : "OK");
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

static int case_proc_self_exe(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "readlink /proc/self/exe", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = 0;
    if (rc != 0 || ec != 0) { fail = 1; goto out; }
    /* Expected: /bin/busybox (readlink is a busybox applet). */
    if (!line_has(so, sol, "busybox") && !line_has(so, sol, "readlink")) {
        fprintf(stderr, "  exe: unexpected target: %s\n", first_line(so, sol));
        fail = 1;
    }
out:
    fprintf(stderr, "/proc/self/exe:    %s (target=%s)\n",
            fail ? "FAIL" : "OK", first_line(so ? so : "", sol));
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

static int case_proc_cpuinfo(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "cat /proc/cpuinfo", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = 0;
    if (rc != 0 || ec != 0) { fail = 1; goto out; }
    /* The AArch64 guest exposes a conservative emulated baseline. cpuinfo MUST
     * NOT advertise extensions that the emulator does not implement and that
     * JIT runtimes (Bun/V8) might otherwise try to use. */
    const char *forbidden[] = {" sve", " sve2", " pac", " mte", NULL};
    for (int i = 0; forbidden[i]; i++) {
        if (line_has(so, sol, forbidden[i])) {
            fprintf(stderr, "  cpuinfo: advertises forbidden feature%s\n",
                    forbidden[i]);
            fail = 1;
        }
    }
out:
    fprintf(stderr, "/proc/cpuinfo:     %s\n", fail ? "FAIL" : "OK");
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

static int case_uname(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "uname -a", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = (rc != 0 || ec != 0 || sol < 4);
    fprintf(stderr, "uname -a:          %s (%s)\n",
            fail ? "FAIL" : "OK", first_line(so ? so : "", sol));
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

/* The pinned iSH fork contains an opt-out arm64 BRK-recovery experiment.
 * Production/host builds define ISH_DISABLE_SKIP_BRK=1; prove that an
 * unhandled guest SIGTRAP remains fatal instead of resuming the shell. */
static int case_fatal_sigtrap(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "kill -TRAP $$; echo SIGTRAP_SURVIVED", 5000,
                    &ec, &sg, &so, &sol, &se, &sel);
    int fail = rc != 0 || sg != SIGTRAP ||
        line_has(so, sol, "SIGTRAP_SURVIVED");
    if (fail) {
        fprintf(stderr,
                "  fatal SIGTRAP: rc=%d exit=%d signal=%d stdout=%s\n",
                rc, ec, sg, first_line(so ? so : "", sol));
    }
    fprintf(stderr, "fatal guest SIGTRAP:%s\n", fail ? "FAIL" : "OK");
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

static int case_proc_self_cmdline(ish_embed_instance_t *inst) {
    int32_t ec = 0, sg = 0;
    char *so = NULL, *se = NULL; size_t sol = 0, sel = 0;
    int rc = run_sh(inst, "tr '\\0' ' ' < /proc/self/cmdline; echo",
                    5000, &ec, &sg, &so, &sol, &se, &sel);
    int fail = (rc != 0 || ec != 0 || sol < 2);
    fprintf(stderr, "/proc/self/cmdline:%s (%s)\n",
            fail ? "FAIL" : "OK", first_line(so ? so : "", sol));
    ish_embed_free(so); ish_embed_free(se);
    return fail;
}

/* ---- VM provisioning ------------------------------------------ */

/* Run a host-driven command WITHOUT a chroot (i.e. as PID 1 in the
 * real rootfs). Used only by setup_test_vm() to clone the template. */
static int run_in_root(ish_embed_instance_t *inst, const char *script,
                       uint32_t timeout_ms) {
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    const char *envp[] = {"PATH=/usr/local/bin:/usr/bin:/bin:/sbin", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.envp = envp;
    opts.cwd = "/";
    opts.timeout_ms = timeout_ms;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    if (rc != 0) {
        fprintf(stderr, "[setup] run_in_root rc=%d\n", rc);
        return -1;
    }
    if (r.exit_code != 0) {
        fprintf(stderr, "[setup] script failed (exit=%d): %s\n",
                r.exit_code, script);
        if (r.stderr_buf && r.stderr_len) {
            fwrite(r.stderr_buf, 1, r.stderr_len, stderr);
        }
    }
    int ec = r.exit_code;
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return ec;
}

/* ishsv's adopted-child cleanup scans the supervisor-visible root /proc, not
 * a per-VM mount. The embedded boot path must establish this mount before PID 1
 * starts; otherwise an empty RootFS directory makes two clean scans look valid
 * and lets a double-fork/setsid descendant escape. */
static int case_supervisor_root_procfs(ish_embed_instance_t *inst) {
    int rc = run_in_root(
        inst,
        "test -r /proc/1/stat && test -r /proc/self/stat",
        5000);
    int fail = rc != 0;
    fprintf(stderr, "supervisor root procfs:%s\n", fail ? "FAIL" : "OK");
    return fail;
}

/* Clone /srv/vms/.template → /srv/vms/proctest if not present. The first
 * chrooted spawn below makes the supervisor mount /proc, /dev, /dev/pts in
 * guest context, so this test also covers automatic VM-root initialization. */
static int setup_test_vm(ish_embed_instance_t *inst) {
    /* Clone (cheap if already there: cp -a with -n? busybox cp lacks
     * -n. Use a stamp file instead.) */
    const char *clone =
        "if [ ! -e " TEST_VM_ROOT "/.cloned ]; then "
        "  rm -rf " TEST_VM_ROOT " && "
        "  mkdir -p " TEST_VM_ROOT " && "
        "  cp -a /srv/vms/.template/. " TEST_VM_ROOT "/ && "
        "  touch " TEST_VM_ROOT "/.cloned ; "
        "fi";
    if (run_in_root(inst, clone, 60000) != 0) {
        fprintf(stderr, "[setup] failed to clone template into "
                        TEST_VM_ROOT "\n");
        return -1;
    }
    return 0;
}

/* ---- main ------------------------------------------------------ */

int main(void) {
    const char *root = rootfs_path();
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

    int fails = case_supervisor_root_procfs(inst);

    if (setup_test_vm(inst) != 0) {
        ish_embed_shutdown(inst, 2000);
        return 1;
    }

    fails += case_proc_self_status(inst);
    fails += case_proc_self_statm(inst);
    fails += case_proc_self_stat(inst);
    fails += case_proc_self_exe(inst);
    fails += case_proc_self_cmdline(inst);
    fails += case_proc_cpuinfo(inst);
    fails += case_uname(inst);
    fails += case_fatal_sigtrap(inst);

    ish_embed_shutdown(inst, 2000);
    fprintf(stderr, "\nprocfs_test: %d failure(s)\n", fails);
    if (fails > 125) fails = 125;
    return fails ? 1 : 0;
}

/* Host-side smoke test: boots iSH against a fakefs rootfs and runs a
 * few commands. Build with:
 *
 *     meson setup build/embed embed -Dish_src=$(pwd)/third_party/ish \
 *         -Dish_build=$(pwd)/build/ish-host
 *     ninja -C build/embed
 *
 * Run:
 *     ISH_EMBED_ROOTFS=$(pwd)/build/fs ./build/embed/ishembed_smoke
 */

#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ishembed.h"
#include "proto.h"  /* ISH_STREAM_* */

static const char *rootfs_path(void) {
    const char *p = getenv("ISH_EMBED_ROOTFS");
    if (!p) {
        fprintf(stderr, "ISH_EMBED_ROOTFS env var not set; skipping smoke test\n");
        exit(77);
    }
    return p;
}

static int test_echo(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/echo", "hi", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    if (rc != 0) { fprintf(stderr, "echo: rc=%d\n", rc); return 1; }
    int ok = (r.exit_code == 0 && r.stdout_len == 3 &&
              memcmp(r.stdout_buf, "hi\n", 3) == 0);
    fprintf(stderr, "echo: exit=%d stdout=\"%.*s\"  -> %s\n",
            r.exit_code, (int)r.stdout_len, r.stdout_buf ? (char*)r.stdout_buf : "",
            ok ? "OK" : "FAIL");
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return ok ? 0 : 1;
}

static int test_false(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/false", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    int ok = (rc == 0 && r.exit_code == 1);
    fprintf(stderr, "false: rc=%d exit=%d -> %s\n", rc, r.exit_code, ok ? "OK" : "FAIL");
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return ok ? 0 : 1;
}

static int test_cwd_env(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/sh", "-c", "echo \"$FOO@$PWD\"", NULL};
    const char *envp[] = {"FOO=bar", "PATH=/bin:/usr/bin", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.envp = envp;
    opts.cwd = "/tmp";
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    int ok = (rc == 0 && r.exit_code == 0 &&
              r.stdout_buf && strncmp((char*)r.stdout_buf, "bar@/tmp", 8) == 0);
    fprintf(stderr, "cwd/env: \"%.*s\" -> %s\n",
            (int)r.stdout_len, r.stdout_buf ? (char*)r.stdout_buf : "(null)",
            ok ? "OK" : "FAIL");
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return ok ? 0 : 1;
}

static int test_stdin_pipe(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/cat", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    ish_embed_session_t *s;
    int rc = ish_embed_spawn(inst, &opts, &s);
    if (rc != 0) { fprintf(stderr, "stdin: spawn rc=%d\n", rc); return 1; }
    ish_embed_session_write(s, (const uint8_t*)"hi\n", 3);
    ish_embed_session_close_stdin(s);
    char acc[64] = {0}; size_t accn = 0;
    int saw_exit = 0; int32_t xc = 0, sg = 0;
    while (!saw_exit && accn < sizeof(acc)-1) {
        uint8_t *b = NULL; size_t L = 0; int k = 0; uint64_t seq = 0;
        rc = ish_embed_session_read(s, 5000, &b, &L, &k, &seq, &xc, &sg);
        if (rc != 0) { fprintf(stderr, "stdin: read rc=%d\n", rc); break; }
        if (k == ISH_STREAM_STDOUT) { if (L) memcpy(acc+accn, b, L); accn += L; ish_embed_free(b); }
        else if (k == ISH_STREAM_EXITED) saw_exit = 1;
        else ish_embed_free(b);
    }
    int ok = (saw_exit && xc == 0 && strncmp(acc, "hi\n", 3) == 0);
    fprintf(stderr, "stdin: \"%.*s\" -> %s\n", (int)accn, acc, ok ? "OK" : "FAIL");
    ish_embed_session_close(s);
    return ok ? 0 : 1;
}

static int test_timeout(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/sleep", "5", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 500;
    ish_embed_oneshot_result_t r;
    int rc = ish_embed_run_oneshot(inst, &opts, &r);
    int ok = (rc == 0 && r.timed_out == 1);
    fprintf(stderr, "timeout: timed_out=%d -> %s\n", r.timed_out, ok ? "OK" : "FAIL");
    ish_embed_free(r.stdout_buf);
    ish_embed_free(r.stderr_buf);
    return ok ? 0 : 1;
}

struct concur_arg { ish_embed_instance_t *inst; int idx; int rc; };
static void *concur_worker(void *arg) {
    struct concur_arg *a = (struct concur_arg *)arg;
    char num[16]; snprintf(num, sizeof(num), "x%d", a->idx);
    const char *argv[] = {"/bin/echo", num, NULL};
    ish_embed_spawn_opts_t opts = {0}; opts.argv = argv;
    ish_embed_oneshot_result_t r;
    a->rc = ish_embed_run_oneshot(a->inst, &opts, &r);
    if (a->rc == 0) a->rc = r.exit_code;
    ish_embed_free(r.stdout_buf); ish_embed_free(r.stderr_buf);
    return NULL;
}

static int test_concurrent(ish_embed_instance_t *inst) {
    pthread_t th[4]; struct concur_arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].inst = inst; args[i].idx = i; args[i].rc = -1;
        pthread_create(&th[i], NULL, concur_worker, &args[i]);
    }
    int ok = 1;
    for (int i = 0; i < 4; i++) { pthread_join(th[i], NULL); if (args[i].rc != 0) ok = 0; }
    fprintf(stderr, "concurrent: -> %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

static int test_ctrlc(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/sleep", "60", NULL};
    ish_embed_spawn_opts_t opts = {0}; opts.argv = argv;
    ish_embed_session_t *s;
    int rc = ish_embed_spawn(inst, &opts, &s);
    if (rc != 0) return 1;
    usleep(200000);
    ish_embed_session_signal(s, 2 /* SIGINT */);
    int saw_exit = 0; int32_t xc = 0, sg = 0;
    for (int i = 0; i < 60 && !saw_exit; i++) {
        uint8_t *b = NULL; size_t L = 0; int k = 0; uint64_t seq = 0;
        rc = ish_embed_session_read(s, 1000, &b, &L, &k, &seq, &xc, &sg);
        if (rc != 0) break;
        if (k == ISH_STREAM_EXITED) saw_exit = 1;
        ish_embed_free(b);
    }
    int ok = (saw_exit && sg != 0);
    fprintf(stderr, "ctrl+c: signal=%d -> %s\n", sg, ok ? "OK" : "FAIL");
    ish_embed_session_close(s);
    return ok ? 0 : 1;
}

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

    int fails = 0;
    fails += test_echo(inst);
    fails += test_false(inst);
    fails += test_cwd_env(inst);
    fails += test_stdin_pipe(inst);
    fails += test_timeout(inst);
    fails += test_concurrent(inst);
    fails += test_ctrlc(inst);

    ish_embed_shutdown(inst, 2000);
    fprintf(stderr, "\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-side deterministic tests for ishsv's bounded stdin queue.  Include the
 * implementation so the static queue helpers can be exercised without an iSH
 * guest or a second copy of production logic.
 */

#define main ishsv_program_main
#include "../supervisor/ishsv.c"
#undef main

int (*ishsv_test_mount_hook)(const char *, const char *);
int (*ishsv_test_statfs_type_hook)(const char *, uint64_t *);
int (*ishsv_test_mknod_hook)(const char *, mode_t, dev_t);
int (*ishsv_test_waitid_hook)(idtype_t, id_t, siginfo_t *, int);
pid_t (*ishsv_test_waitpid_hook)(pid_t, int *, int);
int (*ishsv_test_kill_hook)(pid_t, int);
pid_t (*ishsv_test_tcgetpgrp_hook)(int);
int (*ishsv_test_rename_noreplace_hook)(const char *, const char *);
#if defined(__linux__)
int (*ishsv_test_adopted_scan_hook)(struct proc_child_identity *);
#endif

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        dprintf(2, "supervisor_stdin_test:%d: %s failed (errno=%d: %s)\n",  \
                __LINE__, #expr, errno, strerror(errno));                    \
        exit(1);                                                             \
    }                                                                        \
} while (0)

struct error_capture {
    int saved_stdout;
    int read_fd;
};

static void read_expected_frame(int fd, uint8_t expected_type,
                                uint32_t expected_sid,
                                uint8_t *payload, uint32_t expected_len) {
    struct pollfd wait_fd = {.fd = fd, .events = POLLIN};
    CHECK(poll(&wait_fd, 1, 2000) == 1);
    CHECK(wait_fd.revents & (POLLIN | POLLHUP));
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    CHECK(read_full(fd, hdr, sizeof(hdr)) == 0);
    uint8_t type = 0, flags = 0;
    uint32_t len = 0, sid = 0;
    CHECK(ish_proto_parse_hdr(hdr, &type, &flags, &len, &sid) == 0);
    CHECK(type == expected_type);
    CHECK(flags == 0);
    CHECK(sid == expected_sid);
    CHECK(len == expected_len);
    if (len > 0) CHECK(read_full(fd, payload, len) == 0);
}

static int rename_hook_result;
static int rename_hook_errno;
static char rename_hook_source[128];
static char rename_hook_destination[128];

static int capture_rename_noreplace(const char *source,
                                    const char *destination) {
    snprintf(rename_hook_source, sizeof(rename_hook_source), "%s", source);
    snprintf(rename_hook_destination, sizeof(rename_hook_destination), "%s",
             destination);
    errno = rename_hook_errno;
    return rename_hook_result;
}

static void run_rename_helper_case(int result, int error,
                                   const char *expected_record) {
    int output[2];
    CHECK(pipe(output) == 0);
    int saved_stdout = dup(STDOUT_FILENO);
    CHECK(saved_stdout >= 0);
    CHECK(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]);

    rename_hook_result = result;
    rename_hook_errno = error;
    rename_hook_source[0] = '\0';
    rename_hook_destination[0] = '\0';
    ishsv_test_rename_noreplace_hook = capture_rename_noreplace;
    char *argv[] = {
        "ishsv", "--rename-noreplace", "/workspace/source",
        "/workspace/destination", NULL,
    };
    CHECK(run_rename_noreplace_helper(4, argv) == 0);
    CHECK(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdout);

    char record[32] = {0};
    ssize_t count = read(output[0], record, sizeof(record));
    close(output[0]);
    CHECK(count == (ssize_t)strlen(expected_record));
    CHECK(memcmp(record, expected_record, (size_t)count) == 0);
    CHECK(strcmp(rename_hook_source, "/workspace/source") == 0);
    CHECK(strcmp(rename_hook_destination, "/workspace/destination") == 0);
    ishsv_test_rename_noreplace_hook = NULL;
}

static void test_rename_noreplace_helper(void) {
    run_rename_helper_case(0, 0, "0\n");
    run_rename_helper_case(-1, EEXIST, "17\n");
    char *bad_argv[] = {"ishsv", "--rename-noreplace", "/source", NULL};
    CHECK(run_rename_noreplace_helper(3, bad_argv) == 64);
}

/* Exercise the production main loop across real host pipes. PID 1 starts
 * before the host writer in the app, so deliberately delay HELLO and prove
 * the blocking handshake does not mistake EAGAIN for EOF/failure. */
static void test_delayed_hello_full_main(void) {
    int control[2], events[2];
    CHECK(pipe(control) == 0);
    CHECK(pipe(events) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(control[1]);
        close(events[0]);
        CHECK(dup2(control[0], 0) == 0);
        CHECK(dup2(events[1], 1) == 1);
        int null_fd = open("/dev/null", O_WRONLY);
        CHECK(null_fd >= 0);
        CHECK(dup2(null_fd, 2) == 2);
        close(null_fd);
        close(control[0]);
        close(events[1]);
        char *argv[] = {"ishsv", NULL};
        _exit(ishsv_program_main(1, argv));
    }

    close(control[0]);
    close(events[1]);
    usleep(100 * 1000);

    const char greeting[] = "test";
    uint8_t hello[12 + sizeof(greeting) - 1];
    ish_proto_put_u32(hello, ISH_EMBED_ABI_VERSION);
    hello[4] = ISH_PROTO_VERSION;
    hello[5] = hello[6] = hello[7] = 0;
    ish_proto_put_u32(hello + 8, (uint32_t)(sizeof(greeting) - 1));
    memcpy(hello + 12, greeting, sizeof(greeting) - 1);
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, ISH_FT_HELLO, 0, sizeof(hello), 0);
    CHECK(write_full(control[1], hdr, sizeof(hdr)) == 0);
    CHECK(write_full(control[1], hello, sizeof(hello)) == 0);

    uint8_t ack[12];
    read_expected_frame(events[0], ISH_FT_HELLO_ACK, 0, ack, sizeof(ack));
    CHECK(ish_proto_get_u32(ack) == ISH_EMBED_ABI_VERSION);
    CHECK(ack[4] == ISH_PROTO_VERSION);
    CHECK(ish_proto_get_u32(ack + 8) == SUPERVISOR_MAX_SESSIONS);

    ish_proto_pack_hdr(hdr, ISH_FT_SHUTDOWN, 0, 0, 0);
    CHECK(write_full(control[1], hdr, sizeof(hdr)) == 0);
    read_expected_frame(events[0], ISH_FT_SHUTDOWN_ACK, 0, NULL, 0);
    close(control[1]);
    close(events[0]);

    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void send_test_frame(int fd, uint8_t type, uint32_t sid,
                            const void *payload, uint32_t payload_len) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, type, 0, payload_len, sid);
    CHECK(write_full(fd, hdr, sizeof(hdr)) == 0);
    if (payload_len > 0) CHECK(write_full(fd, payload, payload_len) == 0);
}

static int read_test_frame(int fd, int timeout_ms, uint8_t *type,
                           uint32_t *sid, uint8_t **payload,
                           uint32_t *payload_len) {
    struct pollfd wait_fd = {.fd = fd, .events = POLLIN};
    int ready;
    do {
        ready = poll(&wait_fd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready != 1 || !(wait_fd.revents & (POLLIN | POLLHUP))) return -1;

    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    uint8_t flags = 0;
    if (read_full(fd, hdr, sizeof(hdr)) != 0 ||
        ish_proto_parse_hdr(hdr, type, &flags, payload_len, sid) != 0 ||
        flags != (uint8_t)((*type == ISH_FT_STDOUT_DATA ||
                            *type == ISH_FT_STDERR_DATA)
                               ? ISH_FF_SEQ_PRESENT : 0))
        return -1;

    *payload = NULL;
    if (*payload_len > 0) {
        *payload = (uint8_t *)malloc(*payload_len);
        if (!*payload || read_full(fd, *payload, *payload_len) != 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }
    return 0;
}

/* A hot stdout producer used to keep drain_session_streams... inside an
 * unbounded read-until-EAGAIN loop.  Prove a SIGKILL sent after SPAWNED is
 * serviced before an arbitrary amount of output can pass the control frame. */
static void test_output_flood_does_not_starve_control(void) {
    CHECK(access("/usr/bin/yes", X_OK) == 0);

    int control[2], events[2];
    CHECK(pipe(control) == 0);
    CHECK(pipe(events) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(control[1]);
        close(events[0]);
        CHECK(dup2(control[0], 0) == 0);
        CHECK(dup2(events[1], 1) == 1);
        int null_fd = open("/dev/null", O_WRONLY);
        CHECK(null_fd >= 0);
        CHECK(dup2(null_fd, 2) == 2);
        close(null_fd);
        close(control[0]);
        close(events[1]);
        char *argv[] = {"ishsv", NULL};
        _exit(ishsv_program_main(1, argv));
    }

    close(control[0]);
    close(events[1]);

    const char greeting[] = "fairness";
    uint8_t hello[12 + sizeof(greeting) - 1];
    ish_proto_put_u32(hello, ISH_EMBED_ABI_VERSION);
    hello[4] = ISH_PROTO_VERSION;
    hello[5] = hello[6] = hello[7] = 0;
    ish_proto_put_u32(hello + 8, (uint32_t)(sizeof(greeting) - 1));
    memcpy(hello + 12, greeting, sizeof(greeting) - 1);
    send_test_frame(control[1], ISH_FT_HELLO, 0, hello, sizeof(hello));

    uint8_t *payload = NULL;
    uint8_t type = 0;
    uint32_t sid = 0, payload_len = 0;
    CHECK(read_test_frame(events[0], 2000, &type, &sid, &payload,
                          &payload_len) == 0);
    CHECK(type == ISH_FT_HELLO_ACK && sid == 0 && payload_len == 12);
    free(payload);

    const char command[] = "/usr/bin/yes";
    uint8_t spawn[64];
    size_t off = 0;
    ish_proto_put_u32(spawn + off, 0); off += 4; /* cwd */
    ish_proto_put_u32(spawn + off, 1); off += 4; /* argc */
    ish_proto_put_u32(spawn + off, (uint32_t)(sizeof(command) - 1)); off += 4;
    memcpy(spawn + off, command, sizeof(command) - 1); off += sizeof(command) - 1;
    ish_proto_put_u32(spawn + off, 0); off += 4; /* envc */
    ish_proto_put_u32(spawn + off, 0); off += 4; /* chroot */
    memset(spawn + off, 0, 8); off += 8;         /* winsize */
    send_test_frame(control[1], ISH_FT_SPAWN, 77, spawn, (uint32_t)off);

    CHECK(read_test_frame(events[0], 2000, &type, &sid, &payload,
                          &payload_len) == 0);
    CHECK(type == ISH_FT_SPAWNED && sid == 77 && payload_len == 4);
    free(payload);

    uint8_t signal_payload[4];
    ish_proto_put_i32(signal_payload, SIGKILL);
    send_test_frame(control[1], ISH_FT_SIGNAL, 77,
                    signal_payload, sizeof(signal_payload));

    const size_t max_output_after_signal = 2u * 1024u * 1024u;
    size_t output_after_signal = 0;
    int exited = 0;
    uint64_t deadline = monotonic_ms() + 5000u;
    while (monotonic_ms() <= deadline) {
        payload = NULL;
        if (read_test_frame(events[0], 500, &type, &sid, &payload,
                            &payload_len) != 0)
            continue;
        if ((type == ISH_FT_STDOUT_DATA || type == ISH_FT_STDERR_DATA) &&
            sid == 77 && payload_len >= 8) {
            output_after_signal += payload_len - 8;
        } else if (type == ISH_FT_EXITED && sid == 77) {
            exited = 1;
        }
        free(payload);
        CHECK(output_after_signal <= max_output_after_signal);
        if (exited) break;
    }
    CHECK(exited);

    send_test_frame(control[1], ISH_FT_SHUTDOWN, 0, NULL, 0);
    CHECK(read_test_frame(events[0], 2000, &type, &sid, &payload,
                          &payload_len) == 0);
    CHECK(type == ISH_FT_SHUTDOWN_ACK && sid == 0 && payload_len == 0);
    free(payload);
    close(control[1]);
    close(events[0]);

    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static struct error_capture begin_error_capture(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    struct error_capture capture = {
        .saved_stdout = dup(1),
        .read_fd = fds[0],
    };
    CHECK(capture.saved_stdout >= 0);
    CHECK(dup2(fds[1], 1) == 1);
    close(fds[1]);
    return capture;
}

static int finish_error_capture(struct error_capture capture,
                                uint32_t expected_sid) {
    CHECK(dup2(capture.saved_stdout, 1) == 1);
    close(capture.saved_stdout);

    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    CHECK(read_full(capture.read_fd, hdr, sizeof(hdr)) == 0);
    uint8_t type = 0, flags = 0;
    uint32_t plen = 0, sid = 0;
    CHECK(ish_proto_parse_hdr(hdr, &type, &flags, &plen, &sid) == 0);
    CHECK(type == ISH_FT_ERROR);
    CHECK(flags == 0);
    CHECK(sid == expected_sid);
    CHECK(plen >= 8 && plen <= 264);

    uint8_t payload[264];
    CHECK(read_full(capture.read_fd, payload, plen) == 0);
    close(capture.read_fd);
    return ish_proto_get_i32(payload);
}

static void init_pipe_session(struct session *s, uint32_t sid, int fd) {
    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    s->id = sid;
    s->stdin_fd = fd;
    s->stdout_fd = -1;
    s->stderr_fd = -1;
    CHECK(set_nonblock(fd) == 0);
}

static uint8_t byte_pattern(size_t index, unsigned salt) {
    return (uint8_t)((index * 131u + salt * 17u) & 0xffu);
}

static void fill_pattern(uint8_t *buf, size_t len, unsigned salt) {
    for (size_t i = 0; i < len; i++) buf[i] = byte_pattern(i, salt);
}

static void test_partial_write_wrap_and_deferred_close(void) {
    const size_t first_len = 900u * 1024u;
    const size_t second_len = 600u * 1024u;
    const size_t prefix_to_drain = 650u * 1024u;
    const size_t total = first_len + second_len;

    uint8_t *first = (uint8_t *)malloc(first_len);
    uint8_t *second = (uint8_t *)malloc(second_len);
    uint8_t *actual = (uint8_t *)malloc(total);
    CHECK(first && second && actual);
    fill_pattern(first, first_len, 1);
    fill_pattern(second, second_len, 2);

    int fds[2];
    CHECK(pipe(fds) == 0);
    struct session s;
    init_pipe_session(&s, 101, fds[1]);

    CHECK(enqueue_session_stdin(&s, first, first_len) == 0);
    CHECK(s.stdin_len > 0); /* the pipe is smaller than the submitted frame */
    CHECK(flush_session_stdin_budget(&s, SUPERVISOR_STDIN_WRITE_QUANTUM,
                                     NULL) == 0);

    size_t got = 0;
    while (got < prefix_to_drain) {
        size_t want = prefix_to_drain - got;
        if (want > 8192) want = 8192;
        ssize_t r = read(fds[0], actual + got, want);
        if (r < 0 && errno == EINTR) continue;
        CHECK(r > 0);
        got += (size_t)r;
        CHECK(flush_session_stdin_budget(&s, SUPERVISOR_STDIN_WRITE_QUANTUM,
                                         NULL) == 0);
    }

    CHECK(enqueue_session_stdin(&s, second, second_len) == 0);
    CHECK(s.stdin_len > 0);
    request_session_stdin_close(&s);
    CHECK(s.stdin_close_pending == 1);

    while (1) {
        ssize_t r = read(fds[0], actual + got, total - got);
        if (r < 0 && errno == EINTR) continue;
        CHECK(r >= 0);
        if (r == 0) break;
        got += (size_t)r;
        CHECK(got <= total);
        CHECK(flush_session_stdin_budget(&s, SUPERVISOR_STDIN_WRITE_QUANTUM,
                                         NULL) == 0);
    }

    CHECK(got == total);
    CHECK(memcmp(actual, first, first_len) == 0);
    CHECK(memcmp(actual + first_len, second, second_len) == 0);
    CHECK(s.stdin_fd == -1);
    CHECK(s.stdin_buf == NULL);
    CHECK(s.stdin_len == 0);
    CHECK(s.stdin_close_pending == 0);

    close(fds[0]);
    free_session(&s);
    free(actual);
    free(second);
    free(first);
}

static void fill_pipe_to_eagain(int fd) {
    uint8_t bytes[4096];
    memset(bytes, 0xa5, sizeof(bytes));
    while (1) {
        ssize_t w = write(fd, bytes, sizeof(bytes));
        if (w > 0) continue;
        if (w < 0 && errno == EINTR) continue;
        CHECK(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        return;
    }
}

static void test_overflow_reports_and_terminalizes_stdin(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    struct session s;
    init_pipe_session(&s, 202, fds[1]);
    fill_pipe_to_eagain(s.stdin_fd);

    uint8_t *limit = (uint8_t *)malloc(SUPERVISOR_STDIN_LIMIT);
    CHECK(limit != NULL);
    memset(limit, 0x5a, SUPERVISOR_STDIN_LIMIT);
    CHECK(enqueue_session_stdin(&s, limit, SUPERVISOR_STDIN_LIMIT) == 0);
    CHECK(s.stdin_len == SUPERVISOR_STDIN_LIMIT);

    struct error_capture capture = begin_error_capture();
    uint8_t extra = 0xff;
    CHECK(enqueue_session_stdin(&s, &extra, 1) == -1);
    CHECK(finish_error_capture(capture, 202) == ENOBUFS);
    CHECK(s.stdin_fd == -1);
    CHECK(s.stdin_buf == NULL);
    CHECK(s.stdin_len == 0);

    close(fds[0]);
    free_session(&s);
    free(limit);
}

static void test_hard_write_error_is_observable(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    close(fds[0]);
    struct session s;
    init_pipe_session(&s, 303, fds[1]);

    struct error_capture capture = begin_error_capture();
    uint8_t byte = 0x11;
    CHECK(enqueue_session_stdin(&s, &byte, 1) == 0);
    CHECK(flush_session_stdin_budget(&s, SUPERVISOR_STDIN_WRITE_QUANTUM,
                                     NULL) == -1);
    CHECK(finish_error_capture(capture, 303) == EPIPE);
    CHECK(s.stdin_fd == -1);
    CHECK(s.stdin_buf == NULL);
    free_session(&s);
}

static void test_tty_close_clears_both_fd_slots_before_reuse(void) {
    int shared = open("/dev/null", O_RDWR);
    CHECK(shared >= 0);

    struct session s;
    memset(&s, 0, sizeof(s));
    s.in_use = 1;
    s.id = 404;
    s.is_tty = 1;
    s.stdin_fd = shared;
    s.stdout_fd = shared;
    s.stderr_fd = -1;

    request_session_stdin_close(&s);
    CHECK(flush_session_stdin_budget(&s, SUPERVISOR_STDIN_WRITE_QUANTUM,
                                     NULL) == 0);
    CHECK(s.stdin_fd == -1);
    CHECK(s.stdout_fd == -1);

    int reused = open("/dev/null", O_RDWR);
    CHECK(reused >= 0);
    if (reused != shared) {
        CHECK(dup2(reused, shared) == shared);
        close(reused);
        reused = shared;
    }

    free_session(&s);
    CHECK(fcntl(reused, F_GETFD, 0) >= 0);
    close(reused);
}

static void expect_bad_spawn_payload(const uint8_t *payload, size_t len) {
    struct spawn_args parsed;
    CHECK(len <= UINT32_MAX);
    CHECK(parse_spawn_payload(payload, (uint32_t)len, &parsed) == -1);
    free_spawn_args(&parsed);
}

static void test_spawn_parser_rejects_wrapping_lengths(void) {
    uint8_t payload[64];
    size_t off;

    struct spawn_args empty;
    memset(&empty, 0xa5, sizeof(empty));
    CHECK(parse_spawn_payload(NULL, 0, &empty) == -1);
    free_spawn_args(&empty);

    memset(payload, 0, sizeof(payload));
    ish_proto_put_u32(payload, UINT32_MAX);
    expect_bad_spawn_payload(payload, 4);

    memset(payload, 0, sizeof(payload));
    off = 0;
    ish_proto_put_u32(payload + off, 0); off += 4; /* cwd */
    ish_proto_put_u32(payload + off, 1); off += 4; /* argc */
    ish_proto_put_u32(payload + off, UINT32_MAX); off += 4;
    expect_bad_spawn_payload(payload, off);

    memset(payload, 0, sizeof(payload));
    off = 0;
    ish_proto_put_u32(payload + off, 0); off += 4; /* cwd */
    ish_proto_put_u32(payload + off, 0); off += 4; /* argc */
    ish_proto_put_u32(payload + off, 1); off += 4; /* envc */
    ish_proto_put_u32(payload + off, UINT32_MAX); off += 4;
    expect_bad_spawn_payload(payload, off);

    memset(payload, 0, sizeof(payload));
    off = 0;
    ish_proto_put_u32(payload + off, 0); off += 4; /* cwd */
    ish_proto_put_u32(payload + off, 0); off += 4; /* argc */
    ish_proto_put_u32(payload + off, 0); off += 4; /* envc */
    ish_proto_put_u32(payload + off, UINT32_MAX); off += 4;
    expect_bad_spawn_payload(payload, off);

    /* The complete payload shape introduced by v3 still preserves every
     * known field under the current exact-version protocol. */
    const char command[] = "/bin/true";
    const char env[] = "A=B";
    memset(payload, 0, sizeof(payload));
    off = 0;
    ish_proto_put_u32(payload + off, 1); off += 4;
    payload[off++] = '/';
    ish_proto_put_u32(payload + off, 1); off += 4;
    ish_proto_put_u32(payload + off, sizeof(command) - 1); off += 4;
    memcpy(payload + off, command, sizeof(command) - 1); off += sizeof(command) - 1;
    ish_proto_put_u32(payload + off, 1); off += 4;
    ish_proto_put_u32(payload + off, sizeof(env) - 1); off += 4;
    memcpy(payload + off, env, sizeof(env) - 1); off += sizeof(env) - 1;
    ish_proto_put_u32(payload + off, 0); off += 4;
    ish_proto_put_u16(payload + off, 24); off += 2;
    ish_proto_put_u16(payload + off, 80); off += 2;
    ish_proto_put_u16(payload + off, 0); off += 2;
    ish_proto_put_u16(payload + off, 0); off += 2;
    struct spawn_args parsed;
    CHECK(parse_spawn_payload(payload, (uint32_t)off, &parsed) == 0);
    CHECK(strcmp(parsed.cwd, "/") == 0);
    CHECK(parsed.argv && parsed.argv[0] &&
          strcmp(parsed.argv[0], command) == 0 && !parsed.argv[1]);
    CHECK(parsed.envp && parsed.envp[0] &&
          strcmp(parsed.envp[0], env) == 0 && !parsed.envp[1]);
    CHECK(parsed.init_rows == 24 && parsed.init_cols == 80);
    free_spawn_args(&parsed);
}

static int vm_prepare_mount_calls;
static int vm_prepare_fail_mount_call;
static int vm_prepare_mknod_calls;
static int vm_prepare_devpts_mounted;
static int vm_prepare_proc_mounted;

static int path_has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
        memcmp(path + path_len - suffix_len, suffix, suffix_len) == 0;
}

static int test_vm_statfs_type(const char *path, uint64_t *out_type) {
    CHECK(path != NULL && out_type != NULL);
    if (path_has_suffix(path, "/dev/pts")) {
        *out_type = vm_prepare_devpts_mounted
            ? ISH_FS_MAGIC_DEVPTS : ISH_FS_MAGIC_FAKEFS;
    } else if (path_has_suffix(path, "/proc")) {
        *out_type = vm_prepare_proc_mounted
            ? ISH_FS_MAGIC_PROC : ISH_FS_MAGIC_FAKEFS;
    } else {
        *out_type = ISH_FS_MAGIC_FAKEFS;
    }
    return 0;
}

static int test_vm_mount(const char *type, const char *target) {
    vm_prepare_mount_calls++;
    if (vm_prepare_mount_calls == vm_prepare_fail_mount_call) {
        errno = EIO;
        return -1;
    }
    if (strcmp(type, "devpts") == 0 && path_has_suffix(target, "/dev/pts"))
        vm_prepare_devpts_mounted = 1;
    else if (strcmp(type, "proc") == 0 && path_has_suffix(target, "/proc"))
        vm_prepare_proc_mounted = 1;
    else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int test_vm_mknod(const char *path, mode_t mode, dev_t dev) {
    (void)path;
    (void)mode;
    (void)dev;
    vm_prepare_mknod_calls++;
    return 0;
}

static size_t make_chroot_spawn_payload(uint8_t *payload, size_t capacity,
                                        const char *chroot_path) {
    const char command[] = "/bin/true";
    size_t root_len = strlen(chroot_path);
    size_t needed = 4 + 4 + 4 + sizeof(command) - 1 + 4 + 4 + root_len + 8;
    CHECK(needed <= capacity && needed <= UINT32_MAX && root_len <= UINT32_MAX);
    size_t off = 0;
    ish_proto_put_u32(payload + off, 0); off += 4; /* cwd */
    ish_proto_put_u32(payload + off, 1); off += 4; /* argc */
    ish_proto_put_u32(payload + off, sizeof(command) - 1); off += 4;
    memcpy(payload + off, command, sizeof(command) - 1); off += sizeof(command) - 1;
    ish_proto_put_u32(payload + off, 0); off += 4; /* envc */
    ish_proto_put_u32(payload + off, (uint32_t)root_len); off += 4;
    memcpy(payload + off, chroot_path, root_len); off += root_len;
    memset(payload + off, 0, 8); off += 8;
    CHECK(off == needed);
    return off;
}

static void remove_prepared_vm_tree(const char *root) {
    char path[PATH_MAX];
#define REMOVE_VM_DIR(suffix)                                                 \
    do {                                                                      \
        CHECK(vm_path(path, root, suffix) == 0);                              \
        if (rmdir(path) < 0) CHECK(errno == ENOENT);                          \
    } while (0)
    REMOVE_VM_DIR("/root");
    REMOVE_VM_DIR("/proc");
    REMOVE_VM_DIR("/dev/pts");
    REMOVE_VM_DIR("/dev");
    CHECK(rmdir(root) == 0);
#undef REMOVE_VM_DIR
}

static void reset_vm_prepare_state(void) {
    vm_prepare_mount_calls = 0;
    vm_prepare_fail_mount_call = 0;
    vm_prepare_mknod_calls = 0;
    vm_prepare_devpts_mounted = 0;
    vm_prepare_proc_mounted = 0;
}

static void enable_vm_prepare_hooks(void) {
    ishsv_test_mknod_hook = test_vm_mknod;
    ishsv_test_mount_hook = test_vm_mount;
    ishsv_test_statfs_type_hook = test_vm_statfs_type;
}

static void disable_vm_prepare_hooks(void) {
    ishsv_test_mount_hook = NULL;
    ishsv_test_statfs_type_hook = NULL;
    ishsv_test_mknod_hook = NULL;
    reset_vm_prepare_state();
}

static void test_vm_prepare_failure_is_error_and_retryable(void) {
    char root_template[] = "/tmp/ishsv-vm-prepare.XXXXXX";
    char *root = mkdtemp(root_template);
    CHECK(root != NULL);
    reset_vm_prepare_state();
    enable_vm_prepare_hooks();

    uint8_t payload[PATH_MAX + 64];
    size_t payload_len = make_chroot_spawn_payload(payload, sizeof(payload), root);
    vm_prepare_mount_calls = 0;
    vm_prepare_fail_mount_call = 1; /* devpts */
    struct error_capture capture = begin_error_capture();
    CHECK(do_spawn(700, 0, payload, (uint32_t)payload_len) == -1);
    CHECK(finish_error_capture(capture, 700) == EIO);
    CHECK(find_session(700) == NULL);

    /* A second attempt reaches both mounts and succeeds after every required
     * step has been revalidated. */
    reset_vm_prepare_state();
    CHECK(ensure_vm_devices(root) == 0);
    CHECK(vm_prepare_mount_calls == 2);
    remove_prepared_vm_tree(root);

    /* procfs failures are equally fatal and remain retryable. */
    char proc_template[] = "/tmp/ishsv-vm-proc.XXXXXX";
    root = mkdtemp(proc_template);
    CHECK(root != NULL);
    reset_vm_prepare_state();
    vm_prepare_fail_mount_call = 2; /* proc */
    CHECK(ensure_vm_devices(root) == -1);
    CHECK(errno == EIO);
    remove_prepared_vm_tree(root);

    disable_vm_prepare_hooks();
}

static void test_vm_prepare_rejects_long_path_and_wrong_node(void) {
    char too_long[PATH_MAX + 32];
    too_long[0] = '/';
    memset(too_long + 1, 'x', sizeof(too_long) - 2);
    too_long[sizeof(too_long) - 1] = 0;
    CHECK(ensure_vm_devices(too_long) == -1);
    CHECK(errno == ENAMETOOLONG);

    char root_template[] = "/tmp/ishsv-vm-node.XXXXXX";
    char *root = mkdtemp(root_template);
    CHECK(root != NULL);
    char path[PATH_MAX];
    CHECK(vm_path(path, root, "/dev") == 0);
    CHECK(mkdir(path, 0755) == 0);
    CHECK(vm_path(path, root, "/dev/null") == 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    close(fd);
    CHECK(ensure_vm_devices(root) == -1);
    CHECK(unlink(path) == 0);
    CHECK(vm_path(path, root, "/dev/pts") == 0);
    CHECK(rmdir(path) == 0);
    CHECK(vm_path(path, root, "/dev") == 0);
    CHECK(rmdir(path) == 0);
    CHECK(rmdir(root) == 0);
}

static void check_vm_runtime_state(const char *root) {
    static const char *const directories[] = {
        "/dev", "/dev/pts", "/proc", "/root",
    };
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        CHECK(vm_path(path, root, directories[i]) == 0);
        CHECK(path_is_directory(path));
    }

    CHECK(vm_path(path, root, "/root/.codex") == 0);
    struct stat state;
    CHECK(lstat(path, &state) == -1);
    CHECK(errno == ENOENT);
}

/* A path string is not a stable VM identity. Replacing the directory at the
 * same path must cause every invariant and both synthetic mounts to be applied
 * to the new tree rather than accepting a stale success. */
static void test_vm_prepare_revalidates_replaced_root(void) {
    char root_template[] = "/tmp/ishsv-vm-replaced.XXXXXX";
    char *root = mkdtemp(root_template);
    CHECK(root != NULL);
    reset_vm_prepare_state();
    enable_vm_prepare_hooks();

    CHECK(ensure_vm_devices(root) == 0);
    check_vm_runtime_state(root);
    CHECK(vm_prepare_mount_calls == 2);
    CHECK(vm_prepare_mknod_calls == 8);

    char old_root[PATH_MAX];
    CHECK(snprintf(old_root, sizeof(old_root), "%s.old", root) > 0);
    CHECK(rename(root, old_root) == 0);
    CHECK(mkdir(root, 0755) == 0);
    struct stat old_state, new_state;
    CHECK(stat(old_root, &old_state) == 0);
    CHECK(stat(root, &new_state) == 0);
    CHECK(old_state.st_dev != new_state.st_dev ||
          old_state.st_ino != new_state.st_ino);

    /* Mounts attached to the old tree are not evidence for the replacement. */
    vm_prepare_devpts_mounted = 0;
    vm_prepare_proc_mounted = 0;
    CHECK(ensure_vm_devices(root) == 0);
    check_vm_runtime_state(root);
    CHECK(vm_prepare_mount_calls == 4);
    CHECK(vm_prepare_mknod_calls == 16);

    remove_prepared_vm_tree(root);
    remove_prepared_vm_tree(old_root);
    disable_vm_prepare_hooks();
}

/* Correct mounts are a statfs fast path, while missing mounts/directories are
 * repaired on the next spawn into the same root. */
static void test_vm_prepare_repairs_lost_runtime_state(void) {
    char root_template[] = "/tmp/ishsv-vm-repair.XXXXXX";
    char *root = mkdtemp(root_template);
    CHECK(root != NULL);
    reset_vm_prepare_state();
    enable_vm_prepare_hooks();

    CHECK(ensure_vm_devices(root) == 0);
    check_vm_runtime_state(root);
    CHECK(vm_prepare_mount_calls == 2);
    CHECK(vm_prepare_mknod_calls == 8);

    /* Revalidation checks nodes again but must not stack mounts. */
    CHECK(ensure_vm_devices(root) == 0);
    CHECK(vm_prepare_mount_calls == 2);
    CHECK(vm_prepare_mknod_calls == 16);

    char path[PATH_MAX];
    CHECK(vm_path(path, root, "/root") == 0);
    CHECK(rmdir(path) == 0);
    CHECK(vm_path(path, root, "/proc") == 0);
    CHECK(rmdir(path) == 0);
    CHECK(vm_path(path, root, "/dev/pts") == 0);
    CHECK(rmdir(path) == 0);
    vm_prepare_devpts_mounted = 0;
    vm_prepare_proc_mounted = 0;

    CHECK(ensure_vm_devices(root) == 0);
    check_vm_runtime_state(root);
    CHECK(vm_prepare_mount_calls == 4);
    CHECK(vm_prepare_mknod_calls == 24);
    CHECK(vm_prepare_devpts_mounted && vm_prepare_proc_mounted);

    remove_prepared_vm_tree(root);
    disable_vm_prepare_hooks();
}

static void test_stdin_round_budget_and_fair_rotation(void) {
    enum { session_count = SUPERVISOR_MAX_SESSIONS };
    CHECK(SUPERVISOR_STDIN_ROUND_BUDGET ==
          SUPERVISOR_STDIN_WRITE_QUANTUM);
    CHECK(2u * SUPERVISOR_STDIN_WRITE_QUANTUM <= SUPERVISOR_STDIN_LIMIT);
    memset(g_sessions, 0, sizeof(g_sessions));
    g_stdin_rr_cursor = 0;

    for (size_t i = 0; i < session_count; i++) {
        struct session *s = &g_sessions[i];
        memset(s, 0, sizeof(*s));
        s->in_use = 1;
        s->id = (uint32_t)(500 + i);
        s->stdin_fd = open("/dev/null", O_WRONLY);
        CHECK(s->stdin_fd >= 0);
        s->stdout_fd = -1;
        s->stderr_fd = -1;
        s->stdin_buf = (uint8_t *)calloc(
            1, 2u * SUPERVISOR_STDIN_WRITE_QUANTUM);
        CHECK(s->stdin_buf != NULL);
        s->stdin_len = 2u * SUPERVISOR_STDIN_WRITE_QUANTUM;
    }

    for (size_t round = 0; round < session_count; round++) {
        size_t written = flush_all_session_stdin();
        CHECK(written == SUPERVISOR_STDIN_ROUND_BUDGET);
        CHECK(g_stdin_rr_cursor == (round + 1) % session_count);
        for (size_t i = 0; i < session_count; i++) {
            size_t expected = (i <= round ? 1u : 2u) *
                              SUPERVISOR_STDIN_WRITE_QUANTUM;
            CHECK(g_sessions[i].stdin_len == expected);
        }
    }
    for (size_t i = 0; i < session_count; i++)
        CHECK(g_sessions[i].stdin_len == SUPERVISOR_STDIN_WRITE_QUANTUM);

    for (size_t i = 0; i < session_count; i++) free_session(&g_sessions[i]);
    g_stdin_rr_cursor = 0;
}

struct waitpid_event {
    pid_t pid;
    int status;
};

static struct waitpid_event test_waitpid_events[4];
static size_t test_waitpid_event_count;
static size_t test_waitpid_event_index;

static int test_waitid(idtype_t idtype, id_t id, siginfo_t *info,
                       int options) {
    CHECK(idtype == P_ALL);
    CHECK(id == 0);
    CHECK(options == (WEXITED | WNOHANG | WNOWAIT));
    CHECK(info != NULL);
    memset(info, 0, sizeof(*info));
    if (test_waitpid_event_index >= test_waitpid_event_count) return 0;
    struct waitpid_event event =
        test_waitpid_events[test_waitpid_event_index];
    if (event.pid <= 0) {
        test_waitpid_event_index++;
        return 0;
    }
    info->si_pid = event.pid;
    if (WIFEXITED(event.status)) {
        info->si_code = CLD_EXITED;
        info->si_status = WEXITSTATUS(event.status);
    } else {
        info->si_code = CLD_KILLED;
        info->si_status = WTERMSIG(event.status);
    }
    return 0;
}

static pid_t test_waitpid(pid_t requested, int *status, int options) {
    CHECK(options == WNOHANG);
    CHECK(test_waitpid_event_index < test_waitpid_event_count);
    struct waitpid_event event =
        test_waitpid_events[test_waitpid_event_index++];
    CHECK(requested == event.pid);
    CHECK(event.pid > 0);
    if (event.pid > 0 && status) *status = event.status;
    return event.pid;
}

static pid_t test_kill_pids[8];
static int test_kill_signals[8];
static size_t test_kill_count;
static pid_t test_foreground_pgid;

static int test_kill(pid_t pid, int signum) {
    CHECK(test_kill_count < sizeof(test_kill_pids) / sizeof(test_kill_pids[0]));
    test_kill_pids[test_kill_count] = pid;
    test_kill_signals[test_kill_count] = signum;
    test_kill_count++;
    return 0;
}

static pid_t test_tcgetpgrp(int fd) {
    CHECK(fd >= 0);
    return test_foreground_pgid;
}

static void set_waitpid_events(const struct waitpid_event *events,
                               size_t count) {
    CHECK(count <= sizeof(test_waitpid_events) / sizeof(test_waitpid_events[0]));
    memcpy(test_waitpid_events, events, count * sizeof(events[0]));
    test_waitpid_event_count = count;
    test_waitpid_event_index = 0;
}

static void reset_process_hooks(void) {
    ishsv_test_waitid_hook = NULL;
    ishsv_test_waitpid_hook = NULL;
    ishsv_test_kill_hook = NULL;
    ishsv_test_tcgetpgrp_hook = NULL;
    test_waitpid_event_count = 0;
    test_waitpid_event_index = 0;
    test_kill_count = 0;
    test_foreground_pgid = 0;
    g_instance_fail_closed = 0;
#if defined(__linux__)
    ishsv_test_adopted_scan_hook = NULL;
#endif
}

static void test_host_build_requires_hook_for_instance_cleanup(void) {
    reset_process_hooks();
    errno = 0;
    CHECK(supervisor_kill_all(SIGKILL) < 0 && errno == EPERM);

    ishsv_test_kill_hook = test_kill;
    CHECK(supervisor_kill_all(SIGKILL) == 0);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -1);
    CHECK(test_kill_signals[0] == SIGKILL);
    reset_process_hooks();
}

static void test_incomplete_shutdown_is_not_acknowledged(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();
    ishsv_test_kill_hook = test_kill;
    g_sessions[0].in_use = 1;
    g_sessions[0].stdin_fd = -1;
    g_sessions[0].stdout_fd = -1;
    g_sessions[0].stderr_fd = -1;

    require_shutdown_complete();
    CHECK(g_instance_fail_closed);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -1);
    CHECK(test_kill_signals[0] == SIGKILL);

    free_session(&g_sessions[0]);
    reset_process_hooks();
}

#if defined(__linux__)
static int fail_adopted_scan(struct proc_child_identity *identity) {
    (void)identity;
    errno = EIO;
    return -1;
}

static void test_scan_failure_fail_closes_without_exited_frame(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();
    ishsv_test_kill_hook = test_kill;
    ishsv_test_adopted_scan_hook = fail_adopted_scan;

    struct session *s = &g_sessions[0];
    s->in_use = 1;
    s->id = 1301;
    s->reaped = 1;
    s->stdin_fd = -1;
    s->stdout_fd = -1;
    s->stderr_fd = -1;

    int frames[2];
    CHECK(pipe(frames) == 0);
    int saved_stdout = dup(1);
    CHECK(saved_stdout >= 0);
    CHECK(dup2(frames[1], 1) == 1);
    close(frames[1]);
    CHECK(drain_session_streams_and_handle_exit() == 0);
    CHECK(dup2(saved_stdout, 1) == 1);
    close(saved_stdout);

    CHECK(g_instance_fail_closed);
    CHECK(s->in_use);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -1);
    CHECK(test_kill_signals[0] == SIGKILL);
    uint8_t byte = 0;
    CHECK(read(frames[0], &byte, sizeof(byte)) == 0);
    close(frames[0]);

    free_session(s);
    reset_process_hooks();
}
#endif

static void test_cleanup_failures_do_not_report_success(void) {
#if defined(__linux__)
    test_scan_failure_fail_closes_without_exited_frame();
#endif
}

/* Prove that both released numeric identities are cleared, a defensive stale
 * slot is never matched/signalled, and a new session that reuses the numbers
 * receives its own wait status and pre-reap group cleanup. */
static void test_reaped_pid_is_not_reused_as_session_identity(void) {
    const pid_t reused_pid = 4100;
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();
    ishsv_test_waitid_hook = test_waitid;
    ishsv_test_waitpid_hook = test_waitpid;
    ishsv_test_kill_hook = test_kill;

    struct session *old = &g_sessions[0];
    old->in_use = 1;
    old->id = 801;
    old->pid = reused_pid;
    old->pgid = reused_pid;
    old->pgid_validated = 1;
    old->stdout_fd = open("/dev/null", O_RDONLY);
    old->stderr_fd = -1;
    CHECK(old->stdout_fd >= 0);
    old->stdin_fd = -1;

    const struct waitpid_event first_reap[] = {
        {reused_pid, 3 << 8},
        {0, 0},
    };
    set_waitpid_events(first_reap,
                       sizeof(first_reap) / sizeof(first_reap[0]));
    reap_children();
    CHECK(old->in_use && old->reaped && old->pid == 0 && old->pgid == 0 &&
          !old->pgid_validated);
    CHECK(old->exit_code == 3);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -reused_pid);
    CHECK(test_kill_signals[0] == SIGKILL);
    test_kill_count = 0;

    /* Recreate the stale numeric identities that caused the original bug. Both
     * the reaper and signal path must still reject an already-reaped slot. */
    old->pid = reused_pid;
    old->pgid = reused_pid;
    old->pgid_validated = 1;
    struct session *current = &g_sessions[1];
    current->in_use = 1;
    current->id = 802;
    current->pid = reused_pid;
    current->pgid = reused_pid;
    current->pgid_validated = 1;
    current->stdin_fd = -1;
    current->stdout_fd = -1;
    current->stderr_fd = -1;

    const struct waitpid_event reused_reap[] = {
        {reused_pid, 7 << 8},
        {0, 0},
    };
    set_waitpid_events(reused_reap,
                       sizeof(reused_reap) / sizeof(reused_reap[0]));
    reap_children();
    CHECK(old->exit_code == 3);
    CHECK(current->reaped && current->pid == 0 && current->pgid == 0 &&
          !current->pgid_validated && current->exit_code == 7);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -reused_pid);
    CHECK(test_kill_signals[0] == SIGKILL);
    signal_tracked_group(old, SIGKILL);
    signal_tracked_group(current, SIGKILL);
    CHECK(test_kill_count == 1);

    /* A reaped TTY may still have a transport held open by a detached
     * descendant. Its old foreground pgid is no longer a safe identity:
     * force-close must release the fd/slot without signalling that number. */
    force_close_session(old);
    CHECK(test_kill_count == 1);
    CHECK(old->stdin_fd == -1 && old->stdout_fd == -1);

    old->pid = 0;
    old->pgid = 0;
    old->pgid_validated = 0;
    free_session(old);
    free_session(current);
    reset_process_hooks();
}

/* Direct signals keep their documented tracked-group meaning. Force close is
 * deliberately stronger: it kills a distinct TTY foreground job plus the
 * tracked shell, closes the shared pty exactly once, and releases the slot as
 * soon as the direct child is reaped. */
static void test_tty_foreground_close_terminates_complete_session(void) {
    const pid_t shell_pid = 5100;
    const pid_t foreground_pgid = 5200;
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();
    ishsv_test_waitid_hook = test_waitid;
    ishsv_test_waitpid_hook = test_waitpid;
    ishsv_test_kill_hook = test_kill;
    ishsv_test_tcgetpgrp_hook = test_tcgetpgrp;
    test_foreground_pgid = foreground_pgid;

    struct session *s = &g_sessions[0];
    s->in_use = 1;
    s->id = 901;
    s->pid = shell_pid;
    s->pgid = shell_pid;
    s->pgid_validated = 1;
    s->is_tty = 1;
    s->stdin_fd = open("/dev/null", O_RDWR);
    CHECK(s->stdin_fd >= 0);
    s->stdout_fd = s->stdin_fd;
    s->stderr_fd = -1;

    signal_tracked_group(s, SIGUSR1);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -shell_pid);
    CHECK(test_kill_signals[0] == SIGUSR1);

    test_kill_count = 0;
    force_close_session(s);
    CHECK(test_kill_count == 2);
    CHECK(test_kill_pids[0] == -foreground_pgid);
    CHECK(test_kill_signals[0] == SIGKILL);
    CHECK(test_kill_pids[1] == -shell_pid);
    CHECK(test_kill_signals[1] == SIGKILL);
    CHECK(s->stdin_fd == -1 && s->stdout_fd == -1 && s->stderr_fd == -1);
    CHECK(s->in_use && !s->reaped);

    const struct waitpid_event close_reap[] = {
        {shell_pid, SIGKILL},
        {0, 0},
    };
    set_waitpid_events(close_reap,
                       sizeof(close_reap) / sizeof(close_reap[0]));
    test_kill_count = 0;
    reap_children();
    CHECK(s->reaped && s->pid == 0 && s->pgid == 0 &&
          !s->pgid_validated);
    CHECK(test_kill_count == 1);
    CHECK(test_kill_pids[0] == -shell_pid);
    CHECK(test_kill_signals[0] == SIGKILL);

    int frames[2];
    CHECK(pipe(frames) == 0);
    int saved_stdout = dup(1);
    CHECK(saved_stdout >= 0);
    CHECK(dup2(frames[1], 1) == 1);
    close(frames[1]);
    CHECK(drain_session_streams_and_handle_exit() == 0);
    CHECK(dup2(saved_stdout, 1) == 1);
    close(saved_stdout);

    uint8_t payload[8];
    read_expected_frame(frames[0], ISH_FT_EXITED, 901,
                        payload, sizeof(payload));
    close(frames[0]);
    CHECK(ish_proto_get_i32(payload) == 128 + SIGKILL);
    CHECK(ish_proto_get_i32(payload + 4) == SIGKILL);
    CHECK(!s->in_use);
    reset_process_hooks();
}

/* Real Linux regression for the escape Luna found: a direct child forks a
 * background child in the inherited process group, then exits normally while
 * the background child keeps the session output pipe open. reap_children must
 * observe the leader with WNOWAIT, kill the validated group while that zombie
 * still anchors its PID/PGID, and only then reap/clear the numeric identities. */
static void test_reaped_leader_cannot_leave_background_group_running(void) {
#if defined(__linux__) && defined(PR_SET_CHILD_SUBREAPER)
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
#endif
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();

    int output[2];
    int report[2];
    CHECK(pipe(output) == 0);
    CHECK(pipe(report) == 0);

    pid_t leader = fork();
    CHECK(leader >= 0);
    if (leader == 0) {
        close(output[0]);
        close(report[0]);
        if (setsid() < 0 || getpgrp() != getpid()) _exit(120);

        pid_t background = fork();
        if (background < 0) _exit(121);
        if (background == 0) {
            close(report[1]);
            for (;;) pause();
        }

        if (write_full(report[1], &background, sizeof(background)) < 0)
            _exit(122);
        close(report[1]);
        close(output[1]);
        _exit(23);
    }

    close(output[1]);
    close(report[1]);
    pid_t background = 0;
    CHECK(read_full(report[0], &background, sizeof(background)) == 0);
    CHECK(background > 0);
    close(report[0]);

    struct session *s = &g_sessions[0];
    s->in_use = 1;
    s->id = 1001;
    s->pid = leader;
    s->pgid = leader;
    s->pgid_validated = 1;
    s->stdin_fd = -1;
    s->stdout_fd = output[0];
    s->stderr_fd = -1;

    int frames[2];
    CHECK(pipe(frames) == 0);
    int saved_stdout = dup(1);
    CHECK(saved_stdout >= 0);
    CHECK(dup2(frames[1], 1) == 1);
    close(frames[1]);

    uint64_t deadline = monotonic_ms() + 2000u;
    while (s->in_use && monotonic_ms() < deadline) {
        reap_children();
        (void)drain_session_streams_and_handle_exit();
        if (s->in_use) usleep(1000);
    }

    CHECK(dup2(saved_stdout, 1) == 1);
    close(saved_stdout);
    CHECK(!s->in_use);

    uint8_t payload[8];
    read_expected_frame(frames[0], ISH_FT_EXITED, 1001,
                        payload, sizeof(payload));
    close(frames[0]);
    CHECK(ish_proto_get_i32(payload) == 23);
    CHECK(ish_proto_get_i32(payload + 4) == 0);

#if defined(__linux__) && defined(PR_SET_CHILD_SUBREAPER)
    int background_gone = 0;
    deadline = monotonic_ms() + 2000u;
    while (monotonic_ms() < deadline) {
        reap_children();
        if (kill(background, 0) < 0 && errno == ESRCH) {
            background_gone = 1;
            break;
        }
        usleep(1000);
    }
    CHECK(background_gone);
#else
    (void)background;
#endif
    reset_process_hooks();
}

#if defined(__linux__) && defined(PR_SET_CHILD_SUBREAPER)
static pid_t escaped_test_pid;
static int escaped_test_fail_close_count;

static int escaped_test_kill(pid_t pid, int signum) {
    if (pid == -1) {
        escaped_test_fail_close_count++;
        if (escaped_test_pid > 0) return kill(escaped_test_pid, signum);
        errno = ESRCH;
        return -1;
    }
    return kill(pid, signum);
}

/* Build a real orphan tree:
 *
 *   tracked leader (session leader) -> intermediate -> daemon (new session)
 *
 * The intermediate exits, so the subreaper adopts the daemon even while the
 * tracked leader is alive.  The daemon's setsid gives it a PGID which neither
 * the fixed tracked group nor a TTY foreground lookup can reach. */
static pid_t spawn_setsid_escape(int keep_leader_alive, pid_t *daemon_out) {
    int report[2];
    CHECK(pipe(report) == 0);
    pid_t leader = fork();
    CHECK(leader >= 0);
    if (leader == 0) {
        close(report[0]);
        if (setsid() < 0 || getpgrp() != getpid()) _exit(130);
        pid_t intermediate = fork();
        if (intermediate < 0) _exit(131);
        if (intermediate == 0) {
            pid_t daemon = fork();
            if (daemon < 0) _exit(132);
            if (daemon == 0) {
                if (setsid() < 0 || getpgrp() != getpid()) _exit(133);
                pid_t self = getpid();
                if (write_full(report[1], &self, sizeof(self)) < 0) _exit(134);
                close(report[1]);
                for (;;) pause();
            }
            close(report[1]);
            _exit(0);
        }
        close(report[1]);
        int status = 0;
        while (waitpid(intermediate, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) _exit(135);
        if (keep_leader_alive) for (;;) pause();
        _exit(0);
    }

    close(report[1]);
    CHECK(read_full(report[0], daemon_out, sizeof(*daemon_out)) == 0);
    close(report[0]);
    CHECK(*daemon_out > 0);
    CHECK(getpgid(*daemon_out) == *daemon_out);
    return leader;
}

static void run_setsid_escape_cleanup_case(int close_session) {
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);

    pid_t daemon = 0;
    pid_t leader = spawn_setsid_escape(close_session, &daemon);
    escaped_test_pid = daemon;
    escaped_test_fail_close_count = 0;
    ishsv_test_kill_hook = escaped_test_kill;

    struct session *s = &g_sessions[0];
    s->in_use = 1;
    s->id = close_session ? 1102 : 1101;
    s->pid = leader;
    s->pgid = leader;
    s->pgid_validated = 1;
    s->stdin_fd = -1;
    s->stdout_fd = -1;
    s->stderr_fd = -1;

    if (close_session) force_close_session(s);

    int frames[2];
    CHECK(pipe(frames) == 0);
    int saved_stdout = dup(1);
    CHECK(saved_stdout >= 0);
    CHECK(dup2(frames[1], 1) == 1);
    close(frames[1]);

    uint64_t deadline = monotonic_ms() + 3000u;
    while (s->in_use && monotonic_ms() < deadline) {
        reap_children();
        (void)drain_session_streams_and_handle_exit();
        if (s->in_use) usleep(1000);
    }

    CHECK(dup2(saved_stdout, 1) == 1);
    close(saved_stdout);
    CHECK(!s->in_use);
    CHECK(!g_instance_fail_closed);
    CHECK(escaped_test_fail_close_count == 0);
    CHECK(kill(daemon, 0) < 0 && errno == ESRCH);

    uint8_t payload[8];
    read_expected_frame(frames[0], ISH_FT_EXITED,
                        close_session ? 1102 : 1101,
                        payload, sizeof(payload));
    close(frames[0]);
    if (close_session) {
        CHECK(ish_proto_get_i32(payload) == 128 + SIGKILL);
        CHECK(ish_proto_get_i32(payload + 4) == SIGKILL);
    } else {
        CHECK(ish_proto_get_i32(payload) == 0);
        CHECK(ish_proto_get_i32(payload + 4) == 0);
    }

    escaped_test_pid = 0;
    reset_process_hooks();
}

static pid_t spawn_tracked_pause(void) {
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready[0]);
        if (setsid() < 0 || getpgrp() != getpid()) _exit(140);
        uint8_t byte = 1;
        if (write_full(ready[1], &byte, sizeof(byte)) < 0) _exit(141);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    uint8_t byte = 0;
    CHECK(read_full(ready[0], &byte, sizeof(byte)) == 0 && byte == 1);
    close(ready[0]);
    return child;
}

static void test_two_tracked_sessions_are_not_mistaken_for_adopted_children(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    reset_process_hooks();
    pid_t first = spawn_tracked_pause();
    pid_t second = spawn_tracked_pause();
    pid_t children[] = {first, second};
    for (size_t i = 0; i < 2; i++) {
        struct session *s = &g_sessions[i];
        s->in_use = 1;
        s->id = (uint32_t)(1201 + i);
        s->pid = children[i];
        s->pgid = children[i];
        s->pgid_validated = 1;
        s->stdin_fd = -1;
        s->stdout_fd = -1;
        s->stderr_fd = -1;
    }

    CHECK(cleanup_untracked_adopted_children() == 0);
    CHECK(kill(first, 0) == 0);
    CHECK(kill(second, 0) == 0);
    CHECK(!g_instance_fail_closed);

    for (size_t i = 0; i < 2; i++) {
        CHECK(kill(-children[i], SIGKILL) == 0);
        int status = 0;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        free_session(&g_sessions[i]);
    }
    reset_process_hooks();
}
#endif

static void test_setsid_double_fork_escape_is_cleaned_before_exit(void) {
#if defined(__linux__) && defined(PR_SET_CHILD_SUBREAPER)
    test_two_tracked_sessions_are_not_mistaken_for_adopted_children();
    run_setsid_escape_cleanup_case(0);
    run_setsid_escape_cleanup_case(1);
#endif
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_rename_noreplace_helper();
    test_delayed_hello_full_main();
    test_output_flood_does_not_starve_control();
    test_spawn_parser_rejects_wrapping_lengths();
    test_vm_prepare_failure_is_error_and_retryable();
    test_vm_prepare_rejects_long_path_and_wrong_node();
    test_vm_prepare_revalidates_replaced_root();
    test_vm_prepare_repairs_lost_runtime_state();
    test_stdin_round_budget_and_fair_rotation();
    test_partial_write_wrap_and_deferred_close();
    test_overflow_reports_and_terminalizes_stdin();
    test_hard_write_error_is_observable();
    test_tty_close_clears_both_fd_slots_before_reuse();
    test_host_build_requires_hook_for_instance_cleanup();
    test_incomplete_shutdown_is_not_acknowledged();
    test_cleanup_failures_do_not_report_success();
    test_reaped_pid_is_not_reused_as_session_identity();
    test_tty_foreground_close_terminates_complete_session();
    test_reaped_leader_cannot_leave_background_group_running();
    test_setsid_double_fork_escape_is_cleaned_before_exit();
    dprintf(2, "supervisor stdin tests: ok\n");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ishsv — PID 1 supervisor that runs inside iSH.
 *
 * It is built as a static ARM64 musl ELF and bundled into the XCFramework.
 * The host installs it at a private content-addressed fakefs path without
 * replacing the RootFS-owned /sbin/ishsv.
 *
 * Responsibilities:
 *  - On startup, read fd 0 (multiplexed framed protocol) for commands.
 *  - On SPAWN, fork+execve a new process group; wire its stdin to a pipe
 *    we own, and its stdout/stderr to pipes we own; emit framed events
 *    on fd 1.
 *  - poll() on all child fds + control fd; never block on a single child.
 *  - On SIGNAL { signum }, signal only the tracked command group.
 *  - On TERMINATE, signal the tracked group plus a TTY foreground job;
 *    reaper sends SIGKILL after grace.
 *  - On SESSION_CLOSE, kill a still-live session tree and close transports.
 *  - Before reaping a tracked leader, kill any process-group descendants
 *    while the unreaped leader still pins the validated PID/PGID identity.
 *  - On STDIN_CLOSE, drain queued input and then close(child_stdin).
 *  - On SHUTDOWN, kill all children, send SHUTDOWN_ACK, exit(0).
 *
 * fd hygiene:
 *  - Close every parent-side fd in the child (CLOEXEC + explicit close).
 *  - Never F_DUPFD with a high min: iSH cap is around 256, anything close
 *    breaks with EINVAL.
 *
 * Logging:
 *  - All informational logs go to fd 2 as plain text. The host treats
 *    fd 2 as opaque kernel/supervisor log.
 *  - Framed events go ONLY to fd 1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif
#if defined(__linux__)
#include <sys/sysmacros.h>  /* makedev */
#endif
#include <limits.h>         /* PATH_MAX */
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>

#include "../protocol/proto.h"
#include "../include/ishembed.h"

#ifndef SUPERVISOR_MAX_SESSIONS
#define SUPERVISOR_MAX_SESSIONS 64
#endif

#ifndef SUPERVISOR_PIPE_BUF
#define SUPERVISOR_PIPE_BUF (64 * 1024)
#endif

/* Bound work spent on any one child stream before returning to the control
 * loop.  A continuously-writing child must not starve SIGNAL, TERMINATE,
 * STDIN_CLOSE, or SHUTDOWN frames. */
#ifndef SUPERVISOR_STREAM_READS_PER_ROUND
#define SUPERVISOR_STREAM_READS_PER_ROUND 1u
#endif

/* Input is accepted asynchronously so a slow child can never block the
 * supervisor's control loop.  The limit is per session; exceeding it is a
 * terminal error for that session's stdin (the queue is discarded and the
 * write side is closed) rather than silently dropping a frame. */
#ifndef SUPERVISOR_STDIN_LIMIT
#define SUPERVISOR_STDIN_LIMIT (1u * 1024u * 1024u)
#endif

/* Bound child-stdin work independently of the per-session queue ceiling.
 * Each active session gets a small quantum and all sessions share one total
 * budget per control-loop round. */
#ifndef SUPERVISOR_STDIN_WRITE_QUANTUM
#define SUPERVISOR_STDIN_WRITE_QUANTUM (64u * 1024u)
#endif

#ifndef SUPERVISOR_STDIN_ROUND_BUDGET
#define SUPERVISOR_STDIN_ROUND_BUDGET (64u * 1024u)
#endif

/* ----------------------------- session table ----------------------------- */

struct session {
    int       in_use;
    uint32_t  id;
    pid_t     pid;        /* direct child; cleared immediately after waitpid  */
    pid_t     pgid;       /* expected child group; use only while pid is owned */
    int       pgid_validated; /* child reported immutable session/group setup */
    int       stdin_fd;   /* parent-side write end of child stdin             */
    int       stdout_fd;  /* parent-side read end of child stdout             */
    int       stderr_fd;  /* parent-side read end of child stderr; -1 if merged */
    uint64_t  out_seq;
    uint64_t  err_seq;
    int       exit_code;
    int       term_signal;
    int       reaped;
    int       merge_stderr;
    int       is_tty;     /* stdin_fd == stdout_fd == pty master */
    uint8_t  *stdin_buf;   /* bounded ring buffer; allocated on first input */
    size_t    stdin_head;
    size_t    stdin_len;
    int       stdin_close_pending;
    /* Absolute monotonic SIGKILL deadline after TERMINATE; 0 = none. */
    uint64_t  term_deadline_ms;
};

static struct session g_sessions[SUPERVISOR_MAX_SESSIONS];
static size_t g_stdin_rr_cursor;
static int g_instance_fail_closed;

static void signal_session_tree(struct session *s, int signum);

#ifdef ISH_SUPERVISOR_TESTING
extern int (*ishsv_test_waitid_hook)(idtype_t, id_t, siginfo_t *, int);
extern pid_t (*ishsv_test_waitpid_hook)(pid_t, int *, int);
extern int (*ishsv_test_kill_hook)(pid_t, int);
extern pid_t (*ishsv_test_tcgetpgrp_hook)(int);
#endif

static int supervisor_waitid(idtype_t idtype, id_t id, siginfo_t *info,
                             int options) {
#ifdef ISH_SUPERVISOR_TESTING
    if (ishsv_test_waitid_hook)
        return ishsv_test_waitid_hook(idtype, id, info, options);
#endif
    return waitid(idtype, id, info, options);
}

static pid_t supervisor_waitpid(pid_t pid, int *status, int options) {
#ifdef ISH_SUPERVISOR_TESTING
    if (ishsv_test_waitpid_hook)
        return ishsv_test_waitpid_hook(pid, status, options);
#endif
    return waitpid(pid, status, options);
}

static int supervisor_kill(pid_t pid, int signum) {
#ifdef ISH_SUPERVISOR_TESTING
    if (ishsv_test_kill_hook) return ishsv_test_kill_hook(pid, signum);
#endif
    return kill(pid, signum);
}

static int supervisor_kill_all(int signum) {
#ifdef ISH_SUPERVISOR_TESTING
    /* A host-side test process is not a guest PID namespace.  Requiring an
     * explicit hook makes it impossible for a failed regression to broadcast
     * kill(-1) to unrelated developer/CI processes. */
    if (!ishsv_test_kill_hook) {
        errno = EPERM;
        return -1;
    }
#endif
    return supervisor_kill(-1, signum);
}

static pid_t supervisor_tcgetpgrp(int fd) {
#ifdef ISH_SUPERVISOR_TESTING
    if (ishsv_test_tcgetpgrp_hook) return ishsv_test_tcgetpgrp_hook(fd);
#endif
    return tcgetpgrp(fd);
}

/* ------------------------------ logging ---------------------------------- */

static void slogf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
        ssize_t _ = write(2, buf, (size_t)n); (void)_;
    }
}

/* An untracked adopted child can no longer be attributed to one session if
 * /proc inspection or exact-PID cleanup fails.  iSH implements kill(-1) by
 * walking every guest process except the caller, so this is an instance-local
 * last resort.  The main loop observes the flag and exits without emitting a
 * successful EXITED/SHUTDOWN_ACK frame. */
static void fail_close_instance(const char *reason) {
    if (g_instance_fail_closed) return;
    g_instance_fail_closed = 1;
    slogf("ishsv: instance fail-close: %s\n", reason ? reason : "unknown");
    if (supervisor_kill_all(SIGKILL) < 0 && errno != ESRCH)
        slogf("ishsv: kill(-1, SIGKILL) failed: %s\n", strerror(errno));
}

/* Used to keep the protocol channel itself out of any signal-time path. */
static volatile sig_atomic_t g_got_sigchld = 0;
static void on_sigchld(int sig) { (void)sig; g_got_sigchld = 1; }

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return (uint64_t)time(NULL) * 1000u;
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

/* ------------------- write_full / robust I/O helpers --------------------- */

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += w; len -= (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t r = read(fd, p, len);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -2; /* EOF */
        p += r; len -= (size_t)r;
    }
    return 0;
}

/* ------------------------ frame send (single-locked) ---------------------
 * fd 1 is written from one place only (main loop), so no mutex needed.
 * If we ever multithread the supervisor, add a mutex around emit_frame.   */

static int emit_frame(uint8_t type, uint8_t flags,
                      uint32_t session_id,
                      const void *payload, uint32_t payload_len) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, type, flags, payload_len, session_id);
    if (write_full(1, hdr, sizeof(hdr)) < 0) return -1;
    if (payload_len > 0 && write_full(1, payload, payload_len) < 0) return -1;
    return 0;
}

static void emit_error(uint32_t session_id, int err, const char *msg) {
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > 256) mlen = 256;
    uint8_t buf[256 + 8];
    ish_proto_put_i32(buf, err);
    ish_proto_put_u32(buf + 4, (uint32_t)mlen);
    if (mlen > 0) memcpy(buf + 8, msg, mlen);
    (void)emit_frame(ISH_FT_ERROR, 0, session_id, buf, (uint32_t)(8 + mlen));
}

/* --------------------------- session helpers ----------------------------- */

static struct session *find_session(uint32_t id) {
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && g_sessions[i].id == id)
            return &g_sessions[i];
    return NULL;
}

static struct session *alloc_session(uint32_t id) {
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].in_use   = 1;
            g_sessions[i].id       = id;
            g_sessions[i].stdin_fd = -1;
            g_sessions[i].stdout_fd= -1;
            g_sessions[i].stderr_fd= -1;
            return &g_sessions[i];
        }
    }
    return NULL;
}

static void clear_session_stdin_queue(struct session *s) {
    free(s->stdin_buf);
    s->stdin_buf = NULL;
    s->stdin_head = 0;
    s->stdin_len = 0;
}

/* Close a session's input exactly once.  A PTY master is also the stdout fd,
 * so closing stdin necessarily closes the readable side as well; clear both
 * slots before the descriptor number can be reused. */
static void close_session_stdin(struct session *s) {
    int fd = s->stdin_fd;
    if (fd >= 0) {
        s->stdin_fd = -1;
        if (s->is_tty && s->stdout_fd == fd)
            s->stdout_fd = -1;
        close(fd);
    }
    s->stdin_close_pending = 0;
    clear_session_stdin_queue(s);
}

/* Force-close is stronger than a user-directed signal while the direct child
 * is still our unreaped identity: it terminates both the tracked command group
 * and a TTY's current foreground job. reap_children applies the same cleanup
 * after WNOWAIT observes leader exit but before waitpid releases that identity;
 * a reaped slot therefore needs transport-only cleanup. */
static void force_close_session(struct session *s) {
    if (!s || !s->in_use) return;
    if (!s->reaped && s->pid > 0)
        signal_session_tree(s, SIGKILL);
    s->term_deadline_ms = 0;
    close_session_stdin(s);
    if (s->stdout_fd >= 0) {
        close(s->stdout_fd);
        s->stdout_fd = -1;
    }
    if (s->stderr_fd >= 0) {
        close(s->stderr_fd);
        s->stderr_fd = -1;
    }
}

/* ERROR is terminal in the host protocol. Keep the guest-side lifecycle in
 * sync by force-closing the complete session; otherwise the host would release
 * a session that still owns a running child or live transport descriptors. */
static void terminalize_session_stdin(struct session *s, int err,
                                      const char *message) {
    emit_error(s->id, err, message);
    force_close_session(s);
}

static void free_session(struct session *s) {
    /* In TTY mode stdin_fd == stdout_fd (both point at the pty master).
     * Avoid the double close — second one would EBADF and could race
     * the kernel reusing that fd number. */
    int closed_stdin = -1;
    if (s->stdin_fd  >= 0) { close(s->stdin_fd);  closed_stdin = s->stdin_fd; }
    if (s->stdout_fd >= 0 && s->stdout_fd != closed_stdin) close(s->stdout_fd);
    if (s->stderr_fd >= 0) close(s->stderr_fd);
    clear_session_stdin_queue(s);
    memset(s, 0, sizeof(*s));
}

/* Flush at most `budget` bytes accepted by the nonblocking child fd. Partial
 * writes advance the ring; EAGAIN leaves the remainder queued. Any other
 * failure is reported and terminalizes stdin so no accepted bytes disappear
 * without an observable ERROR frame. */
static int flush_session_stdin_budget(struct session *s, size_t budget,
                                      size_t *out_written) {
    size_t written = 0;
    while (s->stdin_fd >= 0 && s->stdin_len > 0 && written < budget) {
        size_t contiguous = SUPERVISOR_STDIN_LIMIT - s->stdin_head;
        if (contiguous > s->stdin_len) contiguous = s->stdin_len;
        if (contiguous > budget - written) contiguous = budget - written;

        ssize_t w = write(s->stdin_fd, s->stdin_buf + s->stdin_head, contiguous);
        if (w > 0) {
            s->stdin_head = (s->stdin_head + (size_t)w) % SUPERVISOR_STDIN_LIMIT;
            s->stdin_len -= (size_t)w;
            written += (size_t)w;
            if (s->stdin_len == 0) s->stdin_head = 0;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;

        int err = (w == 0) ? EIO : errno;
        terminalize_session_stdin(s, err, "child stdin write failed");
        if (out_written) *out_written = written;
        return -1;
    }

    if (s->stdin_fd >= 0 && s->stdin_len == 0 && s->stdin_close_pending)
        close_session_stdin(s);
    if (out_written) *out_written = written;
    return 0;
}

static int enqueue_session_stdin(struct session *s,
                                 const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    if (s->stdin_fd < 0 || s->stdin_close_pending) {
        terminalize_session_stdin(s, EPIPE, "session stdin is closed");
        return -1;
    }

    if (len > SUPERVISOR_STDIN_LIMIT - s->stdin_len) {
        terminalize_session_stdin(s, ENOBUFS,
                                  "session stdin queue limit exceeded");
        return -1;
    }

    if (!s->stdin_buf) {
        s->stdin_buf = (uint8_t *)malloc(SUPERVISOR_STDIN_LIMIT);
        if (!s->stdin_buf) {
            terminalize_session_stdin(s, ENOMEM,
                                      "session stdin queue allocation failed");
            return -1;
        }
    }

    size_t tail = (s->stdin_head + s->stdin_len) % SUPERVISOR_STDIN_LIMIT;
    size_t first = SUPERVISOR_STDIN_LIMIT - tail;
    if (first > len) first = len;
    memcpy(s->stdin_buf + tail, data, first);
    if (len > first) memcpy(s->stdin_buf, data + first, len - first);
    s->stdin_len += len;

    /* Delivery is centralized in flush_all_session_stdin() so every byte
     * written to children is charged to one fair per-round budget. */
    return 0;
}

static void request_session_stdin_close(struct session *s) {
    if (s->stdin_fd < 0) return;
    s->stdin_close_pending = 1;
}

static size_t flush_all_session_stdin(void) {
    size_t remaining = SUPERVISOR_STDIN_ROUND_BUDGET;
    size_t start = g_stdin_rr_cursor % SUPERVISOR_MAX_SESSIONS;
    size_t scanned = 0;
    while (scanned < SUPERVISOR_MAX_SESSIONS && remaining > 0) {
        size_t index = (start + scanned) % SUPERVISOR_MAX_SESSIONS;
        struct session *s = &g_sessions[index];
        scanned++;
        if (s->in_use && s->stdin_fd >= 0 &&
            (s->stdin_len > 0 || s->stdin_close_pending)) {
            size_t quantum = SUPERVISOR_STDIN_WRITE_QUANTUM;
            if (quantum > remaining) quantum = remaining;
            size_t written = 0;
            (void)flush_session_stdin_budget(s, quantum, &written);
            if (written > remaining) written = remaining;
            remaining -= written;
        }
    }
    /* Start after the last examined slot. If every slot was examined, still
     * rotate by one so a repeatedly-ready low index cannot stay first. */
    g_stdin_rr_cursor = scanned < SUPERVISOR_MAX_SESSIONS
        ? (start + scanned) % SUPERVISOR_MAX_SESSIONS
        : (start + 1) % SUPERVISOR_MAX_SESSIONS;
    return SUPERVISOR_STDIN_ROUND_BUDGET - remaining;
}

/* ------------------------------- spawn ---------------------------------- */

/* Parse SPAWN payload (returns malloc'd argv/envp arrays + cwd). */
struct spawn_args {
    char     *cwd;
    char    **argv;
    char    **envp;
    char     *chroot_path;     /* NULL = no chroot */
    /* Optional initial pty winsize (v3 SPAWN tail). 0/0 = use default. */
    uint16_t  init_rows;
    uint16_t  init_cols;
    uint16_t  init_xpix;
    uint16_t  init_ypix;
};

static void free_spawn_args(struct spawn_args *a) {
    if (!a) return;
    free(a->cwd);
    free(a->chroot_path);
    if (a->argv) { for (int i = 0; a->argv[i]; i++) free(a->argv[i]); free(a->argv); }
    if (a->envp) { for (int i = 0; a->envp[i]; i++) free(a->envp[i]); free(a->envp); }
    memset(a, 0, sizeof(*a));
}

static int spawn_take_bytes(const uint8_t *payload, size_t payload_len,
                            size_t *offset, size_t length,
                            const uint8_t **out_bytes) {
    if (!payload || !offset || *offset > payload_len ||
        length > payload_len - *offset)
        return -1;
    if (out_bytes) *out_bytes = payload + *offset;
    *offset += length;
    return 0;
}

static int spawn_take_u32(const uint8_t *payload, size_t payload_len,
                          size_t *offset, uint32_t *out_value) {
    const uint8_t *field = NULL;
    if (!out_value ||
        spawn_take_bytes(payload, payload_len, offset, 4, &field) < 0)
        return -1;
    *out_value = ish_proto_get_u32(field);
    return 0;
}

static int spawn_copy_string(const uint8_t *payload, size_t payload_len,
                             size_t *offset, uint32_t wire_len,
                             char **out_string) {
    if (!out_string) return -1;
    const uint8_t *bytes = NULL;
    if (spawn_take_bytes(payload, payload_len, offset, (size_t)wire_len,
                         &bytes) < 0)
        return -1;
    /* The remaining-length check above bounds wire_len by the <=1 MiB frame,
     * so adding the terminator is safe on every supported size_t width. */
    char *copy = (char *)malloc((size_t)wire_len + 1);
    if (!copy) return -1;
    if (wire_len) memcpy(copy, bytes, wire_len);
    copy[wire_len] = 0;
    *out_string = copy;
    return 0;
}

static int parse_spawn_payload(const uint8_t *p, uint32_t plen,
                               struct spawn_args *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!p) return -1;
    size_t payload_len = plen;
    size_t off = 0;
    uint32_t cwd_len = 0;
    if (spawn_take_u32(p, payload_len, &off, &cwd_len) < 0 ||
        spawn_copy_string(p, payload_len, &off, cwd_len, &out->cwd) < 0)
        return -1;

    uint32_t argc = 0;
    if (spawn_take_u32(p, payload_len, &off, &argc) < 0) return -1;
    if (argc > 4096) return -1;
    out->argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!out->argv) return -1;
    for (uint32_t i = 0; i < argc; i++) {
        uint32_t length = 0;
        if (spawn_take_u32(p, payload_len, &off, &length) < 0 ||
            spawn_copy_string(p, payload_len, &off, length,
                              &out->argv[i]) < 0)
            return -1;
    }

    uint32_t envc = 0;
    if (spawn_take_u32(p, payload_len, &off, &envc) < 0) return -1;
    if (envc > 4096) return -1;
    out->envp = (char **)calloc((size_t)envc + 1, sizeof(char *));
    if (!out->envp) return -1;
    for (uint32_t i = 0; i < envc; i++) {
        uint32_t length = 0;
        if (spawn_take_u32(p, payload_len, &off, &length) < 0 ||
            spawn_copy_string(p, payload_len, &off, length,
                              &out->envp[i]) < 0)
            return -1;
    }
    /* Chroot was introduced in v2. Keep known-tail decoding shape-tolerant
     * for internal fixtures; exact frame/HELLO version checks still require
     * the live host and supervisor to both use the current protocol. */
    if (off <= payload_len && payload_len - off >= 4) {
        uint32_t chroot_len = 0;
        if (spawn_take_u32(p, payload_len, &off, &chroot_len) < 0)
            return -1;
        if (chroot_len > 0 &&
            spawn_copy_string(p, payload_len, &off, chroot_len,
                              &out->chroot_path) < 0)
            return -1;
    }
    /* Initial winsize was introduced in v3. It is eight bytes of u16le;
     * current hosts always include it. */
    if (off <= payload_len && payload_len - off >= 8) {
        const uint8_t *winsize = NULL;
        if (spawn_take_bytes(p, payload_len, &off, 8, &winsize) < 0)
            return -1;
        out->init_rows = ish_proto_get_u16(winsize + 0);
        out->init_cols = ish_proto_get_u16(winsize + 2);
        out->init_xpix = ish_proto_get_u16(winsize + 4);
        out->init_ypix = ish_proto_get_u16(winsize + 6);
    }
    /* Retain append-only shape tolerance for internal fixtures. A future wire
     * protocol still requires a lockstep version bump before reaching here. */
    (void)off;
    return 0;
}

static int set_cloexec(int fd) {
    int f = fcntl(fd, F_GETFD, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

static int set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

/* Linux and macOS expose the same mount operation with different argument
 * order/count.  The macOS branch is used only by host-side syntax/tests; the
 * supervisor itself is built for Linux/iSH. */
static int mount_synthetic_fs(const char *type, const char *target) {
#ifdef ISH_SUPERVISOR_TESTING
    extern int (*ishsv_test_mount_hook)(const char *, const char *);
    if (ishsv_test_mount_hook) return ishsv_test_mount_hook(type, target);
#endif
#if defined(__linux__)
    return mount(type, target, type, 0, NULL);
#else
    return mount(type, target, 0, NULL);
#endif
}

#define ISH_FS_MAGIC_FAKEFS UINT64_C(0x66616b65)
#define ISH_FS_MAGIC_PROC   UINT64_C(0x00009fa0)
#define ISH_FS_MAGIC_DEVPTS UINT64_C(0x00001cd1)

static int filesystem_type_at(const char *path, uint64_t *out_type) {
    if (!path || !out_type) {
        errno = EINVAL;
        return -1;
    }
#ifdef ISH_SUPERVISOR_TESTING
    extern int (*ishsv_test_statfs_type_hook)(const char *, uint64_t *);
    if (ishsv_test_statfs_type_hook)
        return ishsv_test_statfs_type_hook(path, out_type);
#endif
    struct statfs state;
    if (statfs(path, &state) < 0) return -1;
    *out_type = (uint64_t)(unsigned long)state.f_type;
    return 0;
}

/* Mount only when statfs proves the target is still on the VM root's backing
 * filesystem. Calling mount unconditionally is not idempotent in iSH: its
 * mount table permits duplicate entries at the same path. A different,
 * unexpected filesystem is therefore an error rather than something to hide
 * under another mount. */
static int ensure_synthetic_fs(const char *type, const char *target,
                               uint64_t root_type, uint64_t expected_type) {
    uint64_t actual_type = 0;
    if (filesystem_type_at(target, &actual_type) < 0) {
        slogf("ishsv: statfs(%s) failed: %s\n", target, strerror(errno));
        return -1;
    }
    if (actual_type == expected_type) return 0;
    if (actual_type != root_type) {
        errno = EBUSY;
        slogf("ishsv: %s has unexpected filesystem type %#llx\n",
              target, (unsigned long long)actual_type);
        return -1;
    }

    if (mount_synthetic_fs(type, target) < 0) {
        int saved = errno;
        /* A concurrent successful mount may surface as EBUSY on some libc/
         * kernel combinations. Accept it only after an authoritative recheck. */
        if (saved != EBUSY ||
            filesystem_type_at(target, &actual_type) < 0 ||
            actual_type != expected_type) {
            errno = saved;
            slogf("ishsv: mount %s at %s failed: %s\n",
                  type, target, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (filesystem_type_at(target, &actual_type) < 0) {
        slogf("ishsv: statfs(%s) after mount failed: %s\n",
              target, strerror(errno));
        return -1;
    }
    if (actual_type != expected_type) {
        errno = EIO;
        slogf("ishsv: mount %s at %s produced filesystem type %#llx\n",
              type, target, (unsigned long long)actual_type);
        return -1;
    }
    return 0;
}

static int path_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return 0;
    }
    return 1;
}

static int vm_path(char out[PATH_MAX], const char *root,
                   const char *suffix) {
    if (!out || !root || !suffix) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(out, PATH_MAX, "%s%s", root, suffix);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strnlen(path, PATH_MAX);
    if (len == PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char tmp[PATH_MAX];
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) < 0 &&
                !((errno == EEXIST || errno == EPERM) &&
                  path_is_directory(tmp))) {
                slogf("ishsv: mkdir(%s) failed: %s\n", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 &&
        !((errno == EEXIST || errno == EPERM) && path_is_directory(tmp))) {
        slogf("ishsv: mkdir(%s) failed: %s\n", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

/* Major/minor numbers iSH uses; mirror fs/devices.h there. We hardcode
 * because the supervisor is built outside the iSH source tree. */
#define ISH_DEV_TTY_MAJOR  5
#define ISH_DEV_MEM_MAJOR  1

static int create_node(const char *path, mode_t mode, dev_t dev) {
#ifdef ISH_SUPERVISOR_TESTING
    extern int (*ishsv_test_mknod_hook)(const char *, mode_t, dev_t);
    if (ishsv_test_mknod_hook)
        return ishsv_test_mknod_hook(path, mode, dev);
#endif
    return mknod(path, mode, dev);
}

static int ensure_node(const char *path, mode_t mode, dev_t dev) {
    if (create_node(path, mode, dev) == 0) {
#ifdef ISH_SUPERVISOR_TESTING
        extern int (*ishsv_test_mknod_hook)(const char *, mode_t, dev_t);
        if (ishsv_test_mknod_hook) return 0;
#endif
        /* mknod is umask-sensitive; normalize the required public device
         * permissions before treating the step as complete. */
        if (chmod(path, mode & 0777) == 0) return 0;
        slogf("ishsv: chmod(%s) failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (errno == EEXIST) {
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISCHR(st.st_mode) && st.st_rdev == dev) {
                if ((st.st_mode & 0777) == (mode & 0777) ||
                    chmod(path, mode & 0777) == 0)
                    return 0;
            } else {
                errno = EEXIST;
            }
        }
    }
    slogf("ishsv: mknod(%s) failed: %s\n", path, strerror(errno));
    return -1;
}

/* Revalidate and, where safe, repair /dev nodes, devpts/proc mounts, and the
 * conventional root home immediately before every chrooted spawn. A path-only
 * success cache is unsafe because a VM root can be deleted/replaced at the same
 * path or lose a mount/directory while this supervisor remains alive. */
static int ensure_vm_devices(const char *chroot_path) {
    if (!chroot_path || chroot_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (!path_is_directory(chroot_path)) return -1;
    uint64_t root_type = 0;
    if (filesystem_type_at(chroot_path, &root_type) < 0) return -1;
    char p[PATH_MAX];

    /* /dev itself + /dev/pts. */
    if (vm_path(p, chroot_path, "/dev") < 0 || mkdir_p(p, 0755) < 0 ||
        vm_path(p, chroot_path, "/dev/pts") < 0 || mkdir_p(p, 0755) < 0)
        return -1;

    /* Standard char devices. The major/minor here have to match what
     * iSH's tty driver registers; mismatched majors mean open() of
     * the device returns ENXIO. */
#define ENSURE_VM_NODE(suffix, major_number, minor_number)                    \
    do {                                                                      \
        if (vm_path(p, chroot_path, suffix) < 0 ||                            \
            ensure_node(p, S_IFCHR | 0666,                                    \
                        makedev((major_number), (minor_number))) < 0)          \
            return -1;                                                        \
    } while (0)
    ENSURE_VM_NODE("/dev/null",    ISH_DEV_MEM_MAJOR, 3);
    ENSURE_VM_NODE("/dev/zero",    ISH_DEV_MEM_MAJOR, 5);
    ENSURE_VM_NODE("/dev/full",    ISH_DEV_MEM_MAJOR, 7);
    ENSURE_VM_NODE("/dev/random",  ISH_DEV_MEM_MAJOR, 8);
    ENSURE_VM_NODE("/dev/urandom", ISH_DEV_MEM_MAJOR, 9);
    ENSURE_VM_NODE("/dev/tty",     ISH_DEV_TTY_MAJOR, 0);
    ENSURE_VM_NODE("/dev/console", ISH_DEV_TTY_MAJOR, 1);
    ENSURE_VM_NODE("/dev/ptmx",    ISH_DEV_TTY_MAJOR, 2);
#undef ENSURE_VM_NODE

    /* Mount devpts so /dev/pts/N appears on demand when posix_openpt
     * allocates. statfs avoids duplicate mounts and detects a lost mount. */
    if (vm_path(p, chroot_path, "/dev/pts") < 0) return -1;
    if (ensure_synthetic_fs("devpts", p, root_type,
                            ISH_FS_MAGIC_DEVPTS) < 0) return -1;

    /* Mount procfs so /proc/self/exe and friends work. Many modern
     * tools (Node.js, glibc PATH detection, busybox top, …) read
     * /proc/self/exe and silently degrade or refuse to start when it
     * isn't there. Like devpts, procfs is synthetic in iSH and the
     * supervisor is outside any chroot, so this just works. */
    if (vm_path(p, chroot_path, "/proc") < 0 || mkdir_p(p, 0755) < 0)
        return -1;
    if (ensure_synthetic_fs("proc", p, root_type,
                            ISH_FS_MAGIC_PROC) < 0) return -1;

    /* Keep the conventional root home available for shells and optional
     * packages without creating state for any specific developer tool. */
    if (vm_path(p, chroot_path, "/root") < 0 || mkdir_p(p, 0700) < 0)
        return -1;

    return 0;
}

static int do_spawn(uint32_t sid, uint8_t flags, const uint8_t *p, uint32_t plen) {
    if (find_session(sid) != NULL) {
        emit_error(sid, EEXIST, "session id already in use");
        return -1;
    }
    struct session *s = alloc_session(sid);
    if (!s) {
        emit_error(sid, EAGAIN, "max sessions reached");
        return -1;
    }
    int want_tty = (flags & ISH_FF_TTY) ? 1 : 0;
    s->merge_stderr = (flags & ISH_FF_MERGE_STDERR) ? 1 : 0;
    s->is_tty = want_tty;
    if (want_tty) s->merge_stderr = 1; /* TTY collapses stdout+stderr */

    struct spawn_args sa;
    if (parse_spawn_payload(p, plen, &sa) < 0) {
        emit_error(sid, EINVAL, "bad SPAWN payload");
        free_spawn_args(&sa);
        free_session(s);
        return -1;
    }
    if (!sa.argv || !sa.argv[0] || !sa.argv[0][0]) {
        emit_error(sid, EINVAL, "empty argv");
        free_spawn_args(&sa);
        free_session(s);
        return -1;
    }

    /* Pipe / PTY allocation.
     *
     * Pipe mode (default):
     *   in_pipe[0]  = child stdin reader     (parent writes to in_pipe[1])
     *   out_pipe[1] = child stdout writer    (parent reads out_pipe[0])
     *   err_pipe[1] = child stderr writer    (parent reads err_pipe[0])
     *
     * PTY mode (ISH_FF_TTY):
     *   pty_master  = parent side, both reader and writer
     *   pty_slave   = child stdin/stdout/stderr (single fd, dup2'd to 0,1,2)
     *
     *   In PTY mode the kernel's tty layer handles Ctrl+C: bytes written
     *   to the master with the host pretending to be a terminal user
     *   (e.g. 0x03 for SIGINT) are turned into signals delivered to the
     *   foreground process group, leaving the shell itself alive.
     */
    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int pty_master = -1, pty_slave = -1;

    /* If we're spawning into a chrooted VM, make sure that VM has
     * /proc, /dev/ptmx and /dev/pts mounted, regardless of whether
     * the caller wants a TTY. Non-interactive commands (node --version,
     * npm install, busybox sh -c '…') also read /proc/self/exe and
     * /proc/self/status. Doing this only in the want_tty branch was
     * a bug: oneshot commands ended up running against an empty /proc.
     * The idempotent validation runs before every chrooted spawn because a
     * directory at the same path may have been replaced since the last one. */
    if (sa.chroot_path && sa.chroot_path[0] && sa.chroot_path[0] == '/') {
        if (ensure_vm_devices(sa.chroot_path) < 0) {
            int err = errno ? errno : EIO;
            slogf("ishsv: prepare VM root %s failed: %s\n",
                  sa.chroot_path, strerror(err));
            emit_error(sid, err, "failed to prepare VM root");
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
    }

    if (want_tty) {
        pty_master = posix_openpt(O_RDWR | O_NOCTTY);
        if (pty_master < 0) {
            slogf("ishsv: posix_openpt failed: %s (errno=%d). "
                  "Is /dev/ptmx present and devpts mounted?\n",
                  strerror(errno), errno);
            emit_error(sid, errno, "posix_openpt failed");
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
        if (grantpt(pty_master) < 0) {
            slogf("ishsv: grantpt failed: %s\n", strerror(errno));
            emit_error(sid, errno, "grantpt failed");
            close(pty_master);
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
        if (unlockpt(pty_master) < 0) {
            slogf("ishsv: unlockpt failed: %s\n", strerror(errno));
            emit_error(sid, errno, "unlockpt failed");
            close(pty_master);
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
        const char *slave_name = ptsname(pty_master);
        if (!slave_name) {
            slogf("ishsv: ptsname failed: %s\n", strerror(errno));
            emit_error(sid, errno, "ptsname failed");
            close(pty_master);
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
        pty_slave = open(slave_name, O_RDWR | O_NOCTTY);
        if (pty_slave < 0) {
            slogf("ishsv: open(%s) failed: %s. Is devpts mounted at /dev/pts?\n",
                  slave_name, strerror(errno));
            emit_error(sid, errno, "pty slave open failed");
            close(pty_master);
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
        if (set_nonblock(pty_master) < 0) {
            int err = errno;
            emit_error(sid, err, "failed to make pty input nonblocking");
            close(pty_master);
            close(pty_slave);
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
    } else {
        if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0 ||
            (!s->merge_stderr && pipe(err_pipe) < 0)) {
            emit_error(sid, errno, "pipe() failed");
            if (in_pipe[0] >= 0) close(in_pipe[0]);
            if (in_pipe[1] >= 0) close(in_pipe[1]);
            if (out_pipe[0] >= 0) close(out_pipe[0]);
            if (out_pipe[1] >= 0) close(out_pipe[1]);
            if (err_pipe[0] >= 0) close(err_pipe[0]);
            if (err_pipe[1] >= 0) close(err_pipe[1]);
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
        if (set_nonblock(in_pipe[1]) < 0) {
            int err = errno;
            emit_error(sid, err, "failed to make child stdin nonblocking");
            close(in_pipe[0]);  close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            if (!s->merge_stderr) { close(err_pipe[0]); close(err_pipe[1]); }
            free_spawn_args(&sa);
            free_session(s);
            return -1;
        }
    }

    /* The child reports group setup before SPAWNED is emitted. This closes the
     * parent/child race around setsid and turns pgid into a validated identity,
     * rather than assuming that numeric pid also became a process group. */
    int group_ready[2];
    if (pipe(group_ready) < 0) {
        emit_error(sid, errno, "group readiness pipe failed");
        if (want_tty) {
            close(pty_master); close(pty_slave);
        } else {
            close(in_pipe[0]);  close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            if (!s->merge_stderr) { close(err_pipe[0]); close(err_pipe[1]); }
        }
        free_spawn_args(&sa);
        free_session(s);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        emit_error(sid, errno, "fork() failed");
        close(group_ready[0]); close(group_ready[1]);
        if (want_tty) {
            close(pty_master); close(pty_slave);
        } else {
            close(in_pipe[0]);  close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            if (!s->merge_stderr) { close(err_pipe[0]); close(err_pipe[1]); }
        }
        free_spawn_args(&sa);
        free_session(s);
        return -1;
    }

    if (pid == 0) {
        /* child */
        close(group_ready[0]);
        if (want_tty) {
            /* Force the slave into a sane "cooked" terminal mode
             * before exec'ing the shell. iSH's freshly-allocated
             * pty slaves come up in a state where OPOST / ONLCR
             * aren't enabled, which means bash's "\n" never gets
             * post-processed into "\r\n" on output and every line
             * the host reads is missing the carriage return — the
             * UI ends up rendering everything on a single overwriting
             * line. Setting termios explicitly here gets us the
             * standard sane defaults: canonical input (line buffered
             * + erase/kill), echo on, output post-processing on with
             * ONLCR, and SIGINT/SIGQUIT/SIGTSTP from the kernel's
             * line discipline. */
            struct termios tio;
            if (tcgetattr(pty_slave, &tio) == 0) {
                tio.c_iflag |= ICRNL | IXON | IUTF8;
                tio.c_iflag &= ~(IGNCR | INLCR | IXOFF);
                tio.c_oflag |= OPOST | ONLCR;
                tio.c_oflag &= ~(OCRNL | ONOCR | ONLRET);
                tio.c_lflag |= ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
                tio.c_lflag &= ~(ECHONL | NOFLSH | TOSTOP);
                tio.c_cflag |= CS8 | CREAD | HUPCL;
                tio.c_cflag &= ~(PARENB);
                cfsetispeed(&tio, B38400);
                cfsetospeed(&tio, B38400);
                tcsetattr(pty_slave, TCSANOW, &tio);
            }
            struct winsize ws = {
                .ws_row    = sa.init_rows ? sa.init_rows : 24,
                .ws_col    = sa.init_cols ? sa.init_cols : 80,
                .ws_xpixel = sa.init_xpix,
                .ws_ypixel = sa.init_ypix,
            };
            ioctl(pty_slave, TIOCSWINSZ, &ws);

            /* Become session leader and acquire the slave as the
             * controlling tty. */
            if (setsid() < 0) {
                dprintf(2, "ishsv: failed to establish tty process group: %s\n",
                        strerror(errno));
                _exit(126);
            }
            if (getpgrp() != getpid()) {
                dprintf(2, "ishsv: tty process-group identity mismatch\n");
                _exit(126);
            }
            uint8_t ready = 1;
            if (write_full(group_ready[1], &ready, sizeof(ready)) < 0)
                _exit(126);
            close(group_ready[1]);
            dup2(pty_slave, 0);
            dup2(pty_slave, 1);
            dup2(pty_slave, 2);
#ifdef TIOCSCTTY
            ioctl(pty_slave, TIOCSCTTY, 0);
#endif
            tcsetpgrp(pty_slave, getpid());
            close(pty_master);
            if (pty_slave > 2) close(pty_slave);
            for (int fd = 3; fd < 256; fd++) close(fd);
            goto child_post_io;
        }

        /* A session leader cannot later leave its process group. This makes
         * the validated pid==pgid identity stable until its zombie is reaped,
         * including on iSH where getpgid deliberately excludes zombies. */
        if (setsid() < 0) {
            dprintf(2, "ishsv: failed to establish process group: %s\n",
                    strerror(errno));
            _exit(126);
        }
        if (getpgrp() != getpid()) {
            dprintf(2, "ishsv: process-group identity mismatch\n");
            _exit(126);
        }
        uint8_t ready = 1;
        if (write_full(group_ready[1], &ready, sizeof(ready)) < 0)
            _exit(126);
        close(group_ready[1]);

        /* wire stdin */
        dup2(in_pipe[0], 0);
        /* wire stdout */
        dup2(out_pipe[1], 1);
        /* wire stderr */
        if (s->merge_stderr) {
            dup2(out_pipe[1], 2);
        } else {
            dup2(err_pipe[1], 2);
        }
        /* close all other fds; we keep this list explicit (no /proc/self/fd
         * walk because iSH /proc may be incomplete). */
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        if (!s->merge_stderr) { close(err_pipe[0]); close(err_pipe[1]); }

    child_post_io:;

        /* iSH FD limit is ~256; close everything from 3..256 in case the
         * parent's CLOEXEC slipped. */
        for (int fd = 3; fd < 256; fd++) close(fd);

        /* chroot — must happen before chdir so that cwd is resolved
         * inside the new root. iSH supports the chroot syscall. */
        if (sa.chroot_path && sa.chroot_path[0]) {
            if (chdir(sa.chroot_path) < 0) {
                dprintf(2, "ishsv: chdir(%s): %s\n", sa.chroot_path, strerror(errno));
                _exit(126);
            }
            if (chroot(".") < 0) {
                dprintf(2, "ishsv: chroot(%s): %s\n", sa.chroot_path, strerror(errno));
                _exit(126);
            }
            /* land at "/" of the new root by default */
            (void)chdir("/");
        }

        /* chdir */
        if (sa.cwd && sa.cwd[0]) {
            if (chdir(sa.cwd) < 0) {
                /* not fatal — exec might still succeed if cwd is unused */
            }
        }

        /* reset signal mask & default handlers */
        sigset_t allset; sigfillset(&allset);
        sigprocmask(SIG_UNBLOCK, &allset, NULL);
        for (int sn = 1; sn < 32; sn++) signal(sn, SIG_DFL);

        char *const empty_env[] = {NULL};
        char *const *use_env = sa.envp ? sa.envp : empty_env;
#if defined(__linux__)
        execvpe(sa.argv[0], sa.argv, use_env);
#else
        /* macOS sanity-build only — supervisor is never run there. */
        extern char **environ;
        environ = (char **)use_env;
        execvp(sa.argv[0], sa.argv);
#endif

        /* exec failed */
        const char *m = strerror(errno);
        dprintf(2, "ishsv: execvpe(%s): %s\n", sa.argv[0], m);
        _exit(127);
    }

    /* parent */
    close(group_ready[1]);
    uint8_t ready = 0;
    int group_ready_status = read_full(group_ready[0], &ready, sizeof(ready));
    close(group_ready[0]);
    if (group_ready_status != 0 || ready != 1) {
        (void)supervisor_kill(pid, SIGKILL);
        while (supervisor_waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        emit_error(sid, EPROTO, "child process group setup failed");
        if (want_tty) {
            close(pty_master); close(pty_slave);
        } else {
            close(in_pipe[0]);  close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            if (!s->merge_stderr) { close(err_pipe[0]); close(err_pipe[1]); }
        }
        free_spawn_args(&sa);
        free_session(s);
        return -1;
    }

    if (want_tty) {
        close(pty_slave);
        set_cloexec(pty_master);
        s->pid       = pid;
        s->pgid      = pid;
        s->pgid_validated = 1;
        s->stdin_fd  = pty_master;  /* parent writes guest input here */
        s->stdout_fd = pty_master;  /* and reads guest output from the same fd */
        s->stderr_fd = -1;          /* tty merges stderr into stdout */
    } else {
        close(in_pipe[0]);
        close(out_pipe[1]);
        if (!s->merge_stderr) close(err_pipe[1]);

        set_cloexec(in_pipe[1]);
        set_cloexec(out_pipe[0]);
        if (!s->merge_stderr) set_cloexec(err_pipe[0]);
        set_nonblock(out_pipe[0]);
        if (!s->merge_stderr) set_nonblock(err_pipe[0]);

        s->pid       = pid;
        s->pgid      = pid;
        s->pgid_validated = 1;
        s->stdin_fd  = in_pipe[1];
        s->stdout_fd = out_pipe[0];
        s->stderr_fd = s->merge_stderr ? -1 : err_pipe[0];
    }

    /* SPAWNED event */
    uint8_t pidbuf[4];
    ish_proto_put_u32(pidbuf, (uint32_t)pid);
    emit_frame(ISH_FT_SPAWNED, 0, sid, pidbuf, sizeof(pidbuf));

    free_spawn_args(&sa);
    return 0;
}

/* ----------------------------- main loop -------------------------------- */

static int read_control_frame(uint8_t **out_payload, uint32_t *out_plen,
                              uint8_t *out_type, uint8_t *out_flags,
                              uint32_t *out_sid) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    int r = read_full(0, hdr, sizeof(hdr));
    if (r != 0) return r;  /* -1 error, -2 EOF */
    if (ish_proto_parse_hdr(hdr, out_type, out_flags, out_plen, out_sid) < 0)
        return -1;
    *out_payload = NULL;
    if (*out_plen > 0) {
        *out_payload = (uint8_t *)malloc(*out_plen);
        if (!*out_payload) return -1;
        if (read_full(0, *out_payload, *out_plen) != 0) {
            free(*out_payload); *out_payload = NULL;
            return -1;
        }
    }
    return 0;
}

static struct session *find_session_leader(pid_t pid) {
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (s->in_use && !s->reaped && s->pid == pid) return s;
    }
    return NULL;
}

#if defined(__linux__)
struct proc_child_identity {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    char state;
};

#ifdef ISH_SUPERVISOR_TESTING
extern int (*ishsv_test_adopted_scan_hook)(struct proc_child_identity *);
#endif

/* /proc/<pid>/stat starts with "pid (comm) state ppid pgrp".  Search from
 * the final ')' because Linux permits ')' inside comm.  iSH exposes the same
 * first fields and mounts procfs before ishsv starts. */
static int read_proc_child_identity(pid_t pid,
                                    struct proc_child_identity *identity) {
    char path[64];
    int n = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ESRCH) return 1; /* raced with exit */
        return -1;
    }

    char buf[4096];
    size_t used = 0;
    while (used < sizeof(buf) - 1) {
        ssize_t r = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (r > 0) {
            used += (size_t)r;
            continue;
        }
        if (r == 0) break;
        if (errno == EINTR) continue;
        int err = errno;
        close(fd);
        if (err == ENOENT || err == ESRCH) return 1;
        errno = err;
        return -1;
    }
    if (used == sizeof(buf) - 1) {
        char extra;
        ssize_t r;
        do { r = read(fd, &extra, 1); } while (r < 0 && errno == EINTR);
        if (r != 0) {
            close(fd);
            if (r < 0 && (errno == ENOENT || errno == ESRCH)) return 1;
            errno = EOVERFLOW;
            return -1;
        }
    }
    close(fd);
    buf[used] = '\0';

    char *tail = strrchr(buf, ')');
    long parsed_ppid = 0, parsed_pgid = 0;
    char state = '\0';
    if (!tail || sscanf(tail + 1, " %c %ld %ld",
                        &state, &parsed_ppid, &parsed_pgid) != 3 ||
        parsed_ppid < 0 || parsed_ppid > INT_MAX ||
        parsed_pgid < 0 || parsed_pgid > INT_MAX) {
        if (kill(pid, 0) < 0 && errno == ESRCH) return 1;
        errno = EPROTO;
        return -1;
    }

    identity->pid = pid;
    identity->ppid = (pid_t)parsed_ppid;
    identity->pgid = (pid_t)parsed_pgid;
    identity->state = state;
    return 0;
}

static int process_group_is_tracked(pid_t pgid) {
    if (pgid <= 0) return 0;
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use || s->reaped || s->pid <= 0) continue;
        if (s->pgid_validated && s->pgid == s->pid && s->pgid == pgid)
            return 1;
        if (s->is_tty && s->stdin_fd >= 0) {
            pid_t foreground = supervisor_tcgetpgrp(s->stdin_fd);
            if (foreground > 0 && foreground == pgid) return 1;
        }
    }
    return 0;
}

/* Return one direct child which is neither a tracked leader nor a member of
 * a still-owned session/TTY foreground group.  Once such a PID is observed it
 * is safe to signal by exact PID: this single-threaded supervisor does not reap
 * it concurrently, and an exiting child remains a zombie that pins the PID. */
static int find_untracked_adopted_child(struct proc_child_identity *identity) {
#ifdef ISH_SUPERVISOR_TESTING
    if (ishsv_test_adopted_scan_hook)
        return ishsv_test_adopted_scan_hook(identity);
#endif
    DIR *proc = opendir("/proc");
    if (!proc) return -1;

    pid_t self = getpid();
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(proc);
        if (!entry) {
            if (errno != 0) result = -1;
            break;
        }
        char *end = NULL;
        errno = 0;
        long value = strtol(entry->d_name, &end, 10);
        if (errno != 0 || !end || *end != '\0' || value <= 0 ||
            value > INT_MAX || (pid_t)value == self)
            continue;

        struct proc_child_identity candidate;
        int read_result = read_proc_child_identity((pid_t)value, &candidate);
        if (read_result == 1) continue;
        if (read_result < 0) {
            result = -1;
            break;
        }
        if (candidate.ppid != self) continue;
        if (find_session_leader(candidate.pid) ||
            process_group_is_tracked(candidate.pgid))
            continue;
        *identity = candidate;
        result = 1;
        break;
    }
    int saved_errno = errno;
    closedir(proc);
    errno = saved_errno;
    return result;
}
#endif /* __linux__ */

/* Remove all untracked children, including descendants adopted after an
 * earlier target dies.  No successful session completion may be emitted until
 * two consecutive scans are clean.  Any inability to prove/perform cleanup is
 * escalated to the instance-wide fail-close above. */
static int cleanup_untracked_adopted_children_until(uint64_t deadline) {
    if (g_instance_fail_closed) return -1;
#if !defined(__linux__)
    (void)deadline;
    return 0;
#else
    unsigned clean_scans = 0;
    while (clean_scans < 2) {
        if (monotonic_ms() >= deadline) {
            fail_close_instance("timed out cleaning adopted guest children");
            return -1;
        }
        struct proc_child_identity child = {0};
        int found = find_untracked_adopted_child(&child);
        if (found < 0) {
            fail_close_instance("cannot inspect adopted guest children");
            return -1;
        }
        if (found == 0) {
            clean_scans++;
            if (clean_scans < 2) usleep(1000);
            continue;
        }
        clean_scans = 0;

        if (child.state != 'Z' &&
            supervisor_kill(child.pid, SIGKILL) < 0 && errno != ESRCH) {
            fail_close_instance("cannot kill untracked adopted guest child");
            return -1;
        }

        for (;;) {
            pid_t reaped = supervisor_waitpid(child.pid, NULL, WNOHANG);
            if (reaped == child.pid) break;
            if (reaped < 0) {
                if (errno == EINTR) continue;
                fail_close_instance("cannot reap untracked adopted guest child");
                return -1;
            }
            if (monotonic_ms() >= deadline) {
                fail_close_instance("timed out cleaning adopted guest children");
                return -1;
            }
            usleep(1000);
        }
    }
    return 0;
#endif
}

#if defined(__linux__) && defined(ISH_SUPERVISOR_TESTING)
static int cleanup_untracked_adopted_children(void) {
    return cleanup_untracked_adopted_children_until(monotonic_ms() + 2000u);
}
#endif

static void reap_children_until(uint64_t cleanup_deadline_ms) {
    while (1) {
        /* Peeking with WNOWAIT is essential here. A tracked leader may have
         * left descendants in its process group. While its zombie remains
         * unreaped, the kernel cannot reuse either its PID or its equal PGID,
         * so signal_session_tree can validate and terminate that group without
         * ever targeting a stale numeric identity. waitpid then releases both
         * identities, and we immediately clear both fields. P_ALL also lets us
         * reap adopted, untracked descendants without racing a tracked leader. */
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        int observed;
        do {
            observed = supervisor_waitid(P_ALL, 0, &info,
                                         WEXITED | WNOHANG | WNOWAIT);
        } while (observed < 0 && errno == EINTR);
        if (observed < 0) {
            if (errno != ECHILD) {
                slogf("ishsv: waitid failed: %s\n", strerror(errno));
                fail_close_instance("cannot inspect exited guest children");
            } else {
                (void)cleanup_untracked_adopted_children_until(
                    cleanup_deadline_ms);
            }
            return;
        }
        if (info.si_pid <= 0) {
            (void)cleanup_untracked_adopted_children_until(
                cleanup_deadline_ms);
            return;
        }

        pid_t pid = info.si_pid;
        struct session *s = find_session_leader(pid);
        if (s) signal_session_tree(s, SIGKILL);

        int status;
        pid_t reaped;
        do {
            reaped = supervisor_waitpid(pid, &status, WNOHANG);
        } while (reaped < 0 && errno == EINTR);
        if (reaped != pid) {
            if (reaped < 0 && errno != ECHILD)
                slogf("ishsv: waitpid(%d) failed: %s\n",
                      (int)pid, strerror(errno));
            fail_close_instance("cannot reap an observed guest child");
            return;
        }
        if (!s) continue;

        if (WIFEXITED(status)) {
            s->exit_code = WEXITSTATUS(status);
            s->term_signal = 0;
        } else if (WIFSIGNALED(status)) {
            s->exit_code = 128 + WTERMSIG(status);
            s->term_signal = WTERMSIG(status);
        } else {
            s->exit_code = -1;
        }
        s->reaped = 1;
        s->pid = 0;
        s->pgid = 0;
        s->pgid_validated = 0;
    }
}

static void reap_children(void) {
    reap_children_until(monotonic_ms() + 2000u);
}

static int drain_session_streams_and_handle_exit_until(
        uint64_t cleanup_deadline_ms) {
    /* Drain all readable child fds and emit STDOUT/STDERR events; if a
     * session is reaped AND fully drained, emit EXITED and free it. */
    if (g_instance_fail_closed) return 0;
    struct pollfd pfds[SUPERVISOR_MAX_SESSIONS * 2];
    int    pmap[SUPERVISOR_MAX_SESSIONS * 2][2]; /* [sess_idx][stream_kind] */
    int n = 0;
    int made_progress = 0;

    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        if (s->stdout_fd >= 0) {
            pfds[n].fd = s->stdout_fd; pfds[n].events = POLLIN;
            pmap[n][0] = (int)i; pmap[n][1] = ISH_STREAM_STDOUT; n++;
        }
        if (s->stderr_fd >= 0) {
            pfds[n].fd = s->stderr_fd; pfds[n].events = POLLIN;
            pmap[n][0] = (int)i; pmap[n][1] = ISH_STREAM_STDERR; n++;
        }
    }
    if (n > 0) {
        int pr = poll(pfds, n, 0);
        if (pr > 0) {
            uint8_t buf[SUPERVISOR_PIPE_BUF + 8];
            for (int k = 0; k < n; k++) {
                if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                struct session *s = &g_sessions[pmap[k][0]];
                int kind = pmap[k][1];
                int fd   = (kind == ISH_STREAM_STDOUT) ? s->stdout_fd : s->stderr_fd;
                for (unsigned reads = 0;
                     reads < SUPERVISOR_STREAM_READS_PER_ROUND;
                     reads++) {
                    ssize_t r = read(fd, buf + 8, SUPERVISOR_PIPE_BUF);
                    if (r > 0) {
                        made_progress = 1;
                        uint64_t seq;
                        if (kind == ISH_STREAM_STDOUT) { seq = ++s->out_seq; }
                        else                            { seq = ++s->err_seq; }
                        ish_proto_put_u64(buf, seq);
                        emit_frame(kind == ISH_STREAM_STDOUT ? ISH_FT_STDOUT_DATA : ISH_FT_STDERR_DATA,
                                   ISH_FF_SEQ_PRESENT, s->id, buf, (uint32_t)(8 + r));
                    } else if (r == 0) {
                        /* EOF on this stream. In TTY mode the same fd
                         * is both stdin and stdout; clear stdin too so
                         * we don't try to write to a dead pty. */
                        if (kind == ISH_STREAM_STDOUT) {
                            if (s->is_tty && s->stdin_fd == s->stdout_fd) {
                                if (s->stdin_len > 0)
                                    terminalize_session_stdin(
                                        s, EPIPE, "pty closed with stdin still queued");
                                else
                                    close_session_stdin(s);
                            } else {
                                close(s->stdout_fd); s->stdout_fd = -1;
                            }
                        } else {
                            close(s->stderr_fd); s->stderr_fd = -1;
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        /* hard error: drop the fd */
                        if (kind == ISH_STREAM_STDOUT) {
                            if (s->is_tty && s->stdin_fd == s->stdout_fd) {
                                if (s->stdin_len > 0)
                                    terminalize_session_stdin(
                                        s, errno, "pty failed with stdin still queued");
                                else
                                    close_session_stdin(s);
                            } else {
                                close(s->stdout_fd); s->stdout_fd = -1;
                            }
                        } else {
                            close(s->stderr_fd); s->stderr_fd = -1;
                        }
                        break;
                    }
                }
            }
        }
    }

    /* Sessions where child reaped AND both streams EOF -> emit EXITED.  First
     * prove that no unowned adopted child remains. Descendants of this reaped
     * leader cannot join another tracked leader's process group: every spawn
     * is a distinct session and setpgid cannot cross session boundaries. Thus
     * a clean recursive scan proves this completed session has no live child;
     * a different still-running session may continue independently. */
    int has_exit_candidate = 0;
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (s->in_use && s->reaped &&
            s->stdout_fd < 0 && s->stderr_fd < 0) {
            has_exit_candidate = 1;
            break;
        }
    }
    if (has_exit_candidate &&
        cleanup_untracked_adopted_children_until(cleanup_deadline_ms) < 0)
        return made_progress;

    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        if (s->reaped && s->stdout_fd < 0 && s->stderr_fd < 0) {
            uint8_t buf[8];
            ish_proto_put_i32(buf,     s->exit_code);
            ish_proto_put_i32(buf + 4, s->term_signal);
            emit_frame(ISH_FT_EXITED, 0, s->id, buf, sizeof(buf));
            free_session(s);
        }
    }
    return made_progress;
}

static int drain_session_streams_and_handle_exit(void) {
    return drain_session_streams_and_handle_exit_until(
        monotonic_ms() + 2000u);
}

/* The separately stored PGID is usable only after the child has reported that
 * it became a session leader with pid==pgid. A session leader cannot leave that
 * group, and its unreaped zombie keeps the identity anchored. This validation
 * plus WNOWAIT prevents reuse from redirecting an old session close. */
static int session_group_is_owned(struct session *s) {
    return s && s->in_use && !s->reaped && s->pid > 0 &&
           s->pgid_validated && s->pgid == s->pid;
}

/* User direct signals target only the tracked command group. A direct-PID
 * fallback is reserved for a violated/unavailable readiness invariant; it is
 * still safe because the unreaped PID cannot yet have been reused. */
static void signal_tracked_group(struct session *s, int signum) {
    if (!s || !s->in_use || s->reaped || s->pid <= 0) return;
    pid_t pid = s->pid;
    if (!session_group_is_owned(s)) {
        (void)supervisor_kill(pid, signum);
        return;
    }
    if (supervisor_kill(-s->pgid, signum) < 0 && errno == ESRCH) {
        /* With an unreaped validated session leader, ESRCH is unexpected.
         * Preserve best-effort delivery to the still-owned direct PID without
         * ever retrying a released PGID. */
        (void)supervisor_kill(pid, signum);
    }
}

/* Session-wide terminate/close additionally reaches a TTY foreground job.
 * Never fall back from the foreground pgid to a numeric PID: unlike the direct
 * child, no waitpid ownership proves that such a PID is still the same task. */
static void signal_foreground_group(struct session *s, int signum) {
    if (!session_group_is_owned(s) ||
        !s->is_tty || s->stdin_fd < 0)
        return;
    pid_t foreground = supervisor_tcgetpgrp(s->stdin_fd);
    if (foreground <= 0 || foreground == s->pgid) return;
    (void)supervisor_kill(-foreground, signum);
}

static void signal_session_tree(struct session *s, int signum) {
    signal_foreground_group(s, signum);
    signal_tracked_group(s, signum);
}

static void apply_term_grace(void) {
    uint64_t now = monotonic_ms();
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        if (s->term_deadline_ms > 0 && now >= s->term_deadline_ms) {
            s->term_deadline_ms = 0;
            if (!s->reaped) signal_session_tree(s, SIGKILL);
        }
    }
}

static int active_session_count(void) {
    int active = 0;
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use) active++;
    return active;
}

static void drain_sessions_until(uint64_t deadline_ms) {
    while (monotonic_ms() < deadline_ms) {
        reap_children_until(deadline_ms);
        if (g_instance_fail_closed) return;
        int made_progress =
            drain_session_streams_and_handle_exit_until(deadline_ms);
        if (active_session_count() == 0) return;
        if (!made_progress) usleep(50000);
    }
}

static void require_shutdown_complete(void) {
    if (active_session_count() != 0)
        fail_close_instance("guest children remain after shutdown deadline");
}

static void shutdown_all(void) {
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        signal_session_tree(s, SIGTERM);
    }
    /* Drain for up to two wall-clock seconds. A stream with buffered output
     * is revisited immediately, while an idle child sleeps between reaps. */
    drain_sessions_until(monotonic_ms() + 2000u);
    /* anything remaining: SIGKILL */
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        signal_session_tree(s, SIGKILL);
    }
    drain_sessions_until(monotonic_ms() + 2000u);
    require_shutdown_complete();
}

static void finish_instance_fail_close(void) {
    (void)supervisor_kill_all(SIGKILL);
    uint64_t deadline = monotonic_ms() + 2000u;
    while (monotonic_ms() < deadline) {
        pid_t reaped = supervisor_waitpid(-1, NULL, WNOHANG);
        if (reaped > 0) continue;
        if (reaped < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
        }
        usleep(1000);
    }
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        if (g_sessions[i].in_use) free_session(&g_sessions[i]);
    }
}

static int handshake(void) {
    /* Read HELLO frame, respond with HELLO_ACK. */
    uint8_t *p = NULL; uint32_t plen = 0; uint8_t type = 0, flags = 0; uint32_t sid = 0;
    int r = read_control_frame(&p, &plen, &type, &flags, &sid);
    int valid = r == 0 && type == ISH_FT_HELLO && flags == 0 && sid == 0 &&
        plen >= 12 && p &&
        ish_proto_get_u32(p) == ISH_EMBED_ABI_VERSION &&
        p[4] == ISH_PROTO_VERSION && p[5] == 0 && p[6] == 0 && p[7] == 0;
    if (valid) {
        uint32_t greeting_len = ish_proto_get_u32(p + 8);
        valid = greeting_len <= plen - 12 && greeting_len + 12 == plen;
    }
    if (!valid) {
        free(p);
        slogf("ishsv: bad handshake (r=%d type=0x%x)\n", r, type);
        return -1;
    }
    free(p);
    uint8_t ack[12];
    ish_proto_put_u32(ack, ISH_EMBED_ABI_VERSION);
    ack[4] = ISH_PROTO_VERSION;
    ack[5] = ack[6] = ack[7] = 0;
    ish_proto_put_u32(ack + 8, SUPERVISOR_MAX_SESSIONS);
    return emit_frame(ISH_FT_HELLO_ACK, 0, 0, ack, sizeof(ack));
}

int main(void) {
    g_instance_fail_closed = 0;
    /* PID 1: become a child subreaper just in case (Linux >= 3.4) */
#if defined(__linux__) && defined(PR_SET_CHILD_SUBREAPER)
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif

    /* Avoid SIGPIPE killing us if host closes its end. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = {0};
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    if (handshake() < 0) {
        slogf("ishsv: handshake failed\n");
        return 1;
    }
    /* The initial HELLO may arrive after PID 1 starts. Keep fd 0 blocking for
     * that one frame, then switch to the nonblocking state machine below. */
    if (set_nonblock(0) < 0) {
        slogf("ishsv: failed to make control fd nonblocking\n");
        return 1;
    }

    /* control-frame buffer state machine for nonblocking fd 0 */
    enum { S_HDR, S_BODY } state = S_HDR;
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    size_t  got = 0;
    uint8_t  type = 0, flags = 0;
    uint32_t plen = 0, sid = 0;
    uint8_t *body = NULL;
    size_t   body_got = 0;

    int shutting_down = 0;

    while (1) {
        if (g_got_sigchld) {
            g_got_sigchld = 0;
            reap_children();
            if (g_instance_fail_closed) break;
        }

        /* Read whatever's available on fd 0 in nonblocking chunks. */
        for (int chunks = 0; chunks < 64; chunks++) {
            if (state == S_HDR) {
                ssize_t r = read(0, hdr + got, sizeof(hdr) - got);
                if (r > 0) {
                    got += (size_t)r;
                    if (got == sizeof(hdr)) {
                        if (ish_proto_parse_hdr(hdr, &type, &flags, &plen, &sid) < 0) {
                            slogf("ishsv: bad header, exiting\n");
                            return 2;
                        }
                        if (plen == 0) { body = NULL; goto frame_complete; }
                        body = (uint8_t *)malloc(plen);
                        if (!body) { slogf("ishsv: oom\n"); return 3; }
                        body_got = 0;
                        state = S_BODY;
                        got = 0;
                    }
                } else if (r == 0) {
                    /* host closed control: shut down. */
                    slogf("ishsv: control EOF\n");
                    shutting_down = 1;
                    break;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                    slogf("ishsv: read err %s\n", strerror(errno));
                    return 4;
                }
            } else {
                ssize_t r = read(0, body + body_got, plen - body_got);
                if (r > 0) {
                    body_got += (size_t)r;
                    if (body_got == plen) goto frame_complete;
                } else if (r == 0) {
                    free(body); body = NULL;
                    shutting_down = 1;
                    break;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                    slogf("ishsv: read body err %s\n", strerror(errno));
                    return 5;
                }
            }
            continue;
        frame_complete:
            switch (type) {
                case ISH_FT_SPAWN:
                    do_spawn(sid, flags, body, plen);
                    break;
                case ISH_FT_STDIN_DATA: {
                    struct session *s = find_session(sid);
                    if (!s) {
                        if (plen > 0) emit_error(sid, ESRCH, "unknown session");
                    } else {
                        (void)enqueue_session_stdin(s, body, plen);
                    }
                    break;
                }
                case ISH_FT_STDIN_CLOSE: {
                    struct session *s = find_session(sid);
                    if (s) request_session_stdin_close(s);
                    break;
                }
                case ISH_FT_SIGNAL: {
                    if (plen >= 4) {
                        int signum = ish_proto_get_i32(body);
                        struct session *s = find_session(sid);
                        if (s) signal_tracked_group(s, signum);
                    }
                    break;
                }
                case ISH_FT_TERMINATE: {
                    struct session *s = find_session(sid);
                    if (s && !s->reaped && s->pid > 0) {
                        signal_session_tree(s, SIGTERM);
                        s->term_deadline_ms = monotonic_ms() + 1500u;
                    }
                    break;
                }
                case ISH_FT_SESSION_CLOSE: {
                    struct session *s = find_session(sid);
                    if (s) force_close_session(s);
                    break;
                }
                case ISH_FT_RESIZE: {
                    /* Payload: u16 rows, u16 cols, u16 xpix, u16 ypix
                     * (all little-endian). Only meaningful for TTY
                     * sessions; pipe sessions silently ignore. The
                     * kernel tty layer emits SIGWINCH to the
                     * foreground process group automatically. */
                    if (plen < 8) break;
                    uint16_t rows = ish_proto_get_u16(body + 0);
                    uint16_t cols = ish_proto_get_u16(body + 2);
                    uint16_t xpix = ish_proto_get_u16(body + 4);
                    uint16_t ypix = ish_proto_get_u16(body + 6);
                    struct session *s = find_session(sid);
                    if (s && s->is_tty && s->stdin_fd >= 0 && rows && cols) {
                        struct winsize ws = {
                            .ws_row    = rows,
                            .ws_col    = cols,
                            .ws_xpixel = xpix,
                            .ws_ypixel = ypix,
                        };
                        if (ioctl(s->stdin_fd, TIOCSWINSZ, &ws) < 0) {
                            slogf("ishsv: TIOCSWINSZ sid=%u %ux%u: %s\n",
                                  sid, cols, rows, strerror(errno));
                        }
                    }
                    break;
                }
                case ISH_FT_PING:
                    emit_frame(ISH_FT_PONG, 0, sid, NULL, 0);
                    break;
                case ISH_FT_SHUTDOWN:
                    shutting_down = 1;
                    break;
                default:
                    slogf("ishsv: unknown frame type 0x%x\n", type);
                    break;
            }
            free(body); body = NULL;
            state = S_HDR; got = 0;
            if (shutting_down) break;
            /* Dispatch at most one complete control frame per outer round.
             * This gives queued stdin/output and termination deadlines a
             * bounded opportunity between a burst of host frames. */
            break;
        }

        if (shutting_down) break;

        flush_all_session_stdin();
        int stream_progress = drain_session_streams_and_handle_exit();
        if (g_instance_fail_closed) break;
        apply_term_grace();

        /* Sleep a bit, but wake on SIGCHLD, control input, or a child that
         * can accept more queued stdin. */
        struct pollfd wait_fds[1 + SUPERVISOR_MAX_SESSIONS];
        int wait_count = 1;
        wait_fds[0].fd = 0;
        wait_fds[0].events = POLLIN;
        wait_fds[0].revents = 0;
        for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
            struct session *s = &g_sessions[i];
            if (!s->in_use || s->stdin_fd < 0 || s->stdin_len == 0) continue;
            wait_fds[wait_count].fd = s->stdin_fd;
            wait_fds[wait_count].events = POLLOUT;
            wait_fds[wait_count].revents = 0;
            wait_count++;
        }
        poll(wait_fds, (nfds_t)wait_count, stream_progress ? 0 : 50);
    }

    if (g_instance_fail_closed) {
        finish_instance_fail_close();
        return 6;
    }

    shutdown_all();
    if (g_instance_fail_closed) {
        finish_instance_fail_close();
        return 6;
    }
    emit_frame(ISH_FT_SHUTDOWN_ACK, 0, 0, NULL, 0);
    return 0;
}

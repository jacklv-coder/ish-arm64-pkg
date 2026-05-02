/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ishsv — PID 1 supervisor that runs inside iSH.
 *
 * It is built as a static i386 musl ELF and copied into the rootfs at
 * /sbin/ishsv via the kernel's generic_open / fake-aware install (so
 * fakefs metadata is correct).
 *
 * Responsibilities:
 *  - On startup, read fd 0 (multiplexed framed protocol) for commands.
 *  - On SPAWN, fork+execve a new process group; wire its stdin to a pipe
 *    we own, and its stdout/stderr to pipes we own; emit framed events
 *    on fd 1.
 *  - poll() on all child fds + control fd; never block on a single child.
 *  - On SIGNAL { signum }, kill(-pgid, signum). SIGINT == Ctrl+C.
 *  - On TERMINATE, SIGTERM the pgid; reaper sends SIGKILL after grace.
 *  - On STDIN_CLOSE, close(child_stdin).
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
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>  /* makedev */
#include <limits.h>         /* PATH_MAX */
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>

#include "../protocol/proto.h"

#ifndef SUPERVISOR_MAX_SESSIONS
#define SUPERVISOR_MAX_SESSIONS 64
#endif

#ifndef SUPERVISOR_PIPE_BUF
#define SUPERVISOR_PIPE_BUF (64 * 1024)
#endif

/* ----------------------------- session table ----------------------------- */

struct session {
    int       in_use;
    uint32_t  id;
    pid_t     pid;        /* also the pgid (we setpgid in child)              */
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
    /* SIGTERM sent at this monotonic-ish counter (poll-tick units); 0 = none */
    int       term_grace_ticks;
};

static struct session g_sessions[SUPERVISOR_MAX_SESSIONS];

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

/* Used to keep the protocol channel itself out of any signal-time path. */
static volatile sig_atomic_t g_got_sigchld = 0;
static void on_sigchld(int sig) { (void)sig; g_got_sigchld = 1; }

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

static void free_session(struct session *s) {
    /* In TTY mode stdin_fd == stdout_fd (both point at the pty master).
     * Avoid the double close — second one would EBADF and could race
     * the kernel reusing that fd number. */
    int closed_stdin = -1;
    if (s->stdin_fd  >= 0) { close(s->stdin_fd);  closed_stdin = s->stdin_fd; }
    if (s->stdout_fd >= 0 && s->stdout_fd != closed_stdin) close(s->stdout_fd);
    if (s->stderr_fd >= 0) close(s->stderr_fd);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------- spawn ---------------------------------- */

/* Parse SPAWN payload (returns malloc'd argv/envp arrays + cwd). */
struct spawn_args {
    char  *cwd;
    char **argv;
    char **envp;
    char  *chroot_path;   /* NULL = no chroot */
};

static void free_spawn_args(struct spawn_args *a) {
    if (!a) return;
    free(a->cwd);
    free(a->chroot_path);
    if (a->argv) { for (int i = 0; a->argv[i]; i++) free(a->argv[i]); free(a->argv); }
    if (a->envp) { for (int i = 0; a->envp[i]; i++) free(a->envp[i]); free(a->envp); }
    memset(a, 0, sizeof(*a));
}

static int parse_spawn_payload(const uint8_t *p, uint32_t plen,
                               struct spawn_args *out) {
    memset(out, 0, sizeof(*out));
    uint32_t off = 0;
    if (plen < 4) return -1;
    uint32_t cwd_len = ish_proto_get_u32(p + off); off += 4;
    if (off + cwd_len > plen) return -1;
    out->cwd = (char *)malloc(cwd_len + 1);
    if (!out->cwd) return -1;
    if (cwd_len) memcpy(out->cwd, p + off, cwd_len);
    out->cwd[cwd_len] = 0;
    off += cwd_len;

    if (off + 4 > plen) return -1;
    uint32_t argc = ish_proto_get_u32(p + off); off += 4;
    if (argc > 4096) return -1;
    out->argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!out->argv) return -1;
    for (uint32_t i = 0; i < argc; i++) {
        if (off + 4 > plen) return -1;
        uint32_t L = ish_proto_get_u32(p + off); off += 4;
        if (off + L > plen) return -1;
        out->argv[i] = (char *)malloc(L + 1);
        if (!out->argv[i]) return -1;
        if (L) memcpy(out->argv[i], p + off, L);
        out->argv[i][L] = 0;
        off += L;
    }

    if (off + 4 > plen) return -1;
    uint32_t envc = ish_proto_get_u32(p + off); off += 4;
    if (envc > 4096) return -1;
    out->envp = (char **)calloc((size_t)envc + 1, sizeof(char *));
    if (!out->envp) return -1;
    for (uint32_t i = 0; i < envc; i++) {
        if (off + 4 > plen) return -1;
        uint32_t L = ish_proto_get_u32(p + off); off += 4;
        if (off + L > plen) return -1;
        out->envp[i] = (char *)malloc(L + 1);
        if (!out->envp[i]) return -1;
        if (L) memcpy(out->envp[i], p + off, L);
        out->envp[i][L] = 0;
        off += L;
    }
    /* Optional chroot field (v2). Older hosts/messages stop here. */
    if (off + 4 <= plen) {
        uint32_t chL = ish_proto_get_u32(p + off); off += 4;
        if (off + chL > plen) return -1;
        if (chL > 0) {
            out->chroot_path = (char *)malloc(chL + 1);
            if (!out->chroot_path) return -1;
            memcpy(out->chroot_path, p + off, chL);
            out->chroot_path[chL] = 0;
        }
        off += chL;
    }
    if (off != plen) return -1;
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

/* Track which chroot roots we've already populated with device nodes
 * + devpts mount during this supervisor's lifetime, so we don't redo
 * the work on every spawn into the same VM. */
static char ensured_vms[SUPERVISOR_MAX_SESSIONS][PATH_MAX];
static int  ensured_vm_count = 0;

static int already_ensured(const char *root) {
    for (int i = 0; i < ensured_vm_count; i++)
        if (strcmp(ensured_vms[i], root) == 0)
            return 1;
    return 0;
}

static void mark_ensured(const char *root) {
    if (ensured_vm_count >= SUPERVISOR_MAX_SESSIONS) return;
    snprintf(ensured_vms[ensured_vm_count], sizeof(ensured_vms[0]),
             "%s", root);
    ensured_vm_count++;
}

static int mkdir_p(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) return 0;
    /* try to create parents */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
                slogf("ishsv: mkdir(%s) failed: %s\n", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        slogf("ishsv: mkdir(%s) failed: %s\n", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

/* Major/minor numbers iSH uses; mirror fs/devices.h there. We hardcode
 * because the supervisor is built outside the iSH source tree. */
#define ISH_DEV_TTY_MAJOR  5
#define ISH_DEV_MEM_MAJOR  1

static void ensure_node(const char *path, mode_t mode, dev_t dev) {
    if (mknod(path, mode, dev) < 0 && errno != EEXIST) {
        slogf("ishsv: mknod(%s) failed: %s\n", path, strerror(errno));
    }
}

/* Ensure /dev nodes + devpts mount inside a chroot directory tree.
 * Idempotent. Called immediately before spawning a TTY child whose
 * chroot_path is something we haven't seen before. */
static void ensure_vm_devices(const char *chroot_path) {
    if (already_ensured(chroot_path)) return;
    char p[PATH_MAX];

    /* /dev itself + /dev/pts. */
    snprintf(p, sizeof(p), "%s/dev",     chroot_path); mkdir_p(p, 0755);
    snprintf(p, sizeof(p), "%s/dev/pts", chroot_path); mkdir_p(p, 0755);

    /* Standard char devices. The major/minor here have to match what
     * iSH's tty driver registers; mismatched majors mean open() of
     * the device returns ENXIO. */
    snprintf(p, sizeof(p), "%s/dev/null",    chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_MEM_MAJOR, 3));
    snprintf(p, sizeof(p), "%s/dev/zero",    chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_MEM_MAJOR, 5));
    snprintf(p, sizeof(p), "%s/dev/full",    chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_MEM_MAJOR, 7));
    snprintf(p, sizeof(p), "%s/dev/random",  chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_MEM_MAJOR, 8));
    snprintf(p, sizeof(p), "%s/dev/urandom", chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_MEM_MAJOR, 9));
    snprintf(p, sizeof(p), "%s/dev/tty",     chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_TTY_MAJOR, 0));
    snprintf(p, sizeof(p), "%s/dev/console", chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_TTY_MAJOR, 1));
    snprintf(p, sizeof(p), "%s/dev/ptmx",    chroot_path);
    ensure_node(p, S_IFCHR | 0666, makedev(ISH_DEV_TTY_MAJOR, 2));

    /* Mount devpts so /dev/pts/N appears on demand when posix_openpt
     * allocates. iSH's devptsfs is purely synthetic. The mount happens
     * outside any chroot (we are PID 1, the supervisor, in the real
     * fs root), so the V7 chroot-deny patch in sys_mount lets us
     * through. EBUSY = already mounted = fine. */
    snprintf(p, sizeof(p), "%s/dev/pts", chroot_path);
    if (mount("devpts", p, "devpts", 0, NULL) < 0 && errno != EBUSY) {
        slogf("ishsv: mount devpts at %s failed: %s\n", p, strerror(errno));
    }

    /* Mount procfs so /proc/self/exe and friends work. Many modern
     * tools (codex, glibc PATH detection, busybox top, …) read
     * /proc/self/exe and silently degrade or refuse to start when it
     * isn't there. Like devpts, procfs is synthetic in iSH and the
     * supervisor is outside any chroot, so this just works. */
    snprintf(p, sizeof(p), "%s/proc", chroot_path);
    mkdir_p(p, 0755);
    if (mount("proc", p, "proc", 0, NULL) < 0 && errno != EBUSY) {
        slogf("ishsv: mount procfs at %s failed: %s\n", p, strerror(errno));
    }

    mark_ensured(chroot_path);
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

    if (want_tty) {
        /* If we're spawning into a chrooted VM, make sure that VM has
         * /dev/ptmx and /dev/pts mounted. Without this, the very first
         * shell in a freshly-created VM gets ENOENT on posix_openpt. */
        if (sa.chroot_path && sa.chroot_path[0] && sa.chroot_path[0] == '/') {
            ensure_vm_devices(sa.chroot_path);
        }
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
    }

    pid_t pid = fork();
    if (pid < 0) {
        emit_error(sid, errno, "fork() failed");
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
        if (want_tty) {
            /* Become session leader and acquire the slave as the
             * controlling tty. setsid() detaches from any inherited
             * session/tty; the first open(O_NOCTTY-clear) of a tty
             * after setsid attaches it as controlling. */
            setsid();
            /* Wire stdin/stdout/stderr to the slave. Keep the slave fd
             * itself around long enough to TIOCSCTTY. */
            dup2(pty_slave, 0);
            dup2(pty_slave, 1);
            dup2(pty_slave, 2);
#ifdef TIOCSCTTY
            ioctl(pty_slave, TIOCSCTTY, 0);
#endif
            close(pty_master);
            if (pty_slave > 2) close(pty_slave);
            for (int fd = 3; fd < 256; fd++) close(fd);
            goto child_post_io;
        }

        setpgid(0, 0);

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
    if (want_tty) {
        close(pty_slave);
        set_cloexec(pty_master);
        set_nonblock(pty_master);
        s->pid       = pid;
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

static void reap_children(void) {
    while (1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) return;
        for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
            struct session *s = &g_sessions[i];
            if (!s->in_use) continue;
            if (s->pid == pid) {
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
                break;
            }
        }
    }
}

static void drain_session_streams_and_handle_exit(void) {
    /* Drain all readable child fds and emit STDOUT/STDERR events; if a
     * session is reaped AND fully drained, emit EXITED and free it. */
    struct pollfd pfds[SUPERVISOR_MAX_SESSIONS * 2];
    int    pmap[SUPERVISOR_MAX_SESSIONS * 2][2]; /* [sess_idx][stream_kind] */
    int n = 0;

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
                while (1) {
                    ssize_t r = read(fd, buf + 8, SUPERVISOR_PIPE_BUF);
                    if (r > 0) {
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
                            if (s->is_tty && s->stdin_fd == s->stdout_fd) s->stdin_fd = -1;
                            close(s->stdout_fd); s->stdout_fd = -1;
                        } else {
                            close(s->stderr_fd); s->stderr_fd = -1;
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        /* hard error: drop the fd */
                        if (kind == ISH_STREAM_STDOUT) {
                            if (s->is_tty && s->stdin_fd == s->stdout_fd) s->stdin_fd = -1;
                            close(s->stdout_fd); s->stdout_fd = -1;
                        } else {
                            close(s->stderr_fd); s->stderr_fd = -1;
                        }
                        break;
                    }
                }
            }
        }
    }

    /* Sessions where child reaped AND both streams EOF -> emit EXITED */
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
}

static void apply_term_grace(void) {
    /* Each main-loop tick (~50ms), if a session was TERMINATEd, after
     * ~30 ticks (~1.5s) escalate to SIGKILL. */
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        if (s->term_grace_ticks > 0) {
            s->term_grace_ticks--;
            if (s->term_grace_ticks == 0 && !s->reaped) {
                kill(-s->pid, SIGKILL);
            }
        }
    }
}

static void shutdown_all(void) {
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        kill(-s->pid, SIGTERM);
    }
    /* drain for ~2s */
    for (int t = 0; t < 40; t++) {
        reap_children();
        drain_session_streams_and_handle_exit();
        int active = 0;
        for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) if (g_sessions[i].in_use) active++;
        if (active == 0) break;
        usleep(50000);
    }
    /* anything remaining: SIGKILL */
    for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) {
        struct session *s = &g_sessions[i];
        if (!s->in_use) continue;
        kill(-s->pid, SIGKILL);
    }
    for (int t = 0; t < 40; t++) {
        reap_children();
        drain_session_streams_and_handle_exit();
        int active = 0;
        for (size_t i = 0; i < SUPERVISOR_MAX_SESSIONS; i++) if (g_sessions[i].in_use) active++;
        if (active == 0) break;
        usleep(50000);
    }
}

static int handshake(void) {
    /* Read HELLO frame, respond with HELLO_ACK. */
    uint8_t *p = NULL; uint32_t plen = 0; uint8_t type = 0, flags = 0; uint32_t sid = 0;
    int r = read_control_frame(&p, &plen, &type, &flags, &sid);
    if (r != 0 || type != ISH_FT_HELLO) {
        free(p);
        slogf("ishsv: bad handshake (r=%d type=0x%x)\n", r, type);
        return -1;
    }
    free(p);
    uint8_t ack[12];
    ish_proto_put_u32(ack, 1);              /* abi_version */
    ack[4] = ISH_PROTO_VERSION;
    ack[5] = ack[6] = ack[7] = 0;
    ish_proto_put_u32(ack + 8, SUPERVISOR_MAX_SESSIONS);
    return emit_frame(ISH_FT_HELLO_ACK, 0, 0, ack, sizeof(ack));
}

int main(void) {
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

    set_nonblock(0);

    if (handshake() < 0) {
        slogf("ishsv: handshake failed\n");
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
                    if (s && s->stdin_fd >= 0 && plen > 0) {
                        ssize_t w = write(s->stdin_fd, body, plen);
                        (void)w; /* if EPIPE/EAGAIN just drop */
                    }
                    break;
                }
                case ISH_FT_STDIN_CLOSE: {
                    struct session *s = find_session(sid);
                    if (s && s->stdin_fd >= 0) {
                        close(s->stdin_fd); s->stdin_fd = -1;
                    }
                    break;
                }
                case ISH_FT_SIGNAL: {
                    if (plen >= 4) {
                        int signum = ish_proto_get_i32(body);
                        struct session *s = find_session(sid);
                        if (s && s->pid > 0) kill(-s->pid, signum);
                    }
                    break;
                }
                case ISH_FT_TERMINATE: {
                    struct session *s = find_session(sid);
                    if (s && s->pid > 0) {
                        kill(-s->pid, SIGTERM);
                        s->term_grace_ticks = 30; /* ~1.5s @ 50ms tick */
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
        }

        if (shutting_down) break;

        drain_session_streams_and_handle_exit();
        apply_term_grace();

        /* Sleep a bit, but wake on SIGCHLD or stdin readable */
        struct pollfd pf;
        pf.fd = 0; pf.events = POLLIN;
        poll(&pf, 1, 50);
    }

    shutdown_all();
    emit_frame(ISH_FT_SHUTDOWN_ACK, 0, 0, NULL, 0);
    return 0;
}

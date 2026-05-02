/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libishembed — host-side embedding glue.
 *
 * Built as a static library and linked into the host app together with
 * libish, libish_emu, libfakefs.
 *
 * Layers:
 *
 *   public API (ishembed.h)
 *        |
 *        v
 *   session router  ----->  reader pthread (parses framed protocol)
 *        |                          |
 *        |                          v
 *        |                   per-session inboxes (linked lists of frames)
 *        v
 *   writer (mutex'd) -----> host->guest pipe (control + stdin)
 *        |
 *        v
 *   ish_ffi_*  -------->  iSH kernel + dedicated kernel pthread
 *
 * One IshInstance per process; init holds a static singleton sentinel.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "ishembed.h"
#include "../protocol/proto.h"
#include "../ffi/ish_ffi.h"

/* --------------------------------------------------------------- *
 *  Internal types                                                 *
 * --------------------------------------------------------------- */

/* A frame parked on a session inbox until the host reads it. */
struct inbox_frame {
    struct inbox_frame *next;
    int       kind;          /* ISH_STREAM_STDOUT / STDERR / EXITED */
    uint64_t  seq;
    uint8_t  *data;          /* malloc'd for STDOUT/STDERR; NULL for EXITED */
    size_t    len;
    int32_t   exit_code;
    int32_t   signal;
};

struct ish_embed_session {
    ish_embed_instance_t *inst;
    uint32_t              id;
    int                   closed;     /* host called close_stdin                */
    int                   exited;     /* EXITED frame queued                    */
    pthread_mutex_t       lock;
    pthread_cond_t        cond;
    struct inbox_frame   *head;
    struct inbox_frame   *tail;
    /* doubly-linked into instance session list */
    struct ish_embed_session *prev;
    struct ish_embed_session *next;
};

struct ish_embed_instance {
    /* protocol pipes — host side fds */
    int  host_to_guest_w;   /* host writes commands & stdin           */
    int  guest_to_host_r;   /* host reads protocol events             */
    int  guest_log_r;       /* host reads supervisor log (stderr)     */
    int  kern_log_r;        /* host reads iSH kernel printk (host stderr) */

    /* the kernel-side ends; kernel takes ownership but we keep these
     * around for shutdown */
    int  guest_stdin_r;
    int  guest_stdout_w;
    int  guest_stderr_w;
    int  kern_log_w;        /* duped onto host STDERR_FILENO */
    int  saved_stderr;      /* original host stderr, restored on shutdown */

    pthread_mutex_t writer_lock;     /* serialize writes to control pipe */

    pthread_t       reader_thread;
    pthread_t       log_thread;
    pthread_t       kern_log_thread;
    int             reader_thread_alive;
    int             log_thread_alive;
    int             kern_log_thread_alive;

    pthread_mutex_t sess_lock;
    struct ish_embed_session *sessions_head;
    atomic_uint     next_session_id;

    atomic_int      shutting_down;
    atomic_int      kernel_exited;

    /* host fd for kernel/printk redirect; -1 = stderr */
    int             kernel_log_fd;

    /* hello handshake */
    int             hello_acked;
    pthread_mutex_t hello_lock;
    pthread_cond_t  hello_cond;
    uint32_t        max_concurrent;
};

static ish_embed_instance_t *g_instance = NULL;
static pthread_mutex_t       g_instance_lock = PTHREAD_MUTEX_INITIALIZER;

/* --------------------------------------------------------------- *
 *  utilities                                                      *
 * --------------------------------------------------------------- */

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
        if (r == 0) return -2;
        p += r; len -= (size_t)r;
    }
    return 0;
}

static int set_cloexec_local(int fd) {
    int f = fcntl(fd, F_GETFD, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

/* monotonic-ish ms */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

/* --------------------------------------------------------------- *
 *  send a frame to supervisor (writer-locked)                     *
 * --------------------------------------------------------------- */

static int send_frame(ish_embed_instance_t *inst,
                      uint8_t type, uint8_t flags, uint32_t sid,
                      const void *payload, uint32_t payload_len) {
    if (atomic_load(&inst->shutting_down) && type != ISH_FT_SHUTDOWN) {
        return ISH_ERR_NOT_RUNNING;
    }
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, type, flags, payload_len, sid);
    pthread_mutex_lock(&inst->writer_lock);
    int rc = 0;
    if (write_full(inst->host_to_guest_w, hdr, sizeof(hdr)) < 0) rc = ISH_ERR_BROKEN_PIPE;
    if (rc == 0 && payload_len > 0 &&
        write_full(inst->host_to_guest_w, payload, payload_len) < 0)
        rc = ISH_ERR_BROKEN_PIPE;
    pthread_mutex_unlock(&inst->writer_lock);
    return rc;
}

/* --------------------------------------------------------------- *
 *  session list management                                        *
 * --------------------------------------------------------------- */

static struct ish_embed_session *find_session_locked(ish_embed_instance_t *inst, uint32_t id) {
    for (struct ish_embed_session *s = inst->sessions_head; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

static void enqueue_frame(struct ish_embed_session *s, struct inbox_frame *f) {
    pthread_mutex_lock(&s->lock);
    f->next = NULL;
    if (s->tail) s->tail->next = f; else s->head = f;
    s->tail = f;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

/* called only after ALL queued frames have been dequeued by host */
static void session_destroy(struct ish_embed_session *s) {
    pthread_mutex_lock(&s->lock);
    while (s->head) {
        struct inbox_frame *f = s->head;
        s->head = f->next;
        free(f->data);
        free(f);
    }
    s->tail = NULL;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}

static void session_unlink(ish_embed_instance_t *inst, struct ish_embed_session *s) {
    pthread_mutex_lock(&inst->sess_lock);
    if (s->prev) s->prev->next = s->next; else inst->sessions_head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
    pthread_mutex_unlock(&inst->sess_lock);
}

/* --------------------------------------------------------------- *
 *  reader thread: parse framed protocol from supervisor stdout    *
 * --------------------------------------------------------------- */

static void *reader_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;
    while (!atomic_load(&inst->shutting_down)) {
        uint8_t hdr[ISH_PROTO_HDR_SIZE];
        int r = read_full(inst->guest_to_host_r, hdr, sizeof(hdr));
        if (r != 0) break;
        uint8_t type, flags;
        uint32_t plen, sid;
        if (ish_proto_parse_hdr(hdr, &type, &flags, &plen, &sid) < 0) {
            break;
        }
        uint8_t *body = NULL;
        if (plen > 0) {
            body = (uint8_t *)malloc(plen);
            if (!body) break;
            if (read_full(inst->guest_to_host_r, body, plen) != 0) { free(body); break; }
        }

        switch (type) {
            case ISH_FT_HELLO_ACK: {
                pthread_mutex_lock(&inst->hello_lock);
                inst->hello_acked = 1;
                if (plen >= 12) inst->max_concurrent = ish_proto_get_u32(body + 8);
                pthread_cond_broadcast(&inst->hello_cond);
                pthread_mutex_unlock(&inst->hello_lock);
                break;
            }
            case ISH_FT_SPAWNED:
                /* Informational; we don't surface guest_pid yet. */
                break;
            case ISH_FT_STDOUT_DATA:
            case ISH_FT_STDERR_DATA: {
                pthread_mutex_lock(&inst->sess_lock);
                struct ish_embed_session *s = find_session_locked(inst, sid);
                pthread_mutex_unlock(&inst->sess_lock);
                if (!s) break;
                struct inbox_frame *f = (struct inbox_frame *)calloc(1, sizeof(*f));
                if (!f) break;
                f->kind = (type == ISH_FT_STDOUT_DATA) ? ISH_STREAM_STDOUT : ISH_STREAM_STDERR;
                size_t off = 0;
                if (flags & ISH_FF_SEQ_PRESENT && plen >= 8) {
                    f->seq = ish_proto_get_u64(body);
                    off = 8;
                }
                f->len = plen - off;
                f->data = (uint8_t *)malloc(f->len > 0 ? f->len : 1);
                if (!f->data) { free(f); break; }
                if (f->len) memcpy(f->data, body + off, f->len);
                enqueue_frame(s, f);
                break;
            }
            case ISH_FT_EXITED: {
                pthread_mutex_lock(&inst->sess_lock);
                struct ish_embed_session *s = find_session_locked(inst, sid);
                pthread_mutex_unlock(&inst->sess_lock);
                if (!s) break;
                struct inbox_frame *f = (struct inbox_frame *)calloc(1, sizeof(*f));
                if (!f) break;
                f->kind = ISH_STREAM_EXITED;
                if (plen >= 8) {
                    f->exit_code = ish_proto_get_i32(body);
                    f->signal    = ish_proto_get_i32(body + 4);
                }
                pthread_mutex_lock(&s->lock);
                s->exited = 1;
                pthread_mutex_unlock(&s->lock);
                enqueue_frame(s, f);
                break;
            }
            case ISH_FT_ERROR: {
                /* deliver an EXITED frame with exit_code = -errno to
                 * unblock waiters, plus log to stderr for debug. */
                pthread_mutex_lock(&inst->sess_lock);
                struct ish_embed_session *s = find_session_locked(inst, sid);
                pthread_mutex_unlock(&inst->sess_lock);
                int32_t errv = (plen >= 4) ? ish_proto_get_i32(body) : -1;
                if (s) {
                    struct inbox_frame *f = (struct inbox_frame *)calloc(1, sizeof(*f));
                    if (f) {
                        f->kind = ISH_STREAM_EXITED;
                        f->exit_code = -errv;
                        f->signal = 0;
                        pthread_mutex_lock(&s->lock);
                        s->exited = 1;
                        pthread_mutex_unlock(&s->lock);
                        enqueue_frame(s, f);
                    }
                }
                break;
            }
            case ISH_FT_LOG: {
                /* surface to stderr for debug */
                if (plen) (void)write(2, body, plen);
                break;
            }
            case ISH_FT_SHUTDOWN_ACK:
                atomic_store(&inst->shutting_down, 1);
                break;
            case ISH_FT_PONG:
            default:
                break;
        }
        free(body);
    }

    /* signal any blocked readers that the world ended */
    atomic_store(&inst->shutting_down, 1);
    pthread_mutex_lock(&inst->sess_lock);
    for (struct ish_embed_session *s = inst->sessions_head; s; s = s->next) {
        struct inbox_frame *f = (struct inbox_frame *)calloc(1, sizeof(*f));
        if (!f) continue;
        f->kind = ISH_STREAM_EXITED;
        f->exit_code = -ISH_ERR_BROKEN_PIPE;
        enqueue_frame(s, f);
    }
    pthread_mutex_unlock(&inst->sess_lock);
    return NULL;
}

/* --------------------------------------------------------------- *
 *  log thread (supervisor stderr)                                 *
 * --------------------------------------------------------------- */

static void *log_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;
    int dst = inst->kernel_log_fd >= 0 ? inst->kernel_log_fd
                                        : (inst->saved_stderr >= 0
                                           ? inst->saved_stderr : 2);
    uint8_t buf[4096];
    while (!atomic_load(&inst->shutting_down)) {
        ssize_t r = read(inst->guest_log_r, buf, sizeof(buf));
        if (r > 0) (void)write(dst, buf, (size_t)r);
        else if (r == 0) break;
        else if (errno != EINTR) break;
    }
    return NULL;
}

/// Pump iSH-kernel-side fprintf(stderr,...) messages (NETDIAG, panic
/// chatter, etc.) into the same destination as the supervisor log,
/// each line prefixed so the consumer can tell them apart.
static void *kern_log_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;
    int dst = inst->kernel_log_fd >= 0 ? inst->kernel_log_fd
                                        : (inst->saved_stderr >= 0
                                           ? inst->saved_stderr : 2);
    uint8_t buf[4096];
    while (!atomic_load(&inst->shutting_down)) {
        ssize_t r = read(inst->kern_log_r, buf, sizeof(buf));
        if (r > 0) (void)write(dst, buf, (size_t)r);
        else if (r == 0) break;
        else if (errno != EINTR) break;
    }
    return NULL;
}

/* --------------------------------------------------------------- *
 *  kernel pthread exit hook                                       *
 * --------------------------------------------------------------- */

static void on_kernel_exit(int code, void *ctx) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)ctx;
    (void)code;
    atomic_store(&inst->kernel_exited, 1);
    atomic_store(&inst->shutting_down, 1);
    /* close guest-side write end so reader sees EOF */
    if (inst->guest_stdout_w >= 0) close(inst->guest_stdout_w);
    if (inst->guest_stderr_w >= 0) close(inst->guest_stderr_w);
    inst->guest_stdout_w = inst->guest_stderr_w = -1;
}

/* --------------------------------------------------------------- *
 *  boot                                                           *
 * --------------------------------------------------------------- */

/* Default pre-injected supervisor binary path inside the rootfs. */
#define ISH_DEFAULT_SUPERVISOR_PATH "/sbin/ishsv"

int ish_embed_boot(const ish_embed_boot_opts_t *opts,
                   ish_embed_instance_t **out_instance) {
    if (!opts || !opts->rootfs_path) return ISH_ERR_INVALID_ARG;
    if (out_instance) *out_instance = NULL;

    pthread_mutex_lock(&g_instance_lock);
    if (g_instance) {
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_ALREADY_BOOTED;
    }

    ish_embed_instance_t *inst = (ish_embed_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) { pthread_mutex_unlock(&g_instance_lock); return ISH_ERR_OOM; }
    inst->host_to_guest_w = -1;
    inst->guest_to_host_r = -1;
    inst->guest_log_r     = -1;
    inst->kern_log_r      = -1;
    inst->guest_stdin_r   = -1;
    inst->guest_stdout_w  = -1;
    inst->guest_stderr_w  = -1;
    inst->kern_log_w      = -1;
    inst->saved_stderr    = -1;
    inst->kernel_log_fd   = opts->kernel_log_fd;
    pthread_mutex_init(&inst->writer_lock, NULL);
    pthread_mutex_init(&inst->sess_lock, NULL);
    pthread_mutex_init(&inst->hello_lock, NULL);
    pthread_cond_init(&inst->hello_cond, NULL);
    atomic_store(&inst->next_session_id, 1);
    atomic_store(&inst->shutting_down, 0);
    atomic_store(&inst->kernel_exited, 0);

    /* create four pipes:
     *   p_in:  host writes to host_to_guest_w; guest reads from guest_stdin_r
     *   p_out: guest writes to guest_stdout_w; host reads from guest_to_host_r
     *   p_log: guest writes to guest_stderr_w; host reads from guest_log_r
     *          (this is the SUPERVISOR's stderr — diagnostic logs from
     *           PID 1 inside iSH)
     *   p_kln: dup2'd onto host's STDERR_FILENO; host reads from kern_log_r
     *          (this captures iSH KERNEL printk — fprintf(stderr,...) from
     *           inside libish.a, including NETDIAG and panic messages)
     */
    int p_in[2], p_out[2], p_log[2], p_kln[2];
    if (pipe(p_in) < 0 || pipe(p_out) < 0 ||
        pipe(p_log) < 0 || pipe(p_kln) < 0) {
        free(inst);
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_PIPE;
    }
    inst->guest_stdin_r   = p_in[0];
    inst->host_to_guest_w = p_in[1];
    inst->guest_to_host_r = p_out[0];
    inst->guest_stdout_w  = p_out[1];
    inst->guest_log_r     = p_log[0];
    inst->guest_stderr_w  = p_log[1];
    inst->kern_log_r      = p_kln[0];
    inst->kern_log_w      = p_kln[1];

    set_cloexec_local(inst->host_to_guest_w);
    set_cloexec_local(inst->guest_to_host_r);
    set_cloexec_local(inst->guest_log_r);
    set_cloexec_local(inst->kern_log_r);
    /* guest_*_r/w are handed to the kernel and become guest-side fds;
     * we keep them in the host process but the kernel "owns" them. */

    /* Redirect host stderr to our kernel-log pipe BEFORE the kernel
     * pthread starts, so iSH's fprintf(stderr, …) printk hits us. We
     * keep the original stderr around to restore at shutdown. */
    inst->saved_stderr = dup(STDERR_FILENO);
    if (inst->saved_stderr >= 0) set_cloexec_local(inst->saved_stderr);
    if (dup2(inst->kern_log_w, STDERR_FILENO) < 0) {
        /* not fatal: kernel log just won't reach us */
    }

    /* ---- iSH kernel boot sequence (mirrors xX_main_Xx) ---- */
    int err;
    if ((err = ish_ffi_mount_fakefs(opts->rootfs_path)) < 0) {
        goto fail; /* err is already negative; map below */
    }
    if ((err = ish_ffi_become_init()) < 0) goto fail;
    ish_ffi_create_devices();

    if ((err = ish_ffi_install_pipe_stdio(inst->guest_stdin_r,
                                          inst->guest_stdout_w,
                                          inst->guest_stderr_w)) < 0)
        goto fail;

    const char *workdir = opts->workdir ? opts->workdir : "/";
    if ((err = ish_ffi_chdir(workdir)) < 0) {
        /* fall back to / */
        ish_ffi_chdir("/");
    }

    /* register exit hook BEFORE task_start so we don't race */
    ish_ffi_register_exit_hook(on_kernel_exit, inst);

    const char *sup = opts->supervisor_guest_path ? opts->supervisor_guest_path
                                                  : ISH_DEFAULT_SUPERVISOR_PATH;

    /* Build packed argv: just argv[0]=sup, NUL term + outer NUL */
    char argv_packed[256];
    size_t off = 0;
    size_t L = strlen(sup);
    if (L + 2 > sizeof(argv_packed)) { err = ISH_ERR_INVALID_ARG; goto fail; }
    memcpy(argv_packed, sup, L); argv_packed[L] = 0; off = L + 1;
    argv_packed[off] = 0;

    char envp_packed[256];
    size_t eo = 0;
    const char *env0 = "PATH=/sbin:/usr/sbin:/usr/local/sbin:/bin:/usr/bin:/usr/local/bin";
    size_t e0 = strlen(env0);
    if (e0 + 2 > sizeof(envp_packed)) { err = ISH_ERR_INVALID_ARG; goto fail; }
    memcpy(envp_packed, env0, e0); envp_packed[e0] = 0; eo = e0 + 1;
    envp_packed[eo] = 0;

    if ((err = ish_ffi_execve(sup, 1, argv_packed, envp_packed)) < 0) {
        err = ISH_ERR_EXECVE;
        goto fail;
    }

    /* Start kernel pthread BEFORE the reader, so the supervisor exists
     * to answer our handshake. */
    if (ish_ffi_task_start() < 0) { err = ISH_ERR_THREAD; goto fail; }

    /* Spawn reader & log threads */
    if (pthread_create(&inst->reader_thread, NULL, reader_thread_main, inst) != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }
    inst->reader_thread_alive = 1;
    if (pthread_create(&inst->log_thread, NULL, log_thread_main, inst) != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }
    inst->log_thread_alive = 1;
    if (pthread_create(&inst->kern_log_thread, NULL, kern_log_thread_main, inst) != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }
    inst->kern_log_thread_alive = 1;

    /* Send HELLO and wait for HELLO_ACK with a timeout (5s). */
    {
        uint8_t hello[12 + 7];
        ish_proto_put_u32(hello, ISH_EMBED_ABI_VERSION);
        hello[4] = ISH_PROTO_VERSION;
        hello[5] = hello[6] = hello[7] = 0;
        const char *g = "ishemb1";
        ish_proto_put_u32(hello + 8, (uint32_t)strlen(g));
        memcpy(hello + 12, g, strlen(g));
        if (send_frame(inst, ISH_FT_HELLO, 0, 0, hello, 12 + (uint32_t)strlen(g)) != 0) {
            err = ISH_ERR_BROKEN_PIPE; goto fail;
        }
    }

    {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;
        pthread_mutex_lock(&inst->hello_lock);
        while (!inst->hello_acked && !atomic_load(&inst->shutting_down)) {
            int rc = pthread_cond_timedwait(&inst->hello_cond, &inst->hello_lock, &deadline);
            if (rc == ETIMEDOUT) break;
        }
        int ok = inst->hello_acked;
        pthread_mutex_unlock(&inst->hello_lock);
        if (!ok) { err = ISH_ERR_TIMEOUT; goto fail; }
    }

    g_instance = inst;
    if (out_instance) *out_instance = inst;
    pthread_mutex_unlock(&g_instance_lock);
    return ISH_OK;

fail:;
    int saved = err;
    /* Best-effort cleanup. We deliberately do not close kernel-side fds
     * if the kernel might already own them. */
    if (inst->host_to_guest_w >= 0) close(inst->host_to_guest_w);
    if (inst->guest_to_host_r >= 0) close(inst->guest_to_host_r);
    if (inst->guest_log_r     >= 0) close(inst->guest_log_r);
    free(inst);
    pthread_mutex_unlock(&g_instance_lock);
    if (saved <= 0 && saved >= -200) {
        /* It's already an iSH errno or our negative status code; pass through. */
        return saved;
    }
    return ISH_ERR_BOOT;
}

/* --------------------------------------------------------------- *
 *  spawn / sessions                                               *
 * --------------------------------------------------------------- */

/* serialize argv/envp/chroot into SPAWN payload. argv must not be NULL. */
static int build_spawn_payload(const char *cwd,
                               const char *const *argv,
                               const char *const *envp,
                               const char *chroot_path,
                               uint8_t **out_buf, uint32_t *out_len) {
    size_t cap = 64;
    size_t cwd_len = cwd ? strlen(cwd) : 0;
    size_t chroot_len = chroot_path ? strlen(chroot_path) : 0;
    size_t argc = 0;
    if (argv) for (; argv[argc]; argc++) ;
    size_t envc = 0;
    if (envp) for (; envp[envc]; envc++) ;
    cap += cwd_len + chroot_len;
    for (size_t i = 0; i < argc; i++) cap += strlen(argv[i]) + 8;
    for (size_t i = 0; i < envc; i++) cap += strlen(envp[i]) + 8;

    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return ISH_ERR_OOM;
    size_t off = 0;
    ish_proto_put_u32(buf + off, (uint32_t)cwd_len); off += 4;
    if (cwd_len) { memcpy(buf + off, cwd, cwd_len); off += cwd_len; }
    ish_proto_put_u32(buf + off, (uint32_t)argc); off += 4;
    for (size_t i = 0; i < argc; i++) {
        size_t L = strlen(argv[i]);
        ish_proto_put_u32(buf + off, (uint32_t)L); off += 4;
        memcpy(buf + off, argv[i], L); off += L;
    }
    ish_proto_put_u32(buf + off, (uint32_t)envc); off += 4;
    for (size_t i = 0; i < envc; i++) {
        size_t L = strlen(envp[i]);
        ish_proto_put_u32(buf + off, (uint32_t)L); off += 4;
        memcpy(buf + off, envp[i], L); off += L;
    }
    ish_proto_put_u32(buf + off, (uint32_t)chroot_len); off += 4;
    if (chroot_len) { memcpy(buf + off, chroot_path, chroot_len); off += chroot_len; }
    *out_buf = buf;
    *out_len = (uint32_t)off;
    return ISH_OK;
}

int ish_embed_spawn(ish_embed_instance_t *inst,
                    const ish_embed_spawn_opts_t *opts,
                    ish_embed_session_t **out_session) {
    if (!inst || !opts || !out_session) return ISH_ERR_INVALID_ARG;
    if (!opts->argv || !opts->argv[0]) return ISH_ERR_INVALID_ARG;
    if (atomic_load(&inst->shutting_down)) return ISH_ERR_NOT_RUNNING;
    *out_session = NULL;

    uint32_t sid = atomic_fetch_add(&inst->next_session_id, 1);

    struct ish_embed_session *s = (struct ish_embed_session *)calloc(1, sizeof(*s));
    if (!s) return ISH_ERR_OOM;
    s->inst = inst;
    s->id   = sid;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    /* link */
    pthread_mutex_lock(&inst->sess_lock);
    s->next = inst->sessions_head;
    if (inst->sessions_head) inst->sessions_head->prev = s;
    inst->sessions_head = s;
    pthread_mutex_unlock(&inst->sess_lock);

    uint8_t *payload = NULL;
    uint32_t plen = 0;
    int rc = build_spawn_payload(opts->cwd, opts->argv, opts->envp,
                                 opts->chroot_path, &payload, &plen);
    if (rc != 0) {
        session_unlink(inst, s);
        session_destroy(s);
        return rc;
    }
    uint8_t flags = 0;
    if (opts->allocate_tty)              flags |= ISH_FF_TTY;
    if (opts->merge_stderr_into_stdout)  flags |= ISH_FF_MERGE_STDERR;
    rc = send_frame(inst, ISH_FT_SPAWN, flags, sid, payload, plen);
    free(payload);
    if (rc != 0) {
        session_unlink(inst, s);
        session_destroy(s);
        return rc;
    }
    *out_session = s;
    return ISH_OK;
}

int ish_embed_session_read(ish_embed_session_t *s,
                           uint32_t wait_ms,
                           uint8_t **out_buf, size_t *out_len,
                           int *out_kind, uint64_t *out_seq,
                           int32_t *out_exit_code, int32_t *out_signal) {
    if (!s) return ISH_ERR_INVALID_ARG;
    if (out_buf) *out_buf = NULL;
    if (out_len) *out_len = 0;
    if (out_kind) *out_kind = 0;
    if (out_seq) *out_seq = 0;
    if (out_exit_code) *out_exit_code = 0;
    if (out_signal) *out_signal = 0;

    pthread_mutex_lock(&s->lock);

    /* wait for a frame */
    if (!s->head) {
        if (wait_ms == 0) {
            pthread_mutex_unlock(&s->lock);
            return ISH_ERR_TIMEOUT;
        }
        if (wait_ms == UINT32_MAX) {
            while (!s->head) pthread_cond_wait(&s->cond, &s->lock);
        } else {
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_sec  += wait_ms / 1000;
            dl.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (dl.tv_nsec >= 1000000000L) { dl.tv_nsec -= 1000000000L; dl.tv_sec += 1; }
            while (!s->head) {
                int rc = pthread_cond_timedwait(&s->cond, &s->lock, &dl);
                if (rc == ETIMEDOUT) {
                    pthread_mutex_unlock(&s->lock);
                    return ISH_ERR_TIMEOUT;
                }
            }
        }
    }

    struct inbox_frame *f = s->head;
    s->head = f->next;
    if (!s->head) s->tail = NULL;
    pthread_mutex_unlock(&s->lock);

    if (out_kind) *out_kind = f->kind;
    if (out_seq)  *out_seq  = f->seq;
    if (f->kind == ISH_STREAM_EXITED) {
        if (out_exit_code) *out_exit_code = f->exit_code;
        if (out_signal)    *out_signal    = f->signal;
    } else {
        if (out_buf && f->len > 0) { *out_buf = f->data; f->data = NULL; }
        if (out_len) *out_len = f->len;
    }
    free(f->data);
    free(f);
    return ISH_OK;
}

int ish_embed_session_write(ish_embed_session_t *s,
                            const uint8_t *buf, size_t len) {
    if (!s) return ISH_ERR_INVALID_ARG;
    if (len == 0) return ISH_OK;
    /* split into 64KiB chunks to keep frames bounded */
    while (len > 0) {
        size_t chunk = len > 65536 ? 65536 : len;
        int rc = send_frame(s->inst, ISH_FT_STDIN_DATA, 0, s->id, buf, (uint32_t)chunk);
        if (rc != 0) return rc;
        buf += chunk; len -= chunk;
    }
    return ISH_OK;
}

int ish_embed_session_signal(ish_embed_session_t *s, int signum) {
    if (!s) return ISH_ERR_INVALID_ARG;
    uint8_t buf[4];
    ish_proto_put_i32(buf, signum);
    return send_frame(s->inst, ISH_FT_SIGNAL, 0, s->id, buf, sizeof(buf));
}

int ish_embed_session_terminate(ish_embed_session_t *s, uint32_t grace_ms) {
    if (!s) return ISH_ERR_INVALID_ARG;
    (void)grace_ms; /* supervisor uses its own ~1.5s grace */
    return send_frame(s->inst, ISH_FT_TERMINATE, 0, s->id, NULL, 0);
}

int ish_embed_session_close_stdin(ish_embed_session_t *s) {
    if (!s) return ISH_ERR_INVALID_ARG;
    pthread_mutex_lock(&s->lock);
    int already = s->closed;
    s->closed = 1;
    pthread_mutex_unlock(&s->lock);
    if (already) return ISH_OK;
    return send_frame(s->inst, ISH_FT_STDIN_CLOSE, 0, s->id, NULL, 0);
}

void ish_embed_session_close(ish_embed_session_t *s) {
    if (!s) return;
    /* If still alive on the guest side, send SIGKILL and wait briefly. */
    pthread_mutex_lock(&s->lock);
    int exited = s->exited;
    pthread_mutex_unlock(&s->lock);
    if (!exited) {
        uint8_t buf[4]; ish_proto_put_i32(buf, 9 /*SIGKILL*/);
        send_frame(s->inst, ISH_FT_SIGNAL, 0, s->id, buf, sizeof(buf));
        /* drain up to 1s waiting for EXITED */
        uint64_t until = now_ms() + 1000;
        while (now_ms() < until) {
            pthread_mutex_lock(&s->lock);
            int e = s->exited;
            pthread_mutex_unlock(&s->lock);
            if (e) break;
            uint8_t *b; size_t L; int k; uint64_t seq; int32_t xc, sig;
            int rc = ish_embed_session_read(s, 100, &b, &L, &k, &seq, &xc, &sig);
            (void)rc; ish_embed_free(b);
            if (k == ISH_STREAM_EXITED) break;
        }
    }
    session_unlink(s->inst, s);
    session_destroy(s);
}

/* --------------------------------------------------------------- *
 *  run_oneshot                                                    *
 * --------------------------------------------------------------- */

int ish_embed_run_oneshot(ish_embed_instance_t *inst,
                          const ish_embed_spawn_opts_t *opts,
                          ish_embed_oneshot_result_t *out) {
    if (!inst || !opts || !out) return ISH_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    ish_embed_session_t *s = NULL;
    int rc = ish_embed_spawn(inst, opts, &s);
    if (rc != 0) return rc;

    /* close stdin immediately for oneshot (no input) */
    ish_embed_session_close_stdin(s);

    size_t out_cap = 0, err_cap = 0;
    uint8_t *out_buf = NULL, *err_buf = NULL;
    size_t out_used = 0, err_used = 0;

    uint64_t deadline = (opts->timeout_ms > 0) ? now_ms() + opts->timeout_ms : 0;
    int timed_out = 0, terminated = 0;
    int32_t exit_code = -1, signal_v = 0;

    while (1) {
        uint32_t wait;
        if (deadline) {
            uint64_t now = now_ms();
            if (now >= deadline) {
                if (!terminated) {
                    ish_embed_session_terminate(s, 0);
                    terminated = 1;
                    deadline = now + 2000; /* +2s grace before SIGKILL */
                } else {
                    timed_out = 1;
                    /* force-kill */
                    ish_embed_session_signal(s, 9);
                }
                wait = 500;
            } else {
                uint64_t left = deadline - now;
                wait = (left > 1000) ? 1000 : (uint32_t)left;
            }
        } else {
            wait = UINT32_MAX;
        }
        uint8_t *b = NULL; size_t L = 0; int k = 0; uint64_t seq = 0;
        int32_t xc = 0, sg = 0;
        rc = ish_embed_session_read(s, wait, &b, &L, &k, &seq, &xc, &sg);
        if (rc == ISH_ERR_TIMEOUT) continue;
        if (rc != ISH_OK) break;
        if (k == ISH_STREAM_STDOUT) {
            if (out_used + L > out_cap) {
                size_t nc = out_cap ? out_cap * 2 : 4096;
                while (nc < out_used + L) nc *= 2;
                uint8_t *nb = (uint8_t *)realloc(out_buf, nc);
                if (!nb) { ish_embed_free(b); break; }
                out_buf = nb; out_cap = nc;
            }
            if (L) memcpy(out_buf + out_used, b, L);
            out_used += L;
            ish_embed_free(b);
        } else if (k == ISH_STREAM_STDERR) {
            if (err_used + L > err_cap) {
                size_t nc = err_cap ? err_cap * 2 : 4096;
                while (nc < err_used + L) nc *= 2;
                uint8_t *nb = (uint8_t *)realloc(err_buf, nc);
                if (!nb) { ish_embed_free(b); break; }
                err_buf = nb; err_cap = nc;
            }
            if (L) memcpy(err_buf + err_used, b, L);
            err_used += L;
            ish_embed_free(b);
        } else if (k == ISH_STREAM_EXITED) {
            exit_code = xc;
            signal_v = sg;
            break;
        }
    }

    out->exit_code   = exit_code;
    out->signal      = signal_v;
    out->stdout_buf  = out_buf;
    out->stdout_len  = out_used;
    out->stderr_buf  = err_buf;
    out->stderr_len  = err_used;
    out->timed_out   = timed_out;
    ish_embed_session_close(s);
    return ISH_OK;
}

void ish_embed_free(void *p) { free(p); }

/* --------------------------------------------------------------- *
 *  shutdown                                                       *
 * --------------------------------------------------------------- */

int ish_embed_shutdown(ish_embed_instance_t *inst, uint32_t grace_ms) {
    if (!inst) return ISH_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_instance_lock);
    if (g_instance != inst) { pthread_mutex_unlock(&g_instance_lock); return ISH_ERR_INVALID_ARG; }
    pthread_mutex_unlock(&g_instance_lock);

    if (!atomic_load(&inst->shutting_down)) {
        send_frame(inst, ISH_FT_SHUTDOWN, 0, 0, NULL, 0);
    }

    uint64_t deadline = now_ms() + (grace_ms ? grace_ms : 5000);
    while (!atomic_load(&inst->kernel_exited) && now_ms() < deadline) {
        usleep(50 * 1000);
    }
    atomic_store(&inst->shutting_down, 1);
    if (inst->host_to_guest_w >= 0) { close(inst->host_to_guest_w); inst->host_to_guest_w = -1; }

    if (inst->reader_thread_alive) pthread_join(inst->reader_thread, NULL);
    if (inst->log_thread_alive)    pthread_join(inst->log_thread, NULL);

    /* Close the host-stderr-redirect pipe write end and join the kern
     * log pump so the read end will see EOF. Then restore the original
     * host stderr so anyone calling fprintf(stderr, ...) post-shutdown
     * (e.g. system frameworks) doesn't write into a closed pipe. */
    if (inst->saved_stderr >= 0) {
        dup2(inst->saved_stderr, STDERR_FILENO);
        close(inst->saved_stderr);
        inst->saved_stderr = -1;
    }
    if (inst->kern_log_w >= 0) { close(inst->kern_log_w); inst->kern_log_w = -1; }
    if (inst->kern_log_thread_alive) pthread_join(inst->kern_log_thread, NULL);
    if (inst->kern_log_r >= 0) { close(inst->kern_log_r); inst->kern_log_r = -1; }

    if (inst->guest_to_host_r >= 0) close(inst->guest_to_host_r);
    if (inst->guest_log_r     >= 0) close(inst->guest_log_r);

    /* free remaining sessions */
    pthread_mutex_lock(&inst->sess_lock);
    while (inst->sessions_head) {
        struct ish_embed_session *s = inst->sessions_head;
        inst->sessions_head = s->next;
        if (s->next) s->next->prev = NULL;
        s->prev = s->next = NULL;
        pthread_mutex_unlock(&inst->sess_lock);
        session_destroy(s);
        pthread_mutex_lock(&inst->sess_lock);
    }
    pthread_mutex_unlock(&inst->sess_lock);

    pthread_mutex_destroy(&inst->writer_lock);
    pthread_mutex_destroy(&inst->sess_lock);
    pthread_mutex_destroy(&inst->hello_lock);
    pthread_cond_destroy(&inst->hello_cond);

    pthread_mutex_lock(&g_instance_lock);
    if (g_instance == inst) g_instance = NULL;
    pthread_mutex_unlock(&g_instance_lock);
    free(inst);
    return ISH_OK;
}

/* --------------------------------------------------------------- *
 *  strerror                                                       *
 * --------------------------------------------------------------- */

const char *ish_embed_strerror(int s) {
    switch (s) {
        case ISH_OK: return "ok";
        case ISH_ERR_BOOT: return "boot failed";
        case ISH_ERR_MOUNT: return "fakefs mount failed";
        case ISH_ERR_BECOME_INIT: return "become_first_process failed";
        case ISH_ERR_STDIO: return "stdio install failed";
        case ISH_ERR_CHDIR: return "chdir failed";
        case ISH_ERR_EXECVE: return "supervisor execve failed";
        case ISH_ERR_PIPE: return "pipe creation failed";
        case ISH_ERR_THREAD: return "pthread_create failed";
        case ISH_ERR_NOT_RUNNING: return "instance not running";
        case ISH_ERR_ALREADY_BOOTED: return "instance already booted";
        case ISH_ERR_PROTOCOL: return "protocol error";
        case ISH_ERR_TIMEOUT: return "timed out";
        case ISH_ERR_INVALID_ARG: return "invalid argument";
        case ISH_ERR_NO_SESSION: return "no such session";
        case ISH_ERR_SUPERVISOR: return "supervisor error";
        case ISH_ERR_OOM: return "out of memory";
        case ISH_ERR_BROKEN_PIPE: return "broken pipe to supervisor";
        case ISH_ERR_INTERNAL: return "internal error";
        default: return "unknown";
    }
}

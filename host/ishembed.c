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
#include <limits.h>
#include <poll.h>
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
#include "ishembed_sha256.h"
#include "../protocol/proto.h"
#include "../ffi/ish_ffi.h"

_Static_assert(ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES >=
                   ISH_PROTO_HDR_SIZE + sizeof(uint32_t),
               "critical control reserve must fit a SIGNAL frame");
_Static_assert(ISH_EMBED_MIN_CONTROL_CRITICAL_RESERVE_BYTES >=
                   ISH_PROTO_HDR_SIZE + sizeof(uint32_t),
               "public lifecycle frame lower bound is stale");

#ifdef ISH_EMBED_BUNDLED_SUPERVISOR
extern const uint8_t ish_embed_bundled_supervisor[];
extern const size_t ish_embed_bundled_supervisor_len;
extern const char ish_embed_bundled_supervisor_sha256[];
extern const char ish_embed_bundled_supervisor_guest_path[];
#endif

#ifndef ISH_EMBED_HELLO_TIMEOUT_MS
#define ISH_EMBED_HELLO_TIMEOUT_MS 5000u
#endif

/* Diagnostic output must never backpressure the guest. The reader copies
 * complete chunks into this fixed-size ring and drops new chunks when it is
 * full; only the sink writer is allowed to wait on the caller-provided fd. */
#ifndef ISH_EMBED_LOG_QUEUE_SLOTS
#define ISH_EMBED_LOG_QUEUE_SLOTS 64u
#endif
#ifndef ISH_EMBED_LOG_CHUNK_BYTES
#define ISH_EMBED_LOG_CHUNK_BYTES 4096u
#endif

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

/* A complete host-to-guest protocol frame. A dedicated writer thread is the
 * only code that writes the control pipe, so a slow guest can never make the
 * event reader wait on a pipe write and deadlock both directions. */
struct outbound_frame {
    struct outbound_frame *next;
    size_t                  len;
    size_t                  written;
    int                     wait_for_completion;
    int                     done;
    int                     result;
    int                     accounted;
    pthread_cond_t          done_cond;
    uint8_t                 bytes[];
};

enum outbound_admission_class {
    OUTBOUND_ADMISSION_NORMAL = 0,
    OUTBOUND_ADMISSION_LIFECYCLE = 1,
};

struct ish_embed_session {
    ish_embed_instance_t *inst;
    uint32_t              id;
    /* A finite streaming spawn uses ordered asynchronous control admission.
     * Preserve its absolute admission deadline so stdin writes/EOF cannot
     * start a fresh wait after the original product budget expires. Terminate
     * remains a separately bounded lifecycle operation. Zero-timeout legacy
     * sessions retain synchronous delivery semantics. */
    uint64_t              streaming_deadline_ms;
    int                   stdin_closed;
    int                   closing;
    int                   guest_exited;
    int                   terminal_ready;
    int                   terminal_consumed;
    int                   terminal_status;
    int32_t               terminal_exit_code;
    int32_t               terminal_signal;
    int                   output_disabled;
    int                   overflow_kill_sent;
    size_t                queued_bytes;
    size_t                queued_frames;
    unsigned              references; /* one owner plus explicit borrows        */
    unsigned              active_controls;
    int                   owner_released;
    int                   linked;
    /* Serializes complete multi-frame stdin writes with stdin close/session
     * close without blocking the event reader on the general session lock. */
    pthread_mutex_t       stdin_lock;
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

    /* the kernel-side ends; kernel takes ownership but we keep these
     * around for shutdown */
    int  guest_stdin_r;
    int  guest_stdout_w;
    int  guest_stderr_w;

    pthread_mutex_t writer_lock;     /* protects writer queue and fd slot */
    pthread_cond_t  writer_cond;
    struct outbound_frame *writer_head;
    struct outbound_frame *writer_tail;
    size_t          writer_accounted_bytes;  /* queued plus current write */
    size_t          writer_accounted_frames; /* queued plus current write */
    pthread_t       writer_thread;
    int             writer_thread_alive;
    atomic_int      writer_stopping;
    atomic_int      writer_stop_status;

    pthread_t       reader_thread;
    pthread_t       log_thread;
    pthread_t       log_writer_thread;
    int             reader_thread_alive;
    int             log_thread_alive;
    int             log_writer_thread_alive;
    int             kernel_thread_alive;

    pthread_mutex_t log_lock;
    pthread_cond_t  log_cond;
    size_t          log_head;
    size_t          log_count;
    size_t          log_lengths[ISH_EMBED_LOG_QUEUE_SLOTS];
    uint8_t         log_chunks[ISH_EMBED_LOG_QUEUE_SLOTS]
                              [ISH_EMBED_LOG_CHUNK_BYTES];
    atomic_int      log_stopping;

    pthread_mutex_t sess_lock;
    struct ish_embed_session *sessions_head;
    atomic_uint     next_session_id;
    /* Serializes the complete measure/build/send/free SPAWN path so at most
     * one maximum-sized staging payload exists outside the writer budget. */
    pthread_mutex_t spawn_lock;

    atomic_int      shutting_down;
    atomic_int      kernel_exited;
    pthread_mutex_t lifecycle_lock;
    pthread_cond_t  lifecycle_cond;
    unsigned        active_calls;

    /* Private duplicate of the requested supervisor-log sink. A negative fd
     * means logs are drained and discarded. */
    int             kernel_log_fd;
    /* Immutable guest path of the exact supervisor selected and executed at
     * boot. Filesystem helpers spawn this same verified binary rather than a
     * RootFS-owned compatibility path. */
    char           *supervisor_guest_path;

    /* hello handshake */
    int             hello_acked;
    int             hello_status;
    pthread_mutex_t hello_lock;
    pthread_cond_t  hello_cond;
    uint32_t        max_concurrent;
};

static ish_embed_instance_t *g_instance = NULL;
static pthread_mutex_t       g_instance_lock = PTHREAD_MUTEX_INITIALIZER;
static int                   g_boot_consumed = 0;

static uint64_t now_ms(void);
static int mutex_lock_until(pthread_mutex_t *lock, uint64_t deadline_ms);

/* Hold the singleton gate just long enough to prove the opaque instance is
 * still live, then account for the call under lifecycle_lock. shutdown holds
 * g_instance_lock while checking active_calls, so it cannot free an instance
 * between this check and the increment. */
static int instance_call_begin_until(ish_embed_instance_t *inst,
                                     uint64_t deadline_ms) {
    if (!inst) return ISH_ERR_INVALID_ARG;
    if (deadline_ms == 0) {
        pthread_mutex_lock(&g_instance_lock);
    } else {
        if (!mutex_lock_until(&g_instance_lock, deadline_ms))
            return ISH_ERR_TIMEOUT;
        if (now_ms() >= deadline_ms) {
            pthread_mutex_unlock(&g_instance_lock);
            return ISH_ERR_TIMEOUT;
        }
    }
    if (g_instance != inst) {
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_INVALID_ARG;
    }
    if (atomic_load(&inst->shutting_down)) {
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_NOT_RUNNING;
    }
    pthread_mutex_lock(&inst->lifecycle_lock);
    inst->active_calls++;
    pthread_mutex_unlock(&inst->lifecycle_lock);
    pthread_mutex_unlock(&g_instance_lock);
    return ISH_OK;
}

static int instance_call_begin(ish_embed_instance_t *inst) {
    return instance_call_begin_until(inst, 0);
}

static void instance_call_end(ish_embed_instance_t *inst) {
    pthread_mutex_lock(&inst->lifecycle_lock);
    if (inst->active_calls > 0) inst->active_calls--;
    pthread_cond_broadcast(&inst->lifecycle_cond);
    pthread_mutex_unlock(&inst->lifecycle_lock);
}

/* --------------------------------------------------------------- *
 *  utilities                                                      *
 * --------------------------------------------------------------- */

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

static int duplicate_cloexec_local(int fd) {
    int duplicate;
#ifdef F_DUPFD_CLOEXEC
    duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate >= 0) return duplicate;
    if (errno != EINVAL) return -1;
#endif
    duplicate = dup(fd);
    if (duplicate < 0) return -1;
    if (set_cloexec_local(duplicate) < 0) {
        close(duplicate);
        return -1;
    }
    return duplicate;
}

static int set_nonblocking_local(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

/* Darwin exposes a per-descriptor SIGPIPE suppression flag. The control pipe
 * is library-owned, so setting it cannot change caller-owned descriptor
 * semantics. This makes a dead PID 1 surface as EPIPE instead of terminating
 * the iOS host process. */
static int set_nosigpipe_local(int fd) {
#ifdef F_SETNOSIGPIPE
    return fcntl(fd, F_SETNOSIGPIPE, 1);
#else
    (void)fd;
    return 0;
#endif
}

/* monotonic-ish ms */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

/* pthread_mutex_timedlock is not uniformly available across the deployment
 * targets. A short trylock loop gives finite internal lifecycle operations an
 * absolute monotonic deadline without changing the mutex type or clock. */
static int mutex_lock_until(pthread_mutex_t *lock, uint64_t deadline_ms) {
    for (;;) {
        int rc = pthread_mutex_trylock(lock);
        if (rc == 0) return 1;
        if (rc != EBUSY || now_ms() >= deadline_ms) return 0;
        usleep(1000);
    }
}

static void close_fd_slot(int *fd);

static void enqueue_log_best_effort(ish_embed_instance_t *inst,
                                    const void *bytes, size_t len) {
    if (!inst || inst->kernel_log_fd < 0 || !bytes || len == 0 ||
        atomic_load(&inst->log_stopping))
        return;

    const uint8_t *src = (const uint8_t *)bytes;
    while (len > 0) {
        size_t chunk_len = len > ISH_EMBED_LOG_CHUNK_BYTES
            ? ISH_EMBED_LOG_CHUNK_BYTES : len;
        pthread_mutex_lock(&inst->log_lock);
        if (atomic_load(&inst->log_stopping) ||
            inst->log_count == ISH_EMBED_LOG_QUEUE_SLOTS) {
            pthread_mutex_unlock(&inst->log_lock);
            return;
        }
        size_t tail = (inst->log_head + inst->log_count) %
                      ISH_EMBED_LOG_QUEUE_SLOTS;
        memcpy(inst->log_chunks[tail], src, chunk_len);
        inst->log_lengths[tail] = chunk_len;
        inst->log_count++;
        pthread_cond_signal(&inst->log_cond);
        pthread_mutex_unlock(&inst->log_lock);
        src += chunk_len;
        len -= chunk_len;
    }
}

/* --------------------------------------------------------------- *
 *  cancellable, single-owner control writer                       *
 * --------------------------------------------------------------- */

/* Every admitted frame is charged exactly once before it becomes visible to
 * the writer and stays charged through its actual free: asynchronous frames
 * are freed here, while synchronous frames remain charged until their waiter
 * wakes, destroys the per-frame condition, and frees under writer_lock. */
static void free_outbound_locked(ish_embed_instance_t *inst,
                                 struct outbound_frame *frame) {
    size_t frame_len = frame->len;
    int accounted = frame->accounted;
    frame->accounted = 0;
    free(frame);
    if (accounted) {
        if (inst->writer_accounted_bytes >= frame_len)
            inst->writer_accounted_bytes -= frame_len;
        else
            inst->writer_accounted_bytes = 0;
        if (inst->writer_accounted_frames > 0)
            inst->writer_accounted_frames--;
    }
}

static void finish_outbound_locked(ish_embed_instance_t *inst,
                                   struct outbound_frame *frame, int result) {
    frame->result = result;
    frame->done = 1;
    if (frame->wait_for_completion) {
        pthread_cond_signal(&frame->done_cond);
    } else {
        pthread_cond_destroy(&frame->done_cond);
        free_outbound_locked(inst, frame);
    }
}

/* Only the writer pthread calls this function. The fd is nonblocking, and a
 * short poll interval makes a fatal reader-side abort able to cancel a full
 * control pipe without closing an fd underneath an in-flight write. */
static int write_outbound_frame(ish_embed_instance_t *inst,
                                struct outbound_frame *frame) {
    while (frame->written < frame->len) {
        if (atomic_load(&inst->writer_stopping))
            return atomic_load(&inst->writer_stop_status);

        ssize_t n = write(inst->host_to_guest_w,
                          frame->bytes + frame->written,
                          frame->len - frame->written);
        if (n > 0) {
            frame->written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {
                .fd = inst->host_to_guest_w,
                .events = POLLOUT,
            };
            int ready = poll(&pfd, 1, 50);
            if (ready < 0 && errno == EINTR) continue;
            if (ready == 0) continue;
            if (ready > 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                continue;
        }
        return ISH_ERR_BROKEN_PIPE;
    }
    return ISH_OK;
}

static void *writer_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;

    /* On platforms without F_SETNOSIGPIPE, contain SIGPIPE to this dedicated
     * thread. Do not restore the mask: a pending SIGPIPE must disappear with
     * the writer thread instead of being delivered to the host application. */
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    for (;;) {
        pthread_mutex_lock(&inst->writer_lock);
        while (!inst->writer_head && !atomic_load(&inst->writer_stopping))
            pthread_cond_wait(&inst->writer_cond, &inst->writer_lock);

        if (atomic_load(&inst->writer_stopping)) {
            int status = atomic_load(&inst->writer_stop_status);
            while (inst->writer_head) {
                struct outbound_frame *frame = inst->writer_head;
                inst->writer_head = frame->next;
                finish_outbound_locked(inst, frame, status);
            }
            inst->writer_tail = NULL;
            close_fd_slot(&inst->host_to_guest_w);
            pthread_mutex_unlock(&inst->writer_lock);
            return NULL;
        }

        struct outbound_frame *frame = inst->writer_head;
        inst->writer_head = frame->next;
        if (!inst->writer_head) inst->writer_tail = NULL;
        pthread_mutex_unlock(&inst->writer_lock);

        int result = write_outbound_frame(inst, frame);
        if (result != ISH_OK) {
            atomic_store(&inst->shutting_down, 1);
            atomic_store(&inst->writer_stop_status, result);
            atomic_store(&inst->writer_stopping, 1);
        }

        pthread_mutex_lock(&inst->writer_lock);
        finish_outbound_locked(inst, frame, result);
        pthread_mutex_unlock(&inst->writer_lock);
    }
}

static int enqueue_frame(ish_embed_instance_t *inst,
                         uint8_t type, uint8_t flags, uint32_t sid,
                         const void *payload, uint32_t payload_len,
                         int wait_for_completion,
                         enum outbound_admission_class admission_class,
                         uint64_t deadline_ms) {
    if (!inst || (payload_len > 0 && !payload) ||
        payload_len > ISH_EMBED_MAX_PROTOCOL_FRAME_BYTES)
        return ISH_ERR_INVALID_ARG;
    if (atomic_load(&inst->shutting_down) && type != ISH_FT_SHUTDOWN)
        return ISH_ERR_NOT_RUNNING;

    size_t frame_len = ISH_PROTO_HDR_SIZE + (size_t)payload_len;
    /* Admission precedes allocation while holding the same lock as dequeue
     * and completion. This prevents an unbounded number of concurrent callers
     * from each allocating a maximum-sized frame before discovering the queue
     * is full. The counters include the writer's current dequeued frame. */
    if (deadline_ms == 0) {
        pthread_mutex_lock(&inst->writer_lock);
    } else {
        if (!mutex_lock_until(&inst->writer_lock, deadline_ms))
            return ISH_ERR_TIMEOUT;
        if (now_ms() >= deadline_ms) {
            pthread_mutex_unlock(&inst->writer_lock);
            return ISH_ERR_TIMEOUT;
        }
    }
#ifdef ISH_EMBED_TESTING
    extern void ish_embed_test_after_writer_lock(uint8_t type);
    ish_embed_test_after_writer_lock(type);
#endif
    if (!inst->writer_thread_alive || atomic_load(&inst->writer_stopping) ||
        (atomic_load(&inst->shutting_down) && type != ISH_FT_SHUTDOWN)) {
        int result = atomic_load(&inst->writer_stopping)
            ? atomic_load(&inst->writer_stop_status) : ISH_ERR_NOT_RUNNING;
        pthread_mutex_unlock(&inst->writer_lock);
        return result;
    }
    size_t frame_ceiling = ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES;
    size_t byte_ceiling = ISH_EMBED_MAX_CONTROL_QUEUE_BYTES;
    if (admission_class == OUTBOUND_ADMISSION_NORMAL) {
        frame_ceiling -= ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES;
        byte_ceiling -= ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES;
    }
    int over_frames = inst->writer_accounted_frames >= frame_ceiling;
    int over_bytes = frame_len > byte_ceiling ||
        inst->writer_accounted_bytes >
            byte_ceiling - frame_len;
    if (over_frames || over_bytes) {
        pthread_mutex_unlock(&inst->writer_lock);
        return ISH_ERR_CONTROL_LIMIT;
    }

    struct outbound_frame *frame =
        (struct outbound_frame *)calloc(1, sizeof(*frame) + frame_len);
    if (!frame) {
        pthread_mutex_unlock(&inst->writer_lock);
        return ISH_ERR_OOM;
    }
    if (pthread_cond_init(&frame->done_cond, NULL) != 0) {
        free(frame);
        pthread_mutex_unlock(&inst->writer_lock);
        return ISH_ERR_THREAD;
    }
    frame->len = frame_len;
    frame->wait_for_completion = wait_for_completion;
    ish_proto_pack_hdr(frame->bytes, type, flags, payload_len, sid);
    if (payload_len)
        memcpy(frame->bytes + ISH_PROTO_HDR_SIZE, payload, payload_len);

    /* Allocation and a maximum-sized payload copy can consume the remainder of
     * a short deadline after the first check. Queue admission is the instant
     * the frame becomes visible, so reject an expired finite call immediately
     * before linking it rather than publishing a late SPAWN. */
    if (deadline_ms != 0 && now_ms() >= deadline_ms) {
        pthread_cond_destroy(&frame->done_cond);
        free(frame);
        pthread_mutex_unlock(&inst->writer_lock);
        return ISH_ERR_TIMEOUT;
    }

    frame->accounted = 1;
    inst->writer_accounted_bytes += frame_len;
    inst->writer_accounted_frames++;
    frame->next = NULL;
    if (inst->writer_tail) inst->writer_tail->next = frame;
    else inst->writer_head = frame;
    inst->writer_tail = frame;
    pthread_cond_signal(&inst->writer_cond);

    if (!wait_for_completion) {
        pthread_mutex_unlock(&inst->writer_lock);
        return ISH_OK;
    }
    while (!frame->done)
        pthread_cond_wait(&frame->done_cond, &inst->writer_lock);
    int result = frame->result;
    pthread_cond_destroy(&frame->done_cond);
    free_outbound_locked(inst, frame);
    pthread_mutex_unlock(&inst->writer_lock);
    return result;
}

static int send_frame(ish_embed_instance_t *inst,
                      uint8_t type, uint8_t flags, uint32_t sid,
                      const void *payload, uint32_t payload_len) {
    return enqueue_frame(inst, type, flags, sid, payload, payload_len, 1,
                         OUTBOUND_ADMISSION_NORMAL, 0);
}

static int send_frame_async_normal_until(
        ish_embed_instance_t *inst,
        uint8_t type, uint8_t flags, uint32_t sid,
        const void *payload, uint32_t payload_len,
        uint64_t deadline_ms) {
    return enqueue_frame(inst, type, flags, sid, payload, payload_len, 0,
                         OUTBOUND_ADMISSION_NORMAL, deadline_ms);
}

/* Lifecycle callers use capacity reserved inside the same total queue ceiling
 * and never wait for a stalled pipe write. They learn whether ownership was
 * admitted; a later pipe failure stops/drains the writer. Every call site must
 * have a bounded fallback when the requested transition is not observed. */
static int send_lifecycle_frame_async(ish_embed_instance_t *inst,
                                      uint8_t type, uint8_t flags, uint32_t sid,
                                      const void *payload,
                                      uint32_t payload_len) {
    return enqueue_frame(inst, type, flags, sid, payload, payload_len, 0,
                         OUTBOUND_ADMISSION_LIFECYCLE, 0);
}

#ifdef ISH_EMBED_TESTING
void ish_embed_test_control_usage(ish_embed_instance_t *inst,
                                  size_t *out_bytes, size_t *out_frames) {
    pthread_mutex_lock(&inst->writer_lock);
    if (out_bytes) *out_bytes = inst->writer_accounted_bytes;
    if (out_frames) *out_frames = inst->writer_accounted_frames;
    pthread_mutex_unlock(&inst->writer_lock);
}
#endif

/* --------------------------------------------------------------- *
 *  session list management                                        *
 * --------------------------------------------------------------- */

static struct ish_embed_session *find_session_locked(ish_embed_instance_t *inst, uint32_t id) {
    for (struct ish_embed_session *s = inst->sessions_head; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

/* Requires s->lock. */
static void enqueue_frame_locked(struct ish_embed_session *s, struct inbox_frame *f) {
    f->next = NULL;
    if (s->tail) s->tail->next = f; else s->head = f;
    s->tail = f;
    s->queued_bytes += f->len;
    s->queued_frames++;
    pthread_cond_broadcast(&s->cond);
}

/* Called only when the session is unlinked and its reference count is zero. */
static void session_destroy_unreferenced(struct ish_embed_session *s) {
    while (s->head) {
        struct inbox_frame *f = s->head;
        s->head = f->next;
        free(f->data);
        free(f);
    }
    s->tail = NULL;
    pthread_mutex_destroy(&s->stdin_lock);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}

static void session_release_reference(struct ish_embed_session *s) {
    int destroy = 0;
    pthread_mutex_lock(&s->lock);
    if (s->references > 0) s->references--;
    pthread_cond_broadcast(&s->cond);
    if (s->references == 0 && !s->linked) destroy = 1;
    pthread_mutex_unlock(&s->lock);
    if (destroy) session_destroy_unreferenced(s);
}

/* Unlink with the global lock order sess_lock -> session.lock. */
static void session_unlink(ish_embed_instance_t *inst, struct ish_embed_session *s) {
    pthread_mutex_lock(&inst->sess_lock);
    pthread_mutex_lock(&s->lock);
    if (s->linked) {
        if (s->prev) s->prev->next = s->next; else inst->sessions_head = s->next;
        if (s->next) s->next->prev = s->prev;
        s->prev = s->next = NULL;
        s->linked = 0;
    }
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&inst->sess_lock);
}

static void free_inbox_frame(struct inbox_frame *f) {
    if (!f) return;
    free(f->data);
    free(f);
}

/* Route a data frame while holding the only lookup lifetime lock. Returns
 * nonzero when the guest session should be killed after all locks are free. */
static int route_output_frame(ish_embed_instance_t *inst, uint32_t sid,
                              struct inbox_frame *f) {
    int kill_guest = 0;
    int accepted = 0;

    pthread_mutex_lock(&inst->sess_lock);
    struct ish_embed_session *s = find_session_locked(inst, sid);
    if (s) {
        pthread_mutex_lock(&s->lock);
        if (!s->closing && !s->guest_exited && !s->output_disabled) {
            int over_bytes = f->len > ISH_EMBED_MAX_SESSION_BACKLOG_BYTES ||
                s->queued_bytes > ISH_EMBED_MAX_SESSION_BACKLOG_BYTES - f->len;
            int over_frames = s->queued_frames >= ISH_EMBED_MAX_SESSION_BACKLOG_FRAMES;
            if (over_bytes || over_frames) {
                s->output_disabled = 1;
                if (!s->terminal_ready) {
                    s->terminal_ready = 1;
                    s->terminal_status = ISH_ERR_OUTPUT_LIMIT;
                }
                if (!s->overflow_kill_sent) {
                    s->overflow_kill_sent = 1;
                    kill_guest = 1;
                }
                pthread_cond_broadcast(&s->cond);
            } else {
                enqueue_frame_locked(s, f);
                accepted = 1;
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&inst->sess_lock);

    if (!accepted) free_inbox_frame(f);
    return kill_guest;
}

static void route_terminal(ish_embed_instance_t *inst, uint32_t sid,
                           int status, int32_t exit_code, int32_t signal) {
    pthread_mutex_lock(&inst->sess_lock);
    struct ish_embed_session *s = find_session_locked(inst, sid);
    if (s) {
        pthread_mutex_lock(&s->lock);
        s->guest_exited = 1;
        if (!s->terminal_ready) {
            s->terminal_ready = 1;
            s->terminal_status = status;
            s->terminal_exit_code = exit_code;
            s->terminal_signal = signal;
        }
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&inst->sess_lock);
}

static void close_control_pipe(ish_embed_instance_t *inst);

/* A fatal frame cannot be resynchronised safely. The reader is the sole owner
 * of this descriptor until shutdown joins it, so closing and clearing the
 * slot makes a guest blocked in a large write receive EPIPE without risking a
 * later double-close of a recycled descriptor. Closing control then makes PID
 * 1 leave its loop even when no event write was in progress. */
static void abort_protocol_pump(ish_embed_instance_t *inst) {
    atomic_store(&inst->shutting_down, 1);
    close_fd_slot(&inst->guest_to_host_r);
    close_control_pipe(inst);
}

/* Validate the complete wire shape before dispatch. A malformed supervisor
 * event is fatal because continuing after silently dropping or reinterpreting
 * one frame would leave session state ambiguous. */
static int event_frame_is_valid(uint8_t type, uint8_t flags, uint32_t sid,
                                const uint8_t *body, uint32_t plen) {
    if (plen > 0 && !body) return 0;
    switch (type) {
        case ISH_FT_HELLO_ACK:
            return flags == 0 && sid == 0 && plen == 12;
        case ISH_FT_SPAWNED:
            return flags == 0 && sid != 0 && plen == 4 &&
                   ish_proto_get_u32(body) != 0;
        case ISH_FT_STDOUT_DATA:
        case ISH_FT_STDERR_DATA:
            return flags == ISH_FF_SEQ_PRESENT && sid != 0 && plen >= 8;
        case ISH_FT_EXITED:
            return flags == 0 && sid != 0 && plen == 8;
        case ISH_FT_ERROR: {
            if (flags != 0 || sid == 0 || plen < 8) return 0;
            uint32_t message_len = ish_proto_get_u32(body + 4);
            return message_len == plen - 8;
        }
        case ISH_FT_LOG:
            return flags == 0 && sid == 0;
        case ISH_FT_SHUTDOWN_ACK:
        case ISH_FT_PONG:
            return flags == 0 && sid == 0 && plen == 0;
        default:
            return 0;
    }
}

static struct inbox_frame *allocate_output_frame(size_t len) {
#ifdef ISH_EMBED_TESTING
    extern int ish_embed_test_fail_output_allocation(void);
    if (ish_embed_test_fail_output_allocation()) return NULL;
#endif
    struct inbox_frame *f = (struct inbox_frame *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->data = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!f->data) {
        free(f);
        return NULL;
    }
    return f;
}

/* --------------------------------------------------------------- *
 *  reader thread: parse framed protocol from supervisor stdout    *
 * --------------------------------------------------------------- */

static void *reader_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;
    int terminal_status = ISH_ERR_BROKEN_PIPE;
    /* Admission may stop before PID 1 exits. Keep draining the blocking
     * supervisor event pipe until EOF so the guest can finish shutdown. */
    while (1) {
        uint8_t hdr[ISH_PROTO_HDR_SIZE];
        int r = read_full(inst->guest_to_host_r, hdr, sizeof(hdr));
        if (r != 0) break;
        uint8_t type, flags;
        uint32_t plen, sid;
        uint32_t declared_len = ((uint32_t)hdr[4] << 24) |
                                ((uint32_t)hdr[5] << 16) |
                                ((uint32_t)hdr[6] << 8) |
                                (uint32_t)hdr[7];
        if (ish_proto_parse_hdr(hdr, &type, &flags, &plen, &sid) < 0 ||
            declared_len > ISH_EMBED_MAX_PROTOCOL_FRAME_BYTES) {
            terminal_status = ISH_ERR_PROTOCOL;
            abort_protocol_pump(inst);
            break;
        }
        uint8_t *body = NULL;
        if (plen > 0) {
            body = (uint8_t *)malloc(plen);
            if (!body) {
                terminal_status = ISH_ERR_OOM;
                abort_protocol_pump(inst);
                break;
            }
            if (read_full(inst->guest_to_host_r, body, plen) != 0) { free(body); break; }
        }

        if (!event_frame_is_valid(type, flags, sid, body, plen)) {
            terminal_status = ISH_ERR_PROTOCOL;
            free(body);
            abort_protocol_pump(inst);
            goto reader_done;
        }

        switch (type) {
            case ISH_FT_HELLO_ACK: {
                int valid = ish_proto_get_u32(body) == ISH_EMBED_ABI_VERSION &&
                    body[4] == ISH_PROTO_VERSION &&
                    body[5] == 0 && body[6] == 0 && body[7] == 0 &&
                    ish_proto_get_u32(body + 8) > 0;
                pthread_mutex_lock(&inst->hello_lock);
                if (inst->hello_acked) valid = 0;
                if (valid) {
                    inst->hello_acked = 1;
                    inst->max_concurrent = ish_proto_get_u32(body + 8);
                } else {
                    inst->hello_status = ISH_ERR_PROTOCOL;
                }
                pthread_cond_broadcast(&inst->hello_cond);
                pthread_mutex_unlock(&inst->hello_lock);
                if (!valid) {
                    terminal_status = ISH_ERR_PROTOCOL;
                    free(body);
                    abort_protocol_pump(inst);
                    goto reader_done;
                }
                break;
            }
            case ISH_FT_SPAWNED:
                /* Informational; we don't surface guest_pid yet. */
                break;
            case ISH_FT_STDOUT_DATA:
            case ISH_FT_STDERR_DATA: {
                size_t output_len = (size_t)plen - 8;
                struct inbox_frame *f = allocate_output_frame(output_len);
                if (!f) {
                    terminal_status = ISH_ERR_OOM;
                    free(body);
                    abort_protocol_pump(inst);
                    goto reader_done;
                }
                f->kind = (type == ISH_FT_STDOUT_DATA) ? ISH_STREAM_STDOUT : ISH_STREAM_STDERR;
                f->seq = ish_proto_get_u64(body);
                f->len = output_len;
                if (f->len) memcpy(f->data, body + 8, f->len);
                if (route_output_frame(inst, sid, f)) {
                    /* Never wait for the control direction from the event
                     * reader. The writer pump preserves frame order while
                     * this thread keeps draining a potentially full event
                     * pipe, breaking the classic bidirectional pipe cycle. */
                    int kill_rc = send_lifecycle_frame_async(
                        inst, ISH_FT_SESSION_CLOSE, 0, sid, NULL, 0);
                    if (kill_rc != ISH_OK &&
                        kill_rc != ISH_ERR_NOT_RUNNING) {
                        terminal_status = kill_rc;
                        free(body);
                        abort_protocol_pump(inst);
                        goto reader_done;
                    }
                }
                break;
            }
            case ISH_FT_EXITED: {
                int32_t exit_code = ish_proto_get_i32(body);
                int32_t signal = ish_proto_get_i32(body + 4);
                route_terminal(inst, sid, ISH_OK, exit_code, signal);
                break;
            }
            case ISH_FT_ERROR: {
                int32_t errv = ish_proto_get_i32(body);
                route_terminal(inst, sid, ISH_ERR_SUPERVISOR, errv, 0);
                break;
            }
            case ISH_FT_LOG: {
                /* The bundled supervisor uses its dedicated stderr pipe, but
                 * retain protocol LOG compatibility through the same bounded
                 * queue as the dedicated diagnostic pipe. */
                enqueue_log_best_effort(inst, body, plen);
                break;
            }
            case ISH_FT_SHUTDOWN_ACK:
                atomic_store(&inst->shutting_down, 1);
                break;
            case ISH_FT_PONG:
            default:
                /* All known no-op/control events were shape-checked above;
                 * default is unreachable because unknown types are fatal. */
                break;
        }
        free(body);
    }

reader_done:
    /* signal any blocked readers that the world ended */
    atomic_store(&inst->shutting_down, 1);
    pthread_mutex_lock(&inst->hello_lock);
    if (!inst->hello_acked && inst->hello_status == ISH_OK)
        inst->hello_status = terminal_status;
    pthread_cond_broadcast(&inst->hello_cond);
    pthread_mutex_unlock(&inst->hello_lock);
    /* Any event EOF/error means the bidirectional protocol can no longer make
     * progress. Cancel an in-flight control write as well; the writer thread
     * owns the fd and closes it without a cross-thread close race. */
    close_control_pipe(inst);
    pthread_mutex_lock(&inst->sess_lock);
    for (struct ish_embed_session *s = inst->sessions_head; s; s = s->next) {
        pthread_mutex_lock(&s->lock);
        if (!s->terminal_ready) {
            s->terminal_ready = 1;
            s->terminal_status = terminal_status;
        }
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&inst->sess_lock);
    return NULL;
}

/* --------------------------------------------------------------- *
 *  log thread (supervisor stderr)                                 *
 * --------------------------------------------------------------- */

static void *log_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;
    uint8_t buf[ISH_EMBED_LOG_CHUNK_BYTES];
    /* As with the protocol reader, drain through EOF during shutdown. */
    while (1) {
        ssize_t r = read(inst->guest_log_r, buf, sizeof(buf));
        if (r > 0) enqueue_log_best_effort(inst, buf, (size_t)r);
        else if (r == 0) break;
        else if (errno != EINTR) break;
    }
    return NULL;
}

/* The sole code path that writes the user-provided diagnostic fd. It may
 * block indefinitely without slowing the guest log reader; shutdown cancels
 * that blocking write after publishing log_stopping and waking the queue. */
static void *log_writer_thread_main(void *arg) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)arg;
    uint8_t chunk[ISH_EMBED_LOG_CHUNK_BYTES];

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    /* Never permit cancellation while holding log_lock. Condvar wakeups are
     * driven by log_stopping; cancellation is only needed for a blocked
     * caller-fd write. */
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;) {
        pthread_mutex_lock(&inst->log_lock);
        while (inst->log_count == 0 && !atomic_load(&inst->log_stopping))
            pthread_cond_wait(&inst->log_cond, &inst->log_lock);
        if (atomic_load(&inst->log_stopping)) {
            pthread_mutex_unlock(&inst->log_lock);
            return NULL;
        }

        size_t len = inst->log_lengths[inst->log_head];
        memcpy(chunk, inst->log_chunks[inst->log_head], len);
        inst->log_head = (inst->log_head + 1) % ISH_EMBED_LOG_QUEUE_SLOTS;
        inst->log_count--;
        pthread_mutex_unlock(&inst->log_lock);

        (void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
#ifdef ISH_EMBED_TESTING
        extern void ish_embed_test_log_write_begin(void);
        ish_embed_test_log_write_begin();
#endif
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(inst->kernel_log_fd, chunk + written,
                              len - written);
            if (n > 0) {
                written += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break; /* diagnostics are best-effort */
        }
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    }
}

static void stop_log_threads(ish_embed_instance_t *inst) {
    atomic_store(&inst->log_stopping, 1);
    pthread_mutex_lock(&inst->log_lock);
    pthread_cond_broadcast(&inst->log_cond);
    pthread_mutex_unlock(&inst->log_lock);
}

/* --------------------------------------------------------------- *
 *  kernel pthread exit hook                                       *
 * --------------------------------------------------------------- */

static void on_kernel_exit(int code, void *ctx) {
    ish_embed_instance_t *inst = (ish_embed_instance_t *)ctx;
    (void)code;
    atomic_store(&inst->shutting_down, 1);
    /* fdtable_release owns the guest pipe descriptors and naturally wakes
     * the reader/log pumps. Publish completion last so cleanup can never
     * release inst while this callback is still dereferencing it. */
    pthread_mutex_lock(&inst->lifecycle_lock);
    atomic_store(&inst->kernel_exited, 1);
    pthread_cond_broadcast(&inst->lifecycle_cond);
    pthread_mutex_unlock(&inst->lifecycle_lock);
}

static void close_fd_slot(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_control_pipe(ish_embed_instance_t *inst) {
    pthread_mutex_lock(&inst->writer_lock);
    if (inst->writer_thread_alive) {
        if (!atomic_load(&inst->writer_stopping)) {
            atomic_store(&inst->writer_stop_status, ISH_ERR_NOT_RUNNING);
            atomic_store(&inst->writer_stopping, 1);
        }
        pthread_cond_broadcast(&inst->writer_cond);
    } else {
        close_fd_slot(&inst->host_to_guest_w);
    }
    pthread_mutex_unlock(&inst->writer_lock);
}

static int wait_for_kernel_exit(ish_embed_instance_t *inst, uint32_t wait_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000;
    deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }

    pthread_mutex_lock(&inst->lifecycle_lock);
    while (!atomic_load(&inst->kernel_exited)) {
        int rc = pthread_cond_timedwait(&inst->lifecycle_cond,
                                        &inst->lifecycle_lock, &deadline);
        if (rc == ETIMEDOUT) break;
    }
    int exited = atomic_load(&inst->kernel_exited);
    pthread_mutex_unlock(&inst->lifecycle_lock);
    return exited;
}

static int join_instance_threads(ish_embed_instance_t *inst) {
    int result = ISH_OK;
    if (inst->kernel_thread_alive) {
        if (ish_ffi_task_join() == 0) inst->kernel_thread_alive = 0;
        else result = ISH_ERR_THREAD;
    }
    if (inst->reader_thread_alive) {
        /* read/write/poll are cancellation points. Once the kernel thread has
         * stopped, cancellation is a bounded fallback for an unusual log sink
         * or a partially received terminal frame. No session locks are held at
         * those points. */
        (void)pthread_cancel(inst->reader_thread);
        if (pthread_join(inst->reader_thread, NULL) == 0)
            inst->reader_thread_alive = 0;
        else
            result = ISH_ERR_THREAD;
    }
    if (inst->log_thread_alive) {
        (void)pthread_cancel(inst->log_thread);
        if (pthread_join(inst->log_thread, NULL) == 0)
            inst->log_thread_alive = 0;
        else
            result = ISH_ERR_THREAD;
    }
    stop_log_threads(inst);
    if (inst->log_writer_thread_alive) {
        /* No lock is held at its only cancellation point (write). If it is
         * waiting on the queue, log_stopping wakes it for an ordinary exit. */
        (void)pthread_cancel(inst->log_writer_thread);
        if (pthread_join(inst->log_writer_thread, NULL) == 0)
            inst->log_writer_thread_alive = 0;
        else
            result = ISH_ERR_THREAD;
    }
    if (inst->writer_thread_alive) {
        if (pthread_join(inst->writer_thread, NULL) == 0)
            inst->writer_thread_alive = 0;
        else
            result = ISH_ERR_THREAD;
    }
    return result;
}

static void destroy_instance_primitives(ish_embed_instance_t *inst) {
    pthread_mutex_destroy(&inst->writer_lock);
    pthread_cond_destroy(&inst->writer_cond);
    pthread_mutex_destroy(&inst->log_lock);
    pthread_cond_destroy(&inst->log_cond);
    pthread_mutex_destroy(&inst->sess_lock);
    pthread_mutex_destroy(&inst->spawn_lock);
    pthread_mutex_destroy(&inst->hello_lock);
    pthread_cond_destroy(&inst->hello_cond);
    pthread_mutex_destroy(&inst->lifecycle_lock);
    pthread_cond_destroy(&inst->lifecycle_cond);
}

/* --------------------------------------------------------------- *
 *  boot                                                           *
 * --------------------------------------------------------------- */

/* Source builds without a generated blob retain the historical RootFS path.
 * Release XCFrameworks install and execute a content-addressed private path. */
#define ISH_DEFAULT_SUPERVISOR_PATH "/sbin/ishsv"

int ish_embed_boot(const ish_embed_boot_opts_t *opts,
                   ish_embed_instance_t **out_instance) {
    if (!opts || !opts->rootfs_path || !out_instance)
        return ISH_ERR_INVALID_ARG;
    *out_instance = NULL;

    pthread_mutex_lock(&g_instance_lock);
    if (g_instance || g_boot_consumed) {
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_ALREADY_BOOTED;
    }

    ish_embed_instance_t *inst = (ish_embed_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) { pthread_mutex_unlock(&g_instance_lock); return ISH_ERR_OOM; }
    inst->host_to_guest_w = -1;
    inst->guest_to_host_r = -1;
    inst->guest_log_r     = -1;
    inst->guest_stdin_r   = -1;
    inst->guest_stdout_w  = -1;
    inst->guest_stderr_w  = -1;
    inst->kernel_log_fd   = -1;
    pthread_mutex_init(&inst->writer_lock, NULL);
    pthread_cond_init(&inst->writer_cond, NULL);
    pthread_mutex_init(&inst->log_lock, NULL);
    pthread_cond_init(&inst->log_cond, NULL);
    pthread_mutex_init(&inst->sess_lock, NULL);
    pthread_mutex_init(&inst->spawn_lock, NULL);
    pthread_mutex_init(&inst->hello_lock, NULL);
    pthread_cond_init(&inst->hello_cond, NULL);
    pthread_mutex_init(&inst->lifecycle_lock, NULL);
    pthread_cond_init(&inst->lifecycle_cond, NULL);
    atomic_store(&inst->next_session_id, 1);
    atomic_store(&inst->shutting_down, 0);
    atomic_store(&inst->kernel_exited, 0);
    atomic_store(&inst->writer_stopping, 0);
    atomic_store(&inst->writer_stop_status, ISH_ERR_NOT_RUNNING);
    atomic_store(&inst->log_stopping, 0);
    inst->hello_status = ISH_OK;
    g_boot_consumed = 1;

    /* Own a duplicate so the caller can close or reuse its descriptor without
     * redirecting logs into an unrelated resource. Failure disables optional
     * diagnostics rather than failing the Linux runtime boot. */
    int requested_log_fd = opts->kernel_log_fd >= 0 ? opts->kernel_log_fd : 2;
    inst->kernel_log_fd = duplicate_cloexec_local(requested_log_fd);

    int err = ISH_ERR_BOOT;
    int exit_hook_registered = 0;

    /* create three pipes:
     *   p_in:  host writes to host_to_guest_w; guest reads from guest_stdin_r
     *   p_out: guest writes to guest_stdout_w; host reads from guest_to_host_r
     *   p_log: guest writes to guest_stderr_w; host reads from guest_log_r
     *          (this is the SUPERVISOR's stderr — diagnostic logs from
     *           PID 1 inside iSH)
     */
    int p_in[2], p_out[2], p_log[2];
    if (pipe(p_in) < 0) { err = ISH_ERR_PIPE; goto fail; }
    inst->guest_stdin_r   = p_in[0];
    inst->host_to_guest_w = p_in[1];
    if (pipe(p_out) < 0) { err = ISH_ERR_PIPE; goto fail; }
    inst->guest_to_host_r = p_out[0];
    inst->guest_stdout_w  = p_out[1];
    if (pipe(p_log) < 0) { err = ISH_ERR_PIPE; goto fail; }
    inst->guest_log_r     = p_log[0];
    inst->guest_stderr_w  = p_log[1];

    if (set_cloexec_local(inst->host_to_guest_w) < 0 ||
        set_cloexec_local(inst->guest_to_host_r) < 0 ||
        set_cloexec_local(inst->guest_log_r) < 0 ||
        set_nonblocking_local(inst->host_to_guest_w) < 0 ||
        set_nosigpipe_local(inst->host_to_guest_w) < 0) {
        err = ISH_ERR_PIPE;
        goto fail;
    }
    /* guest_*_r/w are handed to the kernel and become guest-side fds;
     * we keep them in the host process but the kernel "owns" them. */

    /* ---- iSH kernel boot sequence (mirrors xX_main_Xx) ---- */
    if (ish_ffi_mount_fakefs(opts->rootfs_path) < 0) {
        err = ISH_ERR_MOUNT;
        goto fail;
    }
    if (ish_ffi_become_init() < 0) {
        err = ISH_ERR_BECOME_INIT;
        goto fail;
    }
    if (ish_ffi_create_devices() < 0) {
        err = ISH_ERR_BOOT;
        goto fail;
    }

    const char *sup = opts->supervisor_guest_path;
    if (!sup) {
#ifdef ISH_EMBED_BUNDLED_SUPERVISOR
        const char *bundled_sha256 = ish_embed_bundled_supervisor_sha256;
        const char *bundled_guest_path =
            ish_embed_bundled_supervisor_guest_path;
#ifdef ISH_EMBED_TESTING
        extern void ish_embed_test_bundled_supervisor_metadata(
            const char **sha256, const char **guest_path);
        ish_embed_test_bundled_supervisor_metadata(
            &bundled_sha256, &bundled_guest_path);
#endif
        if (!ish_embed_supervisor_metadata_valid(
                ish_embed_bundled_supervisor,
                ish_embed_bundled_supervisor_len,
                bundled_sha256,
                bundled_guest_path) ||
            ish_ffi_install_executable(
                bundled_guest_path,
                ish_embed_bundled_supervisor,
                ish_embed_bundled_supervisor_len,
                0755) < 0) {
            err = ISH_ERR_SUPERVISOR_INSTALL;
            goto fail;
        }
        sup = bundled_guest_path;
#else
        sup = ISH_DEFAULT_SUPERVISOR_PATH;
#endif
    }

    if (ish_ffi_install_pipe_stdio(inst->guest_stdin_r,
                                   inst->guest_stdout_w,
                                   inst->guest_stderr_w) < 0) {
        err = ISH_ERR_STDIO;
        goto fail;
    }

    const char *workdir = opts->workdir ? opts->workdir : "/";
    if (ish_ffi_chdir(workdir) < 0) {
        /* fall back to / */
        if (ish_ffi_chdir("/") < 0) {
            err = ISH_ERR_CHDIR;
            goto fail;
        }
    }

    /* register exit hook BEFORE task_start so we don't race */
    ish_ffi_register_exit_hook(on_kernel_exit, inst);
    exit_hook_registered = 1;

    /* Build packed argv: just argv[0]=sup, NUL term + outer NUL */
    char argv_packed[256];
    size_t off = 0;
    size_t L = strlen(sup);
    if (L + 2 > sizeof(argv_packed)) { err = ISH_ERR_INVALID_ARG; goto fail; }
    inst->supervisor_guest_path = strdup(sup);
    if (!inst->supervisor_guest_path) { err = ISH_ERR_OOM; goto fail; }
    memcpy(argv_packed, sup, L); argv_packed[L] = 0; off = L + 1;
    argv_packed[off] = 0;

    char envp_packed[256];
    size_t eo = 0;
    const char *env0 = "PATH=/sbin:/usr/sbin:/usr/local/sbin:/bin:/usr/bin:/usr/local/bin";
    size_t e0 = strlen(env0);
    if (e0 + 2 > sizeof(envp_packed)) { err = ISH_ERR_INVALID_ARG; goto fail; }
    memcpy(envp_packed, env0, e0); envp_packed[e0] = 0; eo = e0 + 1;
    envp_packed[eo] = 0;

    if (ish_ffi_execve(sup, 1, argv_packed, envp_packed) < 0) {
        err = ISH_ERR_EXECVE;
        goto fail;
    }

    /* Start kernel pthread BEFORE the reader, so the supervisor exists
     * to answer our handshake. */
    if (ish_ffi_task_start() < 0) { err = ISH_ERR_THREAD; goto fail; }
    inst->kernel_thread_alive = 1;
    /* The kernel fdtable now owns these descriptors. Never close them from
     * host cleanup: fdtable_release is the single owner and produces EOF. */
    inst->guest_stdin_r = -1;
    inst->guest_stdout_w = -1;
    inst->guest_stderr_w = -1;

    /* Spawn reader & log threads */
    if (pthread_create(&inst->reader_thread, NULL, reader_thread_main, inst) != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }
    inst->reader_thread_alive = 1;
    if (pthread_create(&inst->log_thread, NULL, log_thread_main, inst) != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }
    inst->log_thread_alive = 1;
    if (pthread_create(&inst->log_writer_thread, NULL,
                       log_writer_thread_main, inst) != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }
    inst->log_writer_thread_alive = 1;
    pthread_mutex_lock(&inst->writer_lock);
    int writer_create_rc =
        pthread_create(&inst->writer_thread, NULL, writer_thread_main, inst);
    if (writer_create_rc == 0) inst->writer_thread_alive = 1;
    pthread_mutex_unlock(&inst->writer_lock);
    if (writer_create_rc != 0) {
        err = ISH_ERR_THREAD; goto fail;
    }

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
        deadline.tv_sec += ISH_EMBED_HELLO_TIMEOUT_MS / 1000;
        deadline.tv_nsec += (long)(ISH_EMBED_HELLO_TIMEOUT_MS % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec += 1;
        }
        pthread_mutex_lock(&inst->hello_lock);
        while (!inst->hello_acked && inst->hello_status == ISH_OK &&
               !atomic_load(&inst->shutting_down)) {
            int rc = pthread_cond_timedwait(&inst->hello_cond, &inst->hello_lock, &deadline);
            if (rc == ETIMEDOUT) break;
        }
        int acked = inst->hello_acked;
        int handshake_status = inst->hello_status;
        if (!acked && handshake_status == ISH_OK)
            handshake_status = ISH_ERR_TIMEOUT;
        pthread_mutex_unlock(&inst->hello_lock);
        if (!acked) { err = handshake_status; goto fail; }
    }

    g_instance = inst;
    *out_instance = inst;
    pthread_mutex_unlock(&g_instance_lock);
    return ISH_OK;

fail:;
    int saved = err;
    atomic_store(&inst->shutting_down, 1);
    close_control_pipe(inst);

    if (inst->kernel_thread_alive) {
        /* Closing the control pipe makes a correctly-started supervisor
         * observe EOF and exit. Do not release inst until its callback has
         * completed and the joinable kernel pthread has been reaped. */
        if (!wait_for_kernel_exit(inst, 15000)) {
            /* Memory-safe quarantine: an uncooperative kernel may still hold
             * inst through the registered callback. Keep the singleton live
             * and permanently reject another boot instead of risking UAF. */
            g_instance = inst;
            pthread_mutex_unlock(&g_instance_lock);
            return saved;
        }
        if (join_instance_threads(inst) != ISH_OK) {
            /* A failed join cannot prove that every thread released inst.
             * Quarantine it just like an uncooperative kernel. */
            g_instance = inst;
            pthread_mutex_unlock(&g_instance_lock);
            return saved;
        }
    }

    close_fd_slot(&inst->guest_to_host_r);
    close_fd_slot(&inst->guest_log_r);
    close_fd_slot(&inst->guest_stdin_r);
    close_fd_slot(&inst->guest_stdout_w);
    close_fd_slot(&inst->guest_stderr_w);
    close_fd_slot(&inst->kernel_log_fd);
    if (exit_hook_registered) ish_ffi_register_exit_hook(NULL, NULL);
    destroy_instance_primitives(inst);
    free(inst->supervisor_guest_path);
    free(inst);
    pthread_mutex_unlock(&g_instance_lock);
    return saved;
}

/* --------------------------------------------------------------- *
 *  spawn / sessions                                               *
 * --------------------------------------------------------------- */

/* Serialize argv/envp/chroot/initial-winsize into a SPAWN payload.
 * argv must not be NULL. The winsize tail was introduced in proto v3 and is
 * always emitted by the current exact-version v4 host. */
#define ISH_EMBED_MAX_SPAWN_VECTOR_ENTRIES 4096u

static size_t spawn_payload_limit(void) {
    size_t host_limit = ISH_EMBED_MAX_PROTOCOL_FRAME_BYTES;
    size_t protocol_limit = ISH_PROTO_MAX_PAYLOAD;
    return host_limit < protocol_limit ? host_limit : protocol_limit;
}

static int checked_spawn_size_add(size_t *total, size_t amount,
                                  size_t limit) {
    if (!total || amount > SIZE_MAX - *total) return ISH_ERR_INVALID_ARG;
    size_t next = *total + amount;
    if (next > UINT32_MAX || next > limit) return ISH_ERR_INVALID_ARG;
    *total = next;
    return ISH_OK;
}

static int measure_spawn_string(const char *value, size_t limit,
                                size_t *out_len) {
    if (!out_len) return ISH_ERR_INVALID_ARG;
    if (!value) {
        *out_len = 0;
        return ISH_OK;
    }
    /* Bound the scan to the largest string that can possibly fit on wire. */
    size_t len = strnlen(value, limit + 1);
    if (len > limit || len > UINT32_MAX) return ISH_ERR_INVALID_ARG;
    *out_len = len;
    return ISH_OK;
}

static int measure_spawn_vector(const char *const *values, size_t limit,
                                size_t *total, size_t *out_count) {
    size_t count = 0;
    if (values) {
        while (values[count]) {
            if (count >= ISH_EMBED_MAX_SPAWN_VECTOR_ENTRIES ||
                count >= UINT32_MAX)
                return ISH_ERR_INVALID_ARG;
            size_t len = 0;
            int rc = measure_spawn_string(values[count], limit, &len);
            if (rc != ISH_OK ||
                checked_spawn_size_add(total, 4, limit) != ISH_OK ||
                checked_spawn_size_add(total, len, limit) != ISH_OK)
                return ISH_ERR_INVALID_ARG;
            count++;
        }
    }
    *out_count = count;
    return ISH_OK;
}

static int append_spawn_u32(uint8_t *buf, size_t cap, size_t *off,
                            uint32_t value) {
    if (!buf || !off || *off > cap || cap - *off < 4)
        return ISH_ERR_INVALID_ARG;
    ish_proto_put_u32(buf + *off, value);
    *off += 4;
    return ISH_OK;
}

static int append_spawn_string(uint8_t *buf, size_t cap, size_t *off,
                               const char *value, size_t limit) {
    size_t len = 0;
    if (measure_spawn_string(value, limit, &len) != ISH_OK ||
        len > UINT32_MAX || *off > cap || cap - *off < 4 ||
        len > cap - *off - 4)
        return ISH_ERR_INVALID_ARG;
    ish_proto_put_u32(buf + *off, (uint32_t)len);
    *off += 4;
    if (len) {
        memcpy(buf + *off, value, len);
        *off += len;
    }
    return ISH_OK;
}

static int build_spawn_payload(const char *cwd,
                               const char *const *argv,
                               const char *const *envp,
                               const char *chroot_path,
                               uint16_t init_rows, uint16_t init_cols,
                               uint16_t init_xpix, uint16_t init_ypix,
                               uint8_t **out_buf, uint32_t *out_len) {
    if (!argv || !argv[0] || !out_buf || !out_len)
        return ISH_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    const size_t limit = spawn_payload_limit();
    size_t cap = 0;
    size_t argc = 0, envc = 0;
    size_t ignored_len = 0;
    if (measure_spawn_string(cwd, limit, &ignored_len) != ISH_OK ||
        checked_spawn_size_add(&cap, 4, limit) != ISH_OK ||
        checked_spawn_size_add(&cap, ignored_len, limit) != ISH_OK ||
        checked_spawn_size_add(&cap, 4, limit) != ISH_OK ||
        measure_spawn_vector(argv, limit, &cap, &argc) != ISH_OK ||
        checked_spawn_size_add(&cap, 4, limit) != ISH_OK ||
        measure_spawn_vector(envp, limit, &cap, &envc) != ISH_OK ||
        measure_spawn_string(chroot_path, limit, &ignored_len) != ISH_OK ||
        checked_spawn_size_add(&cap, 4, limit) != ISH_OK ||
        checked_spawn_size_add(&cap, ignored_len, limit) != ISH_OK ||
        checked_spawn_size_add(&cap, 8, limit) != ISH_OK)
        return ISH_ERR_INVALID_ARG;

    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return ISH_ERR_OOM;
    size_t off = 0;
    int rc = append_spawn_string(buf, cap, &off, cwd, limit);
    if (rc != ISH_OK || append_spawn_u32(buf, cap, &off, (uint32_t)argc) != ISH_OK)
        goto invalid;
    for (size_t i = 0; i < argc; i++) {
        if (!argv[i] ||
            append_spawn_string(buf, cap, &off, argv[i], limit) != ISH_OK)
            goto invalid;
    }
    if (append_spawn_u32(buf, cap, &off, (uint32_t)envc) != ISH_OK)
        goto invalid;
    for (size_t i = 0; i < envc; i++) {
        if (!envp[i] ||
            append_spawn_string(buf, cap, &off, envp[i], limit) != ISH_OK)
            goto invalid;
    }
    if (append_spawn_string(buf, cap, &off, chroot_path, limit) != ISH_OK ||
        off > cap || cap - off != 8)
        goto invalid;
    /* v3 tail: initial winsize. Zero means "use supervisor default". */
    ish_proto_put_u16(buf + off, init_rows); off += 2;
    ish_proto_put_u16(buf + off, init_cols); off += 2;
    ish_proto_put_u16(buf + off, init_xpix); off += 2;
    ish_proto_put_u16(buf + off, init_ypix); off += 2;
    if (off != cap || off > UINT32_MAX || off > limit) goto invalid;
    *out_buf = buf;
    *out_len = (uint32_t)off;
    return ISH_OK;

invalid:
    free(buf);
    return ISH_ERR_INVALID_ARG;
}

static int spawn_live_instance_serialized(ish_embed_instance_t *inst,
                                          const ish_embed_spawn_opts_t *opts,
                                          ish_embed_session_t **out_session,
                                          int wait_for_write,
                                          uint64_t deadline_ms) {
    if (!inst || !opts || !out_session) return ISH_ERR_INVALID_ARG;
    if (!opts->argv || !opts->argv[0]) return ISH_ERR_INVALID_ARG;
    if (atomic_load(&inst->shutting_down)) return ISH_ERR_NOT_RUNNING;
    *out_session = NULL;

    uint32_t sid = atomic_fetch_add(&inst->next_session_id, 1);

    struct ish_embed_session *s = (struct ish_embed_session *)calloc(1, sizeof(*s));
    if (!s) return ISH_ERR_OOM;
    s->inst = inst;
    s->id   = sid;
    s->streaming_deadline_ms = deadline_ms;
    s->references = 1;
    pthread_mutex_init(&s->stdin_lock, NULL);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    /* link */
    pthread_mutex_lock(&inst->sess_lock);
    if (atomic_load(&inst->shutting_down)) {
        pthread_mutex_unlock(&inst->sess_lock);
        session_release_reference(s);
        return ISH_ERR_NOT_RUNNING;
    }
    s->linked = 1;
    s->next = inst->sessions_head;
    if (inst->sessions_head) inst->sessions_head->prev = s;
    inst->sessions_head = s;
    pthread_mutex_unlock(&inst->sess_lock);

    uint8_t *payload = NULL;
    uint32_t plen = 0;
    int rc = build_spawn_payload(opts->cwd, opts->argv, opts->envp,
                                 opts->chroot_path,
                                 opts->init_rows, opts->init_cols,
                                 opts->init_xpixel, opts->init_ypixel,
                                 &payload, &plen);
    if (rc != 0) {
        session_unlink(inst, s);
        session_release_reference(s);
        return rc;
    }
    if (deadline_ms != 0 && now_ms() >= deadline_ms) {
        free(payload);
        session_unlink(inst, s);
        session_release_reference(s);
        return ISH_ERR_TIMEOUT;
    }
    uint8_t flags = 0;
    if (opts->allocate_tty)              flags |= ISH_FF_TTY;
    if (opts->merge_stderr_into_stdout)  flags |= ISH_FF_MERGE_STDERR;
    rc = wait_for_write
        ? send_frame(inst, ISH_FT_SPAWN, flags, sid, payload, plen)
        : send_frame_async_normal_until(inst, ISH_FT_SPAWN, flags, sid,
                                        payload, plen, deadline_ms);
    free(payload);
    if (rc != 0) {
        session_unlink(inst, s);
        session_release_reference(s);
        return rc;
    }
    *out_session = s;
    return ISH_OK;
}

static int spawn_live_instance(ish_embed_instance_t *inst,
                               const ish_embed_spawn_opts_t *opts,
                               ish_embed_session_t **out_session) {
    /* build_spawn_payload intentionally allocates before enqueue_frame can
     * copy it into an admitted outbound frame. Keep the complete staging and
     * enqueue/send lifetime behind one gate: concurrent maximum-sized spawns
     * cannot each park an extra 1 MiB allocation outside writer accounting,
     * while all non-SPAWN frames remain independently bounded by the writer
     * byte/frame ceilings. */
    pthread_mutex_lock(&inst->spawn_lock);
    int rc = spawn_live_instance_serialized(inst, opts, out_session, 1, 0);
    pthread_mutex_unlock(&inst->spawn_lock);
    return rc;
}

static int spawn_live_instance_async_until(
        ish_embed_instance_t *inst, const ish_embed_spawn_opts_t *opts,
        ish_embed_session_t **out_session, uint64_t deadline_ms) {
    if (!mutex_lock_until(&inst->spawn_lock, deadline_ms))
        return ISH_ERR_TIMEOUT;
    int rc = spawn_live_instance_serialized(
        inst, opts, out_session, 0, deadline_ms);
    pthread_mutex_unlock(&inst->spawn_lock);
    return rc;
}

int ish_embed_spawn(ish_embed_instance_t *inst,
                    const ish_embed_spawn_opts_t *opts,
                    ish_embed_session_t **out_session) {
    if (!opts || !out_session || !opts->argv || !opts->argv[0])
        return ISH_ERR_INVALID_ARG;
    *out_session = NULL;
    uint64_t deadline = opts->timeout_ms > 0
        ? now_ms() + opts->timeout_ms : 0;
    int gate = instance_call_begin_until(inst, deadline);
    if (gate != ISH_OK) return gate;
    int rc = deadline != 0
        ? spawn_live_instance_async_until(inst, opts, out_session, deadline)
        : spawn_live_instance(inst, opts, out_session);
    instance_call_end(inst);
    return rc;
}

static int spawn_oneshot_until(ish_embed_instance_t *inst,
                               const ish_embed_spawn_opts_t *opts,
                               ish_embed_session_t **out_session,
                               uint64_t deadline_ms) {
    int gate = instance_call_begin_until(inst, deadline_ms);
    if (gate != ISH_OK) return gate;
    int rc = spawn_live_instance_async_until(
        inst, opts, out_session, deadline_ms);
    instance_call_end(inst);
    return rc;
}

int ish_embed_session_retain(ish_embed_session_t *s) {
    if (!s) return ISH_ERR_INVALID_ARG;
    pthread_mutex_lock(&s->lock);
    if (s->closing || s->owner_released) {
        pthread_mutex_unlock(&s->lock);
        return ISH_ERR_NO_SESSION;
    }
    s->references++;
    pthread_mutex_unlock(&s->lock);
    return ISH_OK;
}

void ish_embed_session_release(ish_embed_session_t *s) {
    if (!s) return;
    session_release_reference(s);
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

    /* Wait for queued output, a terminal result, or close. */
    if (wait_ms == UINT32_MAX) {
        while (!s->head && !s->terminal_ready && !s->closing)
            pthread_cond_wait(&s->cond, &s->lock);
    } else if (!s->head && !s->terminal_ready && !s->closing) {
        if (wait_ms == 0) {
            pthread_mutex_unlock(&s->lock);
            return ISH_ERR_TIMEOUT;
        }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec  += wait_ms / 1000;
        dl.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_nsec -= 1000000000L; dl.tv_sec += 1; }
        while (!s->head && !s->terminal_ready && !s->closing) {
            int rc = pthread_cond_timedwait(&s->cond, &s->lock, &dl);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&s->lock);
                return ISH_ERR_TIMEOUT;
            }
        }
    }

    struct inbox_frame *f = s->head;
    if (f) {
        s->head = f->next;
        if (!s->head) s->tail = NULL;
        s->queued_bytes -= f->len;
        s->queued_frames--;
    } else if (s->terminal_ready && !s->terminal_consumed) {
        int status = s->terminal_status;
        int32_t exit_code = s->terminal_exit_code;
        int32_t signal = s->terminal_signal;
        s->terminal_consumed = 1;
        pthread_mutex_unlock(&s->lock);
        if (status != ISH_OK) return status;
        if (out_kind) *out_kind = ISH_STREAM_EXITED;
        if (out_exit_code) *out_exit_code = exit_code;
        if (out_signal) *out_signal = signal;
        return ISH_OK;
    } else {
        pthread_mutex_unlock(&s->lock);
        return ISH_ERR_NO_SESSION;
    }
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

static int session_write_until(ish_embed_session_t *s,
                               const uint8_t *buf, size_t len,
                               uint64_t deadline_ms,
                               int bounded) {
    if (!s) return ISH_ERR_INVALID_ARG;
    if (len == 0) return ISH_OK;
    if (bounded) {
        if (!mutex_lock_until(&s->stdin_lock, deadline_ms))
            return ISH_ERR_TIMEOUT;
    } else {
        pthread_mutex_lock(&s->stdin_lock);
    }
    pthread_mutex_lock(&s->lock);
    if (s->closing) {
        pthread_mutex_unlock(&s->lock);
        pthread_mutex_unlock(&s->stdin_lock);
        return ISH_ERR_NO_SESSION;
    }
    if (s->stdin_closed) {
        pthread_mutex_unlock(&s->lock);
        pthread_mutex_unlock(&s->stdin_lock);
        return ISH_ERR_BROKEN_PIPE;
    }
    ish_embed_instance_t *inst = s->inst;
    uint32_t sid = s->id;
    pthread_mutex_unlock(&s->lock);
    /* split into 64KiB chunks to keep frames bounded */
    while (len > 0) {
        size_t chunk = len > 65536 ? 65536 : len;
        int rc = bounded
            ? send_frame_async_normal_until(
                inst, ISH_FT_STDIN_DATA, 0, sid, buf, (uint32_t)chunk,
                deadline_ms)
            : send_frame(
                inst, ISH_FT_STDIN_DATA, 0, sid, buf, (uint32_t)chunk);
        if (rc != 0) {
            pthread_mutex_unlock(&s->stdin_lock);
            return rc;
        }
        buf += chunk; len -= chunk;
    }
    pthread_mutex_unlock(&s->stdin_lock);
    return ISH_OK;
}

int ish_embed_session_write(ish_embed_session_t *s,
                            const uint8_t *buf, size_t len) {
    if (!s) return ISH_ERR_INVALID_ARG;
    uint64_t deadline_ms = s->streaming_deadline_ms;
    return session_write_until(
        s, buf, len, deadline_ms, deadline_ms != 0);
}

int ish_embed_session_write_timeout(ish_embed_session_t *s,
                                    const uint8_t *buf, size_t len,
                                    uint32_t timeout_ms) {
    if (!s || timeout_ms == 0) return ISH_ERR_INVALID_ARG;
    uint64_t now = now_ms();
    uint64_t call_deadline = now > UINT64_MAX - timeout_ms
        ? UINT64_MAX : now + timeout_ms;
    uint64_t session_deadline = s->streaming_deadline_ms;
    uint64_t deadline_ms =
        session_deadline != 0 && session_deadline < call_deadline
            ? session_deadline : call_deadline;
    return session_write_until(s, buf, len, deadline_ms, 1);
}

static int send_session_control(ish_embed_session_t *s, uint8_t type,
                                const void *payload, uint32_t payload_len) {
    pthread_mutex_lock(&s->lock);
    if (s->closing) {
        pthread_mutex_unlock(&s->lock);
        return ISH_ERR_NO_SESSION;
    }
    s->active_controls++;
    ish_embed_instance_t *inst = s->inst;
    uint32_t sid = s->id;
    int bounded_streaming_controls = s->streaming_deadline_ms != 0;
    pthread_mutex_unlock(&s->lock);
#ifdef ISH_EMBED_TESTING
    extern void ish_embed_test_after_session_control_admission(uint8_t type);
    ish_embed_test_after_session_control_admission(type);
#endif
    int rc = bounded_streaming_controls && type == ISH_FT_TERMINATE
        ? send_lifecycle_frame_async(
            inst, type, 0, sid, payload, payload_len)
        : send_frame(inst, type, 0, sid, payload, payload_len);
    pthread_mutex_lock(&s->lock);
    s->active_controls--;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int ish_embed_session_signal(ish_embed_session_t *s, int signum) {
    if (!s) return ISH_ERR_INVALID_ARG;
    uint8_t buf[4];
    ish_proto_put_i32(buf, signum);
    return send_session_control(s, ISH_FT_SIGNAL, buf, sizeof(buf));
}

int ish_embed_session_resize(ish_embed_session_t *s,
                              uint16_t rows, uint16_t cols,
                              uint16_t xpixel, uint16_t ypixel) {
    if (!s) return ISH_ERR_INVALID_ARG;
    uint8_t buf[8];
    ish_proto_put_u16(buf + 0, rows);
    ish_proto_put_u16(buf + 2, cols);
    ish_proto_put_u16(buf + 4, xpixel);
    ish_proto_put_u16(buf + 6, ypixel);
    return send_session_control(s, ISH_FT_RESIZE, buf, sizeof(buf));
}

int ish_embed_session_terminate(ish_embed_session_t *s, uint32_t grace_ms) {
    if (!s) return ISH_ERR_INVALID_ARG;
    (void)grace_ms; /* supervisor uses its own ~1.5s grace */
    return send_session_control(s, ISH_FT_TERMINATE, NULL, 0);
}

static int session_close_stdin_until(ish_embed_session_t *s,
                                     uint64_t deadline_ms,
                                     int bounded) {
    if (!s) return ISH_ERR_INVALID_ARG;
    if (bounded) {
        int lock_rc = pthread_mutex_trylock(&s->stdin_lock);
        if (lock_rc == EBUSY) return ISH_ERR_BUSY;
        if (lock_rc != 0) return ISH_ERR_THREAD;
    } else {
        pthread_mutex_lock(&s->stdin_lock);
    }
    pthread_mutex_lock(&s->lock);
    if (s->closing) {
        pthread_mutex_unlock(&s->lock);
        pthread_mutex_unlock(&s->stdin_lock);
        return ISH_ERR_NO_SESSION;
    }
    int already = s->stdin_closed;
    s->stdin_closed = 1;
    ish_embed_instance_t *inst = s->inst;
    uint32_t sid = s->id;
    pthread_mutex_unlock(&s->lock);
    if (already) {
        pthread_mutex_unlock(&s->stdin_lock);
        return ISH_OK;
    }
    int rc = bounded
        ? send_frame_async_normal_until(
            inst, ISH_FT_STDIN_CLOSE, 0, sid, NULL, 0,
            deadline_ms)
        : send_frame(inst, ISH_FT_STDIN_CLOSE, 0, sid, NULL, 0);
    if (rc != ISH_OK) {
        pthread_mutex_lock(&s->lock);
        if (!s->closing) s->stdin_closed = 0;
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&s->stdin_lock);
    return rc;
}

int ish_embed_session_close_stdin(ish_embed_session_t *s) {
    if (!s) return ISH_ERR_INVALID_ARG;
    uint64_t deadline_ms = s->streaming_deadline_ms;
    return session_close_stdin_until(s, deadline_ms, deadline_ms != 0);
}

int ish_embed_session_close_stdin_timeout(ish_embed_session_t *s,
                                          uint32_t timeout_ms) {
    if (!s || timeout_ms == 0) return ISH_ERR_INVALID_ARG;
    uint64_t now = now_ms();
    uint64_t call_deadline = now > UINT64_MAX - timeout_ms
        ? UINT64_MAX : now + timeout_ms;
    uint64_t session_deadline = s->streaming_deadline_ms;
    uint64_t deadline_ms =
        session_deadline != 0 && session_deadline < call_deadline
            ? session_deadline : call_deadline;
    return session_close_stdin_until(s, deadline_ms, 1);
}

void ish_embed_session_close(ish_embed_session_t *s) {
    if (!s) return;
    uint64_t close_deadline_ms = now_ms() + 1000;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;

    /* Publish closing without waiting behind synchronous senders. New control
     * calls observe closing under s->lock; an admitted control is counted until
     * its frame completes. New stdin calls take stdin_lock and then observe
     * closing; an existing stdin owner is handled by the EOF fallback below. */
    pthread_mutex_lock(&s->lock);
    if (s->closing || s->owner_released) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    s->closing = 1;
    int exited = s->guest_exited;
    int controls_idle = s->active_controls == 0;
    ish_embed_instance_t *inst = s->inst;
    uint32_t sid = s->id;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    /* Keep the session linked while waiting so the native reader can publish
     * the real EXITED state. SESSION_CLOSE is asynchronous and uses the
     * reserved lifecycle budget: a full/stalled ordinary queue cannot block
     * this void API before its one-second reap deadline begins. Acquiring
     * stdin_lock proves every earlier multi-frame write/EOF completed before
     * SESSION_CLOSE. The active-controls snapshot was taken atomically with
     * publishing closing, so it also proves no signal/resize/terminate can be
     * admitted across this point. Either failed condition must stop the whole
     * control path instead of placing frames out of order. */
    int stdin_serialized = pthread_mutex_trylock(&s->stdin_lock) == 0;
    int close_status = ISH_OK;
    if (stdin_serialized && controls_idle) {
        pthread_mutex_lock(&s->lock);
        exited = s->guest_exited;
        pthread_mutex_unlock(&s->lock);
        if (!exited) {
            close_status = send_lifecycle_frame_async(
                inst, ISH_FT_SESSION_CLOSE, 0, sid, NULL, 0);
        }
        pthread_mutex_unlock(&s->stdin_lock);
    } else if (stdin_serialized) {
        pthread_mutex_unlock(&s->stdin_lock);
    }

    pthread_mutex_lock(&s->lock);
    if (stdin_serialized && controls_idle && close_status == ISH_OK) {
        while (close_status == ISH_OK && !s->guest_exited &&
               !atomic_load(&inst->shutting_down) &&
               now_ms() < close_deadline_ms) {
            int rc = pthread_cond_timedwait(&s->cond, &s->lock, &deadline);
            if (rc == ETIMEDOUT) break;
        }
    }
    exited = s->guest_exited;
    pthread_mutex_unlock(&s->lock);

    /* Never unlink the last host handle while its guest can keep running in an
     * otherwise live instance. A concurrent stdin owner, admission failure,
     * writer failure, or missing reap acknowledgement converts the whole
     * runtime to its protocol-independent EOF shutdown path. */
    if (!stdin_serialized || !controls_idle || !exited) {
        atomic_store(&inst->shutting_down, 1);
        close_control_pipe(inst);
    }

    /* Existing borrowed API calls own references. Keep the session linked
     * until they finish so shutdown cannot free the parent instance out from
     * under an in-flight send/read. New borrows already fail via closing. */
    pthread_mutex_lock(&s->lock);
    while (s->references > 1)
        pthread_cond_wait(&s->cond, &s->lock);
    pthread_mutex_unlock(&s->lock);

    session_unlink(inst, s);
    pthread_mutex_lock(&s->lock);
    s->owner_released = 1;
    pthread_mutex_unlock(&s->lock);
    session_release_reference(s);
}

/* --------------------------------------------------------------- *
 *  run_oneshot                                                    *
 * --------------------------------------------------------------- */

/* A finite-time oneshot must not spend its timeout blocked behind a full
 * control pipe. Its SPAWN and lifecycle frames are admitted asynchronously;
 * the single writer preserves their required wire order. */
static int oneshot_close_stdin_async(ish_embed_session_t *s) {
    pthread_mutex_lock(&s->stdin_lock);
    pthread_mutex_lock(&s->lock);
    if (s->closing) {
        pthread_mutex_unlock(&s->lock);
        pthread_mutex_unlock(&s->stdin_lock);
        return ISH_ERR_NO_SESSION;
    }
    int already = s->stdin_closed;
    s->stdin_closed = 1;
    ish_embed_instance_t *inst = s->inst;
    uint32_t sid = s->id;
    pthread_mutex_unlock(&s->lock);
    if (already) {
        pthread_mutex_unlock(&s->stdin_lock);
        return ISH_OK;
    }
    int rc = send_lifecycle_frame_async(
        inst, ISH_FT_STDIN_CLOSE, 0, sid, NULL, 0);
#ifdef ISH_EMBED_TESTING
    extern void ish_embed_test_oneshot_lifecycle_result(uint8_t type, int rc);
    ish_embed_test_oneshot_lifecycle_result(ISH_FT_STDIN_CLOSE, rc);
#endif
    if (rc != ISH_OK) {
        pthread_mutex_lock(&s->lock);
        if (!s->closing) s->stdin_closed = 0;
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&s->stdin_lock);
    return rc;
}

static int oneshot_send_lifecycle_async(ish_embed_session_t *s, uint8_t type,
                                        const void *payload,
                                        uint32_t payload_len) {
    pthread_mutex_lock(&s->lock);
    if (s->closing) {
        pthread_mutex_unlock(&s->lock);
        return ISH_ERR_NO_SESSION;
    }
    ish_embed_instance_t *inst = s->inst;
    uint32_t sid = s->id;
    pthread_mutex_unlock(&s->lock);
    int rc = send_lifecycle_frame_async(inst, type, 0, sid,
                                        payload, payload_len);
#ifdef ISH_EMBED_TESTING
    extern void ish_embed_test_oneshot_lifecycle_result(uint8_t type, int rc);
    ish_embed_test_oneshot_lifecycle_result(type, rc);
#endif
    return rc;
}

int ish_embed_run_oneshot(ish_embed_instance_t *inst,
                          const ish_embed_spawn_opts_t *opts,
                          ish_embed_oneshot_result_t *out) {
    if (!inst || !opts || !out) return ISH_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* A finite timeout starts at API entry and includes waiting for the spawn
     * staging gate plus SPAWN admission. The finite path admits SPAWN
     * asynchronously; session_close later either observes its ordered reap or
     * stops the whole control direction, so no unowned command can survive. */
    uint64_t deadline = opts->timeout_ms > 0
        ? now_ms() + opts->timeout_ms : 0;
    ish_embed_session_t *s = NULL;
    int rc = deadline != 0
        ? spawn_oneshot_until(inst, opts, &s, deadline)
        : ish_embed_spawn(inst, opts, &s);
    if (rc != 0) return rc;

    size_t out_cap = 0, err_cap = 0;
    uint8_t *out_buf = NULL, *err_buf = NULL;
    size_t out_used = 0, err_used = 0;

    int timed_out = 0, terminate_phase = 0;
    int result_status = ISH_OK;
    int32_t exit_code = -1, signal_v = 0;

    if (deadline != 0 && now_ms() >= deadline) {
        timed_out = 1;
        result_status = ISH_ERR_TIMEOUT;
        goto oneshot_done;
    }

    /* Close stdin immediately for a no-input command, but do not let a stalled
     * writer consume the caller's finite timeout before the read loop begins. */
    rc = oneshot_close_stdin_async(s);
    if (rc != ISH_OK) {
        result_status = rc;
        goto oneshot_done;
    }

    while (1) {
        uint32_t wait;
        if (deadline) {
            uint64_t now = now_ms();
            if (now >= deadline) {
                if (terminate_phase == 0) {
                    int terminate_rc = oneshot_send_lifecycle_async(
                        s, ISH_FT_TERMINATE, NULL, 0);
                    if (terminate_rc != ISH_OK) {
                        result_status = terminate_rc;
                        break;
                    }
                    terminate_phase = 1;
                    timed_out = 1;
                    deadline = now + 2000; /* +2s grace before SIGKILL */
                } else if (terminate_phase == 1) {
                    /* force-kill */
                    uint8_t signal_payload[4];
                    ish_proto_put_i32(signal_payload, 9);
                    int signal_rc = oneshot_send_lifecycle_async(
                        s, ISH_FT_SIGNAL, signal_payload,
                        sizeof(signal_payload));
                    if (signal_rc != ISH_OK) {
                        result_status = signal_rc;
                        break;
                    }
                    terminate_phase = 2;
                    deadline = now + 1000; /* final bounded reap window */
                } else {
                    result_status = ISH_ERR_TIMEOUT;
                    break;
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
        if (rc != ISH_OK) { result_status = rc; break; }
        if (k == ISH_STREAM_STDOUT) {
            if (L > ISH_EMBED_MAX_ONESHOT_STDOUT_BYTES ||
                out_used > ISH_EMBED_MAX_ONESHOT_STDOUT_BYTES - L) {
                ish_embed_free(b);
                int terminate_rc = oneshot_send_lifecycle_async(
                    s, ISH_FT_TERMINATE, NULL, 0);
                result_status = terminate_rc == ISH_OK
                    ? ISH_ERR_OUTPUT_LIMIT : terminate_rc;
                break;
            }
            if (out_used + L > out_cap) {
                size_t nc = out_cap ? out_cap * 2 : 4096;
                while (nc < out_used + L) nc *= 2;
                if (nc > ISH_EMBED_MAX_ONESHOT_STDOUT_BYTES)
                    nc = ISH_EMBED_MAX_ONESHOT_STDOUT_BYTES;
                uint8_t *nb = (uint8_t *)realloc(out_buf, nc);
                if (!nb) { ish_embed_free(b); result_status = ISH_ERR_OOM; break; }
                out_buf = nb; out_cap = nc;
            }
            if (L) memcpy(out_buf + out_used, b, L);
            out_used += L;
            ish_embed_free(b);
        } else if (k == ISH_STREAM_STDERR) {
            if (L > ISH_EMBED_MAX_ONESHOT_STDERR_BYTES ||
                err_used > ISH_EMBED_MAX_ONESHOT_STDERR_BYTES - L) {
                ish_embed_free(b);
                int terminate_rc = oneshot_send_lifecycle_async(
                    s, ISH_FT_TERMINATE, NULL, 0);
                result_status = terminate_rc == ISH_OK
                    ? ISH_ERR_OUTPUT_LIMIT : terminate_rc;
                break;
            }
            if (err_used + L > err_cap) {
                size_t nc = err_cap ? err_cap * 2 : 4096;
                while (nc < err_used + L) nc *= 2;
                if (nc > ISH_EMBED_MAX_ONESHOT_STDERR_BYTES)
                    nc = ISH_EMBED_MAX_ONESHOT_STDERR_BYTES;
                uint8_t *nb = (uint8_t *)realloc(err_buf, nc);
                if (!nb) { ish_embed_free(b); result_status = ISH_ERR_OOM; break; }
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

oneshot_done:
    out->exit_code   = exit_code;
    out->signal      = signal_v;
    out->stdout_buf  = out_buf;
    out->stdout_len  = out_used;
    out->stderr_buf  = err_buf;
    out->stderr_len  = err_used;
    out->timed_out   = timed_out;
    ish_embed_session_close(s);
    if (result_status != ISH_OK) {
        free(out_buf);
        free(err_buf);
        memset(out, 0, sizeof(*out));
        return result_status;
    }
    return ISH_OK;
}

static int parse_guest_errno_output(const uint8_t *buf, size_t len,
                                    int32_t *out_guest_errno) {
    if (!buf || !out_guest_errno || len < 2 || len > 11 ||
        buf[len - 1] != '\n')
        return ISH_ERR_PROTOCOL;
    uint32_t value = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        uint8_t ch = buf[i];
        if (ch < '0' || ch > '9') return ISH_ERR_PROTOCOL;
        uint32_t digit = (uint32_t)(ch - '0');
        if (value > (uint32_t)INT32_MAX / 10u ||
            (value == (uint32_t)INT32_MAX / 10u &&
             digit > (uint32_t)INT32_MAX % 10u))
            return ISH_ERR_PROTOCOL;
        value = value * 10u + digit;
    }
    *out_guest_errno = (int32_t)value;
    return ISH_OK;
}

int ish_embed_rename_noreplace(ish_embed_instance_t *inst,
                               const char *source,
                               const char *destination,
                               uint32_t timeout_ms,
                               int32_t *out_guest_errno) {
    if (!inst || !source || !destination || !out_guest_errno ||
        source[0] != '/' || source[1] == '\0' ||
        destination[0] != '/' || destination[1] == '\0')
        return ISH_ERR_INVALID_ARG;
    *out_guest_errno = 0;

    uint64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    int gate = instance_call_begin_until(inst, deadline);
    if (gate != ISH_OK) return gate;
    char *helper_path = inst->supervisor_guest_path
        ? strdup(inst->supervisor_guest_path) : NULL;
    if (!helper_path) {
        instance_call_end(inst);
        return ISH_ERR_OOM;
    }

    uint32_t remaining_ms = 0;
    if (deadline != 0) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            free(helper_path);
            instance_call_end(inst);
            return ISH_ERR_TIMEOUT;
        }
        uint64_t remaining = deadline - now;
        remaining_ms = remaining > UINT32_MAX
            ? UINT32_MAX : (uint32_t)remaining;
        if (remaining_ms == 0) {
            free(helper_path);
            instance_call_end(inst);
            return ISH_ERR_TIMEOUT;
        }
    }

    const char *argv[] = {
        helper_path,
        "--rename-noreplace",
        source,
        destination,
        NULL,
    };
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.cwd = "/";
    opts.timeout_ms = remaining_ms;
    ish_embed_oneshot_result_t result;
    int rc = ish_embed_run_oneshot(inst, &opts, &result);
    free(helper_path);
    if (rc != ISH_OK) {
        instance_call_end(inst);
        return rc;
    }

    if (result.timed_out) {
        rc = ISH_ERR_TIMEOUT;
    } else if (result.signal != 0 || result.exit_code != 0) {
        rc = ISH_ERR_SUPERVISOR;
    } else {
        rc = parse_guest_errno_output(result.stdout_buf, result.stdout_len,
                                      out_guest_errno);
    }
    ish_embed_free(result.stdout_buf);
    ish_embed_free(result.stderr_buf);
    instance_call_end(inst);
    return rc;
}

void ish_embed_free(void *p) { free(p); }

int ish_embed_setup_vm_root(ish_embed_instance_t *inst, const char *vm_root) {
    if (!inst || !vm_root || vm_root[0] != '/') return ISH_ERR_INVALID_ARG;
    int gate = instance_call_begin(inst);
    if (gate != ISH_OK) return gate;
#ifdef ISH_EMBED_TESTING
    extern void ish_embed_test_after_instance_call_begin(void);
    ish_embed_test_after_instance_call_begin();
#endif
    /* ABI-compatible validation entry point. Runtime filesystem operations
     * cannot safely execute on an arbitrary host thread because iSH binds
     * `current` to the kernel pthread. PID 1 therefore provisions /dev,
     * devpts and procfs inside an absolute chroot_path immediately before
     * its first SPAWN. Keeping this symbol as a no-op preserves callers built
     * against the original ABI without re-entering TLS-bound kernel code. */
    instance_call_end(inst);
    return ISH_OK;
}

/* --------------------------------------------------------------- *
 *  shutdown                                                       *
 * --------------------------------------------------------------- */

int ish_embed_shutdown(ish_embed_instance_t *inst, uint32_t grace_ms) {
    if (!inst) return ISH_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_instance_lock);
    if (g_instance != inst) { pthread_mutex_unlock(&g_instance_lock); return ISH_ERR_INVALID_ARG; }
    /* Keep the singleton gate for the complete transition. A second shutdown
     * blocks here and, after success, observes g_instance == NULL without
     * ever dereferencing the released pointer. */

    /* New raw-instance calls cannot pass instance_call_begin while the
     * singleton gate is held. Refuse shutdown if one was already admitted;
     * the caller can retry after that operation finishes. */
    pthread_mutex_lock(&inst->lifecycle_lock);
    unsigned active_calls = inst->active_calls;
    pthread_mutex_unlock(&inst->lifecycle_lock);
    if (active_calls != 0) {
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_BUSY;
    }

    /* Freeze new spawns atomically with proving the session list is empty. */
    pthread_mutex_lock(&inst->sess_lock);
    if (inst->sessions_head) {
        pthread_mutex_unlock(&inst->sess_lock);
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_BUSY;
    }
    atomic_store(&inst->shutting_down, 1);
    pthread_mutex_unlock(&inst->sess_lock);

    /* Do not make the shutdown caller wait for a full control pipe. The
     * writer pump will send this in order while the reader keeps draining;
     * the grace deadline below remains effective even for a broken peer. */
    int shutdown_frame_status = send_lifecycle_frame_async(
        inst, ISH_FT_SHUTDOWN, 0, 0, NULL, 0);
    if (shutdown_frame_status != ISH_OK)
        close_control_pipe(inst);

    uint32_t polite_wait = grace_ms ? grace_ms : 5000;
    if (!wait_for_kernel_exit(inst, polite_wait)) {
        /* EOF is the supervisor's second, protocol-independent shutdown
         * path. halt_system itself has a bounded 10-second reap loop. */
        close_control_pipe(inst);
        if (!wait_for_kernel_exit(inst, 15000)) {
            pthread_mutex_unlock(&g_instance_lock);
            return ISH_ERR_TIMEOUT;
        }
    }
    close_control_pipe(inst);
    if (join_instance_threads(inst) != ISH_OK) {
        pthread_mutex_unlock(&g_instance_lock);
        return ISH_ERR_THREAD;
    }
    close_fd_slot(&inst->guest_to_host_r);
    close_fd_slot(&inst->guest_log_r);
    close_fd_slot(&inst->kernel_log_fd);
    ish_ffi_register_exit_hook(NULL, NULL);
    destroy_instance_primitives(inst);

    g_instance = NULL;
    free(inst->supervisor_guest_path);
    free(inst);
    pthread_mutex_unlock(&g_instance_lock);
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
        case ISH_ERR_OUTPUT_LIMIT: return "native session output backlog exceeded";
        case ISH_ERR_BUSY: return "operation cannot proceed while runtime state is busy";
        case ISH_ERR_SUPERVISOR_INSTALL: return "bundled supervisor installation failed";
        case ISH_ERR_CONTROL_LIMIT: return "host control queue limit reached";
        case ISH_ERR_UNSUPPORTED: return "feature unavailable in linked native binary";
        case ISH_ERR_INTERNAL: return "internal error";
        default: return "unknown";
    }
}

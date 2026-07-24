/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Deterministic host lifecycle tests for libishembed. A tiny fake FFI owns
 * the guest pipe ends and behaves like the PID 1 supervisor, so these tests
 * exercise host threading and queue invariants without booting iSH.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ishembed.h"
#include "../ffi/ish_ffi.h"
#include "../protocol/proto.h"

enum fake_mode {
    FAKE_BOOT_TIMEOUT,
    FAKE_CLOSE_RACE,
    FAKE_BACKLOG,
    FAKE_BACKLOG_FRAMES,
    FAKE_BACKLOG_CONTROL_PRESSURE,
    FAKE_BORROW_HOLD,
    FAKE_ONESHOT_HANG,
    FAKE_ONESHOT_OUTPUT,
    FAKE_SHUTDOWN_DRAIN,
    FAKE_LOG_BACKPRESSURE,
    FAKE_DOUBLE_SHUTDOWN,
    FAKE_BROKEN_CONTROL,
    FAKE_ACTIVE_CALL_HOLD,
    FAKE_PROTOCOL_FATAL,
    FAKE_PROTOCOL_FATAL_CONTROL_PRESSURE,
    FAKE_BAD_HELLO_ACK,
    FAKE_INSTALL_FAILURE,
    FAKE_BUNDLED_DIGEST_MISMATCH,
    FAKE_BUNDLED_PATH_MISMATCH,
    FAKE_CUSTOM_SUPERVISOR,
    FAKE_STDIN_CLOSE_ORDER,
    FAKE_CONTROL_FRAME_LIMIT,
    FAKE_CONTROL_BYTE_LIMIT,
    FAKE_CONTROL_SPAWN_GATE,
    FAKE_CONTROL_CRITICAL_CLOSE,
    FAKE_CONTROL_CRITICAL_ONESHOT,
    FAKE_CONTROL_SAME_SESSION_CLOSE,
    FAKE_CONTROL_EXITED_SAME_SESSION_CLOSE,
    FAKE_SESSION_CONTROL_CLOSE_RACE,
    FAKE_CONTROL_PREBLOCKED_ONESHOT,
    FAKE_CONTROL_BYTE_RESERVE,
    FAKE_SUPERVISOR_ERROR,
    FAKE_MALFORMED_EVENT,
    FAKE_OUTPUT_ALLOCATION_FAILURE,
    FAKE_SPAWN_ARGUMENT_BOUND,
    FAKE_WRITER_LOCK_HOLD,
    FAKE_WRITER_PRECOMMIT_DEADLINE,
};

enum malformed_event_kind {
    MALFORMED_SPAWNED,
    MALFORMED_STDOUT,
    MALFORMED_STDERR,
    MALFORMED_EXITED,
    MALFORMED_SHUTDOWN_ACK,
    MALFORMED_PONG,
    MALFORMED_UNKNOWN,
};

const uint8_t ish_embed_bundled_supervisor[] = {0x7f, 'E', 'L', 'F'};
const size_t ish_embed_bundled_supervisor_len =
    sizeof(ish_embed_bundled_supervisor);
const char ish_embed_bundled_supervisor_sha256[] =
    "3bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359";
const char ish_embed_bundled_supervisor_guest_path[] =
    "/sbin/.ishsv-ishembed-sha256-3bdbb4fe8397cd2b842430b39ccff01a8663c751945ef5e9a09e267fb8b1d359";

static enum fake_mode g_mode;
static atomic_int g_control_r = -1;
static int g_events_w = -1;
static int g_log_w = -1;
static pthread_t g_kernel_thread;
static int g_kernel_started;
static ish_ffi_exit_cb g_exit_cb;
static void *g_exit_ctx;
static pthread_mutex_t g_fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fake_cond = PTHREAD_COND_INITIALIZER;
static int g_burst_done;
static int g_borrow_waiting;
static int g_release_borrow;
static int g_close_done;
static int g_shutdown_seen;
static int g_release_shutdown;
static int g_shutdown_callers_ready;
static int g_release_shutdown_callers;
static int g_control_closed;
static int g_release_control;
static int g_active_call_waiting;
static int g_release_active_call;
static int g_terminate_count;
static int g_signal_count;
static int g_resize_count;
static int g_session_close_count;
static uint8_t g_session_control_race_type;
static int g_session_control_admitted;
static int g_release_session_control;
static int g_log_sink_fd = -1;
static int g_log_sink_read_fd = -1;
static int g_log_write_started;
static int g_stdin_header_seen;
static int g_start_pressure_events;
static int g_pressure_done;
static int g_pressure_protocol_ok;
static int g_event_epipe_seen;
static int g_pressure_watchdog_fired;
static int g_pressure_writer_done;
static int g_install_count;
static char g_exec_path[256];
static int g_stdin_first_data_seen;
static int g_release_stdin_drain;
static int g_stdin_close_call_done;
static int g_stdin_close_seen;
static int g_stdin_close_order_ok;
static size_t g_stdin_bytes_seen;
static int g_spawn_count;
static int g_control_reader_blocked;
static int g_release_control_reader;
static int g_abort_blocked_control;
static int g_writer_lock_held;
static int g_release_writer_lock;
static int g_oneshot_close_stdin_attempts;
static int g_oneshot_close_stdin_status;
static int g_oneshot_terminate_attempts;
static int g_oneshot_terminate_status;
static int g_oneshot_signal_attempts;
static int g_oneshot_signal_status;
static enum malformed_event_kind g_malformed_event_kind;

static int io_write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int io_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int fake_emit(uint8_t type, uint8_t flags, uint32_t sid,
                     const void *body, uint32_t body_len) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, type, flags, body_len, sid);
    if (io_write_full(g_events_w, hdr, sizeof(hdr)) < 0) return -1;
    if (body_len && io_write_full(g_events_w, body, body_len) < 0) return -1;
    return 0;
}

static int fake_read(uint8_t *type, uint8_t *flags, uint32_t *sid,
                     uint8_t **body, uint32_t *body_len) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    *body = NULL;
    *body_len = 0;
    int control_fd = atomic_load(&g_control_r);
    if (control_fd < 0 || io_read_full(control_fd, hdr, sizeof(hdr)) < 0) return -1;
    if (ish_proto_parse_hdr(hdr, type, flags, body_len, sid) < 0) return -1;
    if (*body_len) {
        *body = malloc(*body_len);
        if (!*body) return -1;
        if (io_read_full(control_fd, *body, *body_len) < 0) {
            free(*body);
            *body = NULL;
            return -1;
        }
    }
    return 0;
}

static void fake_emit_exit(uint32_t sid, int32_t code, int32_t sig) {
    uint8_t body[8];
    ish_proto_put_i32(body, code);
    ish_proto_put_i32(body + 4, sig);
    (void)fake_emit(ISH_FT_EXITED, 0, sid, body, sizeof(body));
}

static void fake_emit_error(uint32_t sid, int32_t err, const char *message) {
    size_t message_len = strlen(message);
    uint8_t body[8 + 64];
    if (message_len > 64) abort();
    ish_proto_put_i32(body, err);
    ish_proto_put_u32(body + 4, (uint32_t)message_len);
    memcpy(body + 8, message, message_len);
    (void)fake_emit(ISH_FT_ERROR, 0, sid, body,
                    (uint32_t)(8 + message_len));
}

static void fake_emit_malformed_event(uint32_t sid) {
    uint8_t body[16] = {0};
    ish_proto_put_u64(body, 1);
    switch (g_malformed_event_kind) {
        case MALFORMED_SPAWNED:
            (void)fake_emit(ISH_FT_SPAWNED, 0, sid, NULL, 0);
            break;
        case MALFORMED_STDOUT:
            (void)fake_emit(ISH_FT_STDOUT_DATA, 0, sid, body, 1);
            break;
        case MALFORMED_STDERR:
            (void)fake_emit(ISH_FT_STDERR_DATA, ISH_FF_SEQ_PRESENT,
                            sid, body, 7);
            break;
        case MALFORMED_EXITED:
            (void)fake_emit(ISH_FT_EXITED, 0, sid, body, 4);
            break;
        case MALFORMED_SHUTDOWN_ACK:
            (void)fake_emit(ISH_FT_SHUTDOWN_ACK, 0, sid, NULL, 0);
            break;
        case MALFORMED_PONG:
            (void)fake_emit(ISH_FT_PONG, 0, 0, body, 1);
            break;
        case MALFORMED_UNKNOWN:
            (void)fake_emit(0x99, 0, 0, NULL, 0);
            break;
    }
}

static void fake_emit_close_race(uint32_t sid) {
    uint8_t body[8 + 64];
    for (uint64_t seq = 1; seq <= 8; seq++) {
        ish_proto_put_u64(body, seq);
        memset(body + 8, (int)('a' + seq), sizeof(body) - 8);
        if (fake_emit(ISH_FT_STDOUT_DATA, ISH_FF_SEQ_PRESENT, sid,
                      body, sizeof(body)) < 0)
            break;
        sched_yield();
    }
    fake_emit_exit(sid, 0, 0);
}

static void fake_emit_backlog(uint32_t sid) {
    const size_t payload_len = 64 * 1024;
    uint8_t *body = malloc(8 + payload_len);
    if (!body) abort();
    memset(body + 8, 'x', payload_len);
    for (uint64_t seq = 1; seq <= 70; seq++) {
        ish_proto_put_u64(body, seq);
        if (fake_emit(ISH_FT_STDOUT_DATA, ISH_FF_SEQ_PRESENT, sid,
                      body, (uint32_t)(8 + payload_len)) < 0)
            break;
    }
    free(body);
    fake_emit_exit(sid, 0, 0);
    pthread_mutex_lock(&g_fake_lock);
    g_burst_done = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
}

static void fake_emit_frame_backlog(uint32_t sid) {
    uint8_t body[9];
    body[8] = 'f';
    for (uint64_t seq = 1; seq <= ISH_EMBED_MAX_SESSION_BACKLOG_FRAMES + 4; seq++) {
        ish_proto_put_u64(body, seq);
        if (fake_emit(ISH_FT_STDOUT_DATA, ISH_FF_SEQ_PRESENT, sid,
                      body, sizeof(body)) < 0)
            break;
    }
    fake_emit_exit(sid, 0, 0);
}

static void fake_emit_oneshot_output(uint32_t sid) {
    const size_t payload_len = 16 * 1024;
    uint8_t *body = malloc(8 + payload_len);
    if (!body) abort();
    memset(body + 8, 'o', payload_len);
    uint64_t frames = (ISH_EMBED_MAX_ONESHOT_STDOUT_BYTES / payload_len) + 8;
    for (uint64_t seq = 1; seq <= frames; seq++) {
        ish_proto_put_u64(body, seq);
        if (fake_emit(ISH_FT_STDOUT_DATA, ISH_FF_SEQ_PRESENT, sid,
                      body, (uint32_t)(8 + payload_len)) < 0)
            break;
        usleep(100);
    }
    free(body);
    fake_emit_exit(sid, 0, 0);
}

static int fake_emit_protocol_fatal(void) {
    const uint32_t payload_len = ISH_EMBED_MAX_PROTOCOL_FRAME_BYTES + 65536;
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    uint8_t chunk[16384] = {0};
    ish_proto_pack_hdr(hdr, ISH_FT_STDOUT_DATA, 0, payload_len, 1);
    if (io_write_full(g_events_w, hdr, sizeof(hdr)) < 0)
        return errno == EPIPE;
    uint32_t remaining = payload_len;
    while (remaining > 0) {
        size_t n = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        if (io_write_full(g_events_w, chunk, n) < 0)
            return errno == EPIPE;
        remaining -= (uint32_t)n;
    }
    return 0;
}

static void fake_emit_shutdown_drain(void) {
    const size_t payload_len = 64 * 1024;
    uint8_t *body = calloc(1, payload_len);
    if (!body) abort();
    for (int i = 0; i < 80; i++) {
        if (fake_emit(ISH_FT_LOG, 0, 0, body, (uint32_t)payload_len) < 0)
            break;
    }
    for (int i = 0; i < 32; i++) {
        if (io_write_full(g_log_w, body, payload_len) < 0) break;
    }
    free(body);
}

/* Leave the first STDIN_DATA payload unread so the host control writer is
 * genuinely backpressured. The fake then drives the opposite event direction
 * to prove that the host reader never waits on the control pipe. */
static void fake_run_control_pressure(uint32_t sid) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    uint8_t type = 0, flags = 0;
    uint32_t body_len = 0, body_sid = 0;
    int control_fd = atomic_load(&g_control_r);
    int ok = control_fd >= 0 &&
        io_read_full(control_fd, hdr, sizeof(hdr)) == 0 &&
        ish_proto_parse_hdr(hdr, &type, &flags, &body_len, &body_sid) == 0 &&
        type == ISH_FT_STDIN_DATA && body_sid == sid && body_len == 65536;

    pthread_mutex_lock(&g_fake_lock);
    g_pressure_protocol_ok = ok;
    g_stdin_header_seen = 1;
    pthread_cond_broadcast(&g_fake_cond);
    while (!g_start_pressure_events)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);

    int event_epipe = 0;
    if (ok && g_mode == FAKE_BACKLOG_CONTROL_PRESSURE) {
        fake_emit_backlog(sid);
    } else if (ok && g_mode == FAKE_PROTOCOL_FATAL_CONTROL_PRESSURE) {
        event_epipe = fake_emit_protocol_fatal();
    }

    /* The partial stdin frame is intentionally abandoned. EOF is the only
     * valid way to terminate a framed stream after a partial payload. */
    control_fd = atomic_exchange(&g_control_r, -1);
    if (control_fd >= 0) close(control_fd);

    pthread_mutex_lock(&g_fake_lock);
    g_event_epipe_seen = event_epipe;
    g_pressure_done = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
}

static void fake_finish_kernel(void) {
    int control_fd = atomic_exchange(&g_control_r, -1);
    if (control_fd >= 0) close(control_fd);
    if (g_events_w >= 0) { close(g_events_w); g_events_w = -1; }
    if (g_log_w >= 0) { close(g_log_w); g_log_w = -1; }
    if (g_exit_cb) g_exit_cb(0, g_exit_ctx);
}

static void *fake_kernel_main(void *unused) {
    (void)unused;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    uint8_t type = 0, flags = 0;
    uint32_t sid = 0, body_len = 0;
    uint8_t *body = NULL;

    if (fake_read(&type, &flags, &sid, &body, &body_len) < 0) {
        fake_finish_kernel();
        return NULL;
    }
    free(body);
    if (type != ISH_FT_HELLO) {
        fake_finish_kernel();
        return NULL;
    }

    if (g_mode == FAKE_BOOT_TIMEOUT) {
        uint8_t byte;
        int control_fd = atomic_load(&g_control_r);
        while (control_fd >= 0 && read(control_fd, &byte, 1) > 0) {}
        usleep(50 * 1000);
        fake_finish_kernel();
        return NULL;
    }

    uint8_t ack[12] = {0};
    ish_proto_put_u32(ack, g_mode == FAKE_BAD_HELLO_ACK
                               ? ISH_EMBED_ABI_VERSION + 1
                               : ISH_EMBED_ABI_VERSION);
    ack[4] = ISH_PROTO_VERSION;
    ish_proto_put_u32(ack + 8, 64);
    if (fake_emit(ISH_FT_HELLO_ACK, 0, 0, ack, sizeof(ack)) < 0) {
        fake_finish_kernel();
        return NULL;
    }
    if (g_mode == FAKE_LOG_BACKPRESSURE) {
        uint8_t log_chunk[4096] = {0};
        if (io_write_full(g_log_w, log_chunk, sizeof(log_chunk)) < 0) {
            fake_finish_kernel();
            return NULL;
        }
    }

    while (fake_read(&type, &flags, &sid, &body, &body_len) == 0) {
        free(body);
        body = NULL;
        if (type == ISH_FT_SPAWN) {
            if (g_mode == FAKE_CONTROL_EXITED_SAME_SESSION_CLOSE)
                (void)fake_emit_exit(sid, 0, 0);
            if (g_mode == FAKE_CONTROL_FRAME_LIMIT ||
                g_mode == FAKE_CONTROL_BYTE_LIMIT ||
                g_mode == FAKE_CONTROL_SPAWN_GATE ||
                g_mode == FAKE_CONTROL_CRITICAL_CLOSE ||
                g_mode == FAKE_CONTROL_CRITICAL_ONESHOT ||
                g_mode == FAKE_CONTROL_SAME_SESSION_CLOSE ||
                g_mode == FAKE_CONTROL_EXITED_SAME_SESSION_CLOSE ||
                g_mode == FAKE_CONTROL_PREBLOCKED_ONESHOT ||
                g_mode == FAKE_CONTROL_BYTE_RESERVE) {
                int block_after = 1;
                if (g_mode == FAKE_CONTROL_BYTE_LIMIT ||
                    g_mode == FAKE_CONTROL_CRITICAL_CLOSE ||
                    g_mode == FAKE_CONTROL_CRITICAL_ONESHOT)
                    block_after = 2;
                else if (g_mode == FAKE_CONTROL_BYTE_RESERVE)
                    block_after = 3;
                pthread_mutex_lock(&g_fake_lock);
                g_spawn_count++;
                if (g_spawn_count == block_after) {
                    g_control_reader_blocked = 1;
                    pthread_cond_broadcast(&g_fake_cond);
                    while (!g_release_control_reader)
                        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
                    int abort_control = g_abort_blocked_control;
                    pthread_mutex_unlock(&g_fake_lock);
                    if (abort_control) {
                        int control_fd = atomic_exchange(&g_control_r, -1);
                        if (control_fd >= 0) close(control_fd);
                        fake_finish_kernel();
                        return NULL;
                    }
                } else {
                    pthread_mutex_unlock(&g_fake_lock);
                }
            }
            if (g_mode == FAKE_BACKLOG_CONTROL_PRESSURE ||
                g_mode == FAKE_PROTOCOL_FATAL_CONTROL_PRESSURE) {
                fake_run_control_pressure(sid);
                fake_finish_kernel();
                return NULL;
            }
            if (g_mode == FAKE_CLOSE_RACE) fake_emit_close_race(sid);
            if (g_mode == FAKE_BACKLOG) fake_emit_backlog(sid);
            if (g_mode == FAKE_BACKLOG_FRAMES) fake_emit_frame_backlog(sid);
            if (g_mode == FAKE_ONESHOT_OUTPUT) fake_emit_oneshot_output(sid);
            if (g_mode == FAKE_PROTOCOL_FATAL) (void)fake_emit_protocol_fatal();
            if (g_mode == FAKE_SUPERVISOR_ERROR)
                fake_emit_error(sid, ENOBUFS, "stdin queue full");
            if (g_mode == FAKE_MALFORMED_EVENT) {
                fake_emit_malformed_event(sid);
                fake_finish_kernel();
                return NULL;
            }
            if (g_mode == FAKE_OUTPUT_ALLOCATION_FAILURE) {
                uint8_t output[9];
                ish_proto_put_u64(output, 1);
                output[8] = 'x';
                (void)fake_emit(ISH_FT_STDOUT_DATA, ISH_FF_SEQ_PRESENT,
                                sid, output, sizeof(output));
                fake_finish_kernel();
                return NULL;
            }
            if (g_mode == FAKE_BROKEN_CONTROL) {
                int control_fd = atomic_exchange(&g_control_r, -1);
                if (control_fd >= 0) close(control_fd);
                pthread_mutex_lock(&g_fake_lock);
                g_control_closed = 1;
                pthread_cond_broadcast(&g_fake_cond);
                while (!g_release_control)
                    pthread_cond_wait(&g_fake_cond, &g_fake_lock);
                pthread_mutex_unlock(&g_fake_lock);
                fake_finish_kernel();
                return NULL;
            }
        } else if (type == ISH_FT_STDIN_DATA &&
                   g_mode == FAKE_STDIN_CLOSE_ORDER) {
            pthread_mutex_lock(&g_fake_lock);
            g_stdin_bytes_seen += body_len;
            if (!g_stdin_first_data_seen) {
                g_stdin_first_data_seen = 1;
                pthread_cond_broadcast(&g_fake_cond);
                while (!g_release_stdin_drain)
                    pthread_cond_wait(&g_fake_cond, &g_fake_lock);
            }
            pthread_mutex_unlock(&g_fake_lock);
        } else if (type == ISH_FT_STDIN_CLOSE &&
                   g_mode == FAKE_STDIN_CLOSE_ORDER) {
            pthread_mutex_lock(&g_fake_lock);
            g_stdin_close_order_ok =
                g_stdin_bytes_seen == 16u * 1024u * 1024u;
            g_stdin_close_seen = 1;
            pthread_cond_broadcast(&g_fake_cond);
            pthread_mutex_unlock(&g_fake_lock);
            fake_emit_exit(sid, 0, 0);
        } else if (type == ISH_FT_SIGNAL || type == ISH_FT_RESIZE ||
                   type == ISH_FT_TERMINATE ||
                   type == ISH_FT_SESSION_CLOSE) {
            pthread_mutex_lock(&g_fake_lock);
            if (type == ISH_FT_SIGNAL) g_signal_count++;
            else if (type == ISH_FT_RESIZE) g_resize_count++;
            else if (type == ISH_FT_TERMINATE) g_terminate_count++;
            else g_session_close_count++;
            if (g_mode == FAKE_BACKLOG_FRAMES &&
                type == ISH_FT_SESSION_CLOSE)
                g_burst_done = 1;
            pthread_cond_broadcast(&g_fake_cond);
            pthread_mutex_unlock(&g_fake_lock);
            if (type == ISH_FT_SESSION_CLOSE ||
                (type != ISH_FT_RESIZE && g_mode != FAKE_ONESHOT_HANG))
                fake_emit_exit(sid, 137, 9);
        } else if (type == ISH_FT_SHUTDOWN) {
            if (g_mode == FAKE_DOUBLE_SHUTDOWN) {
                pthread_mutex_lock(&g_fake_lock);
                g_shutdown_seen = 1;
                pthread_cond_broadcast(&g_fake_cond);
                while (!g_release_shutdown)
                    pthread_cond_wait(&g_fake_cond, &g_fake_lock);
                pthread_mutex_unlock(&g_fake_lock);
            }
            if (g_mode == FAKE_SHUTDOWN_DRAIN ||
                g_mode == FAKE_LOG_BACKPRESSURE)
                fake_emit_shutdown_drain();
            (void)fake_emit(ISH_FT_SHUTDOWN_ACK, 0, 0, NULL, 0);
            break;
        }
    }
    free(body);
    fake_finish_kernel();
    return NULL;
}

/* ---- Fake FFI ------------------------------------------------------- */

int ish_ffi_mount_fakefs(const char *path) { return path ? 0 : -EINVAL; }
int ish_ffi_become_init(void) { return 0; }
int ish_ffi_install_pipe_stdio(int in_rd, int out_wr_a, int out_wr_b) {
    atomic_store(&g_control_r, in_rd);
    g_events_w = out_wr_a;
    g_log_w = out_wr_b;
#ifdef F_SETNOSIGPIPE
    (void)fcntl(g_events_w, F_SETNOSIGPIPE, 1);
    (void)fcntl(g_log_w, F_SETNOSIGPIPE, 1);
#endif
    return 0;
}
int ish_ffi_chdir(const char *path) { return path ? 0 : -EINVAL; }
int ish_ffi_create_devices(void) { return 0; }
int ish_ffi_install_executable(const char *path, const uint8_t *bytes,
                               size_t len, uint32_t mode) {
    g_install_count++;
    if (!path || strcmp(path, ish_embed_bundled_supervisor_guest_path) != 0 ||
        bytes != ish_embed_bundled_supervisor ||
        len != ish_embed_bundled_supervisor_len || mode != 0755)
        return -EINVAL;
    if (g_mode == FAKE_INSTALL_FAILURE) return -EIO;
    return 0;
}
int ish_ffi_execve(const char *path, size_t argc, const char *argv,
                   const char *envp) {
    (void)argc; (void)argv; (void)envp;
    if (path) snprintf(g_exec_path, sizeof(g_exec_path), "%s", path);
    return path ? 0 : -EINVAL;
}
int ish_ffi_task_start(void) {
    int rc = pthread_create(&g_kernel_thread, NULL, fake_kernel_main, NULL);
    if (rc == 0) g_kernel_started = 1;
    return rc == 0 ? 0 : -rc;
}
int ish_ffi_task_join(void) {
    if (!g_kernel_started) return -EINVAL;
    int rc = pthread_join(g_kernel_thread, NULL);
    if (rc == 0) g_kernel_started = 0;
    return rc == 0 ? 0 : -rc;
}
void ish_ffi_register_exit_hook(ish_ffi_exit_cb cb, void *ctx) {
    g_exit_cb = cb;
    g_exit_ctx = ctx;
}

/* Test-only hook compiled into host/ishembed.c with ISH_EMBED_TESTING. It
 * deterministically holds an admitted raw-instance call so shutdown's
 * active-call lease can be exercised without invoking TLS-bound iSH APIs. */
void ish_embed_test_after_instance_call_begin(void) {
    if (g_mode != FAKE_ACTIVE_CALL_HOLD) return;
    pthread_mutex_lock(&g_fake_lock);
    g_active_call_waiting = 1;
    pthread_cond_broadcast(&g_fake_cond);
    while (!g_release_active_call)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);
}

void ish_embed_test_after_session_control_admission(uint8_t type) {
    if (g_mode != FAKE_SESSION_CONTROL_CLOSE_RACE ||
        type != g_session_control_race_type)
        return;
    pthread_mutex_lock(&g_fake_lock);
    g_session_control_admitted = 1;
    pthread_cond_broadcast(&g_fake_cond);
    while (!g_release_session_control)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);
}

void ish_embed_test_after_writer_lock(uint8_t type) {
    if (g_mode == FAKE_WRITER_PRECOMMIT_DEADLINE &&
        type == ISH_FT_SPAWN) {
        usleep(150 * 1000);
        return;
    }
    if (g_mode != FAKE_WRITER_LOCK_HOLD || type != ISH_FT_RESIZE)
        return;
    pthread_mutex_lock(&g_fake_lock);
    g_writer_lock_held = 1;
    pthread_cond_broadcast(&g_fake_cond);
    while (!g_release_writer_lock)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);
}

int ish_embed_test_fail_output_allocation(void) {
    return g_mode == FAKE_OUTPUT_ALLOCATION_FAILURE;
}

void ish_embed_test_log_write_begin(void) {
    if (g_mode != FAKE_LOG_BACKPRESSURE) return;
    pthread_mutex_lock(&g_fake_lock);
    g_log_write_started = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
}

void ish_embed_test_oneshot_lifecycle_result(uint8_t type, int rc) {
    pthread_mutex_lock(&g_fake_lock);
    if (type == ISH_FT_STDIN_CLOSE) {
        g_oneshot_close_stdin_attempts++;
        g_oneshot_close_stdin_status = rc;
    } else if (type == ISH_FT_TERMINATE) {
        g_oneshot_terminate_attempts++;
        g_oneshot_terminate_status = rc;
    } else if (type == ISH_FT_SIGNAL) {
        g_oneshot_signal_attempts++;
        g_oneshot_signal_status = rc;
    }
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
}

void ish_embed_test_bundled_supervisor_metadata(const char **sha256,
                                                const char **guest_path) {
    static const char bad_sha256[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    static const char bad_guest_path[] =
        "/sbin/.ishsv-ishembed-sha256-0000000000000000000000000000000000000000000000000000000000000000";
    if (g_mode == FAKE_BUNDLED_DIGEST_MISMATCH)
        *sha256 = bad_sha256;
    else if (g_mode == FAKE_BUNDLED_PATH_MISMATCH)
        *guest_path = bad_guest_path;
}

void ish_embed_test_control_usage(ish_embed_instance_t *inst,
                                  size_t *out_bytes, size_t *out_frames);

/* ---- Tests ---------------------------------------------------------- */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int wait_fake_flag(int *flag, uint32_t timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }
    pthread_mutex_lock(&g_fake_lock);
    while (!*flag) {
        int rc = pthread_cond_timedwait(&g_fake_cond, &g_fake_lock, &deadline);
        if (rc == ETIMEDOUT) break;
    }
    int ready = *flag;
    pthread_mutex_unlock(&g_fake_lock);
    return ready;
}

static int wait_pressure_with_watchdog(void) {
    if (wait_fake_flag(&g_pressure_done, 6000)) return 1;
    pthread_mutex_lock(&g_fake_lock);
    g_pressure_watchdog_fired = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    int control_fd = atomic_exchange(&g_control_r, -1);
    if (control_fd >= 0) close(control_fd);
    (void)wait_fake_flag(&g_pressure_done, 2000);
    return 0;
}

static ish_embed_instance_t *boot_instance(void) {
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.kernel_log_fd = -1;
    if (g_mode == FAKE_SHUTDOWN_DRAIN) {
        g_log_sink_fd = open("/dev/null", O_WRONLY);
        if (g_log_sink_fd < 0) {
            perror("open /dev/null");
            exit(1);
        }
        opts.kernel_log_fd = g_log_sink_fd;
    } else if (g_mode == FAKE_LOG_BACKPRESSURE) {
        int sink[2];
        if (pipe(sink) < 0) {
            perror("pipe log sink");
            exit(1);
        }
        int flags = fcntl(sink[1], F_GETFL, 0);
        if (flags < 0 || fcntl(sink[1], F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("nonblocking log sink");
            exit(1);
        }
        uint8_t fill[4096] = {0};
        for (;;) {
            ssize_t wrote = write(sink[1], fill, sizeof(fill));
            if (wrote > 0) continue;
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            perror("fill log sink");
            exit(1);
        }
        if (fcntl(sink[1], F_SETFL, flags) < 0) {
            perror("restore blocking log sink");
            exit(1);
        }
        g_log_sink_read_fd = sink[0];
        g_log_sink_fd = sink[1];
        opts.kernel_log_fd = g_log_sink_fd;
    }
    ish_embed_instance_t *inst = NULL;
    int rc = ish_embed_boot(&opts, &inst);
    if (rc != ISH_OK || !inst) {
        fprintf(stderr, "boot failed: rc=%d\n", rc);
        exit(1);
    }
    if (g_install_count != 1 ||
        strcmp(g_exec_path, ish_embed_bundled_supervisor_guest_path) != 0) {
        fprintf(stderr, "bundled supervisor was not installed/executed\n");
        exit(1);
    }
    return inst;
}

static ish_embed_session_t *spawn_echo(ish_embed_instance_t *inst) {
    const char *argv[] = {"/bin/echo", "x", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    ish_embed_session_t *session = NULL;
    int rc = ish_embed_spawn(inst, &opts, &session);
    if (rc != ISH_OK || !session) {
        fprintf(stderr, "spawn failed: rc=%d\n", rc);
        exit(1);
    }
    return session;
}

static int test_boot_timeout_cleanup(void) {
    g_mode = FAKE_BOOT_TIMEOUT;
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.kernel_log_fd = -1;
    ish_embed_instance_t *inst = NULL;
    uint64_t start = monotonic_ms();
    int rc = ish_embed_boot(&opts, &inst);
    uint64_t elapsed = monotonic_ms() - start;
    if (rc != ISH_ERR_TIMEOUT || inst != NULL || elapsed > 2000) {
        fprintf(stderr, "boot-timeout: rc=%d inst=%p elapsed=%llums\n",
                rc, (void *)inst, (unsigned long long)elapsed);
        return 1;
    }
    fprintf(stderr, "boot-timeout cleanup: OK (%llums)\n",
            (unsigned long long)elapsed);
    return 0;
}

static int test_bad_hello_ack(void) {
    g_mode = FAKE_BAD_HELLO_ACK;
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.kernel_log_fd = -1;
    ish_embed_instance_t *inst = NULL;
    uint64_t start = monotonic_ms();
    int rc = ish_embed_boot(&opts, &inst);
    uint64_t elapsed = monotonic_ms() - start;
    if (rc != ISH_ERR_PROTOCOL || inst != NULL || elapsed > 2000) {
        fprintf(stderr, "bad-hello-ack: rc=%d inst=%p elapsed=%llums\n",
                rc, (void *)inst, (unsigned long long)elapsed);
        return 1;
    }
    fprintf(stderr, "incompatible HELLO_ACK rejected: OK\n");
    return 0;
}

static int test_install_failure(void) {
    g_mode = FAKE_INSTALL_FAILURE;
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.kernel_log_fd = -1;
    ish_embed_instance_t *inst = NULL;
    int rc = ish_embed_boot(&opts, &inst);
    if (rc != ISH_ERR_SUPERVISOR_INSTALL || inst != NULL ||
        g_install_count != 1 || g_exec_path[0] != '\0') {
        fprintf(stderr, "install-failure: rc=%d inst=%p installs=%d exec=%s\n",
                rc, (void *)inst, g_install_count, g_exec_path);
        return 1;
    }
    fprintf(stderr, "supervisor install failure mapped: OK\n");
    return 0;
}

static int test_bundled_supervisor_metadata_mismatch(enum fake_mode mode,
                                                     const char *label) {
    g_mode = mode;
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.kernel_log_fd = -1;
    ish_embed_instance_t *inst = NULL;
    int rc = ish_embed_boot(&opts, &inst);
    if (rc != ISH_ERR_SUPERVISOR_INSTALL || inst != NULL ||
        g_install_count != 0 || g_exec_path[0] != '\0') {
        fprintf(stderr,
                "%s: rc=%d inst=%p installs=%d exec=%s\n",
                label, rc, (void *)inst, g_install_count, g_exec_path);
        return 1;
    }
    fprintf(stderr, "%s rejected before install: OK\n", label);
    return 0;
}

struct pressure_write_arg {
    ish_embed_session_t *session;
    uint8_t *bytes;
    size_t len;
    int rc;
};

static void *pressure_write_thread(void *raw) {
    struct pressure_write_arg *arg = raw;
    arg->rc = ish_embed_session_write(arg->session, arg->bytes, arg->len);
    ish_embed_session_release(arg->session);
    pthread_mutex_lock(&g_fake_lock);
    g_pressure_writer_done = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    return NULL;
}

static int start_pressure_writer(ish_embed_session_t *session,
                                 struct pressure_write_arg *arg,
                                 pthread_t *thread) {
    const size_t input_len = 16u * 1024u * 1024u;
    uint8_t *bytes = (uint8_t *)malloc(input_len);
    if (!bytes) return -1;
    memset(bytes, 'i', input_len);
    if (ish_embed_session_retain(session) != ISH_OK) {
        free(bytes);
        return -1;
    }
    *arg = (struct pressure_write_arg) {
        .session = session,
        .bytes = bytes,
        .len = input_len,
        .rc = 123,
    };
    if (pthread_create(thread, NULL, pressure_write_thread, arg) != 0) {
        ish_embed_session_release(session);
        free(bytes);
        return -1;
    }
    return 0;
}

enum bounded_queue_call_kind {
    BOUNDED_QUEUE_WRITE,
    BOUNDED_QUEUE_RESIZE,
};

struct bounded_queue_call {
    ish_embed_session_t *session;
    enum bounded_queue_call_kind kind;
    const uint8_t *bytes;
    size_t len;
    atomic_int done;
    int rc;
};

static void *bounded_queue_call_thread(void *raw) {
    struct bounded_queue_call *call = raw;
    if (call->kind == BOUNDED_QUEUE_WRITE) {
        call->rc = ish_embed_session_write(call->session,
                                           call->bytes, call->len);
    } else {
        call->rc = ish_embed_session_resize(call->session, 24, 80, 0, 0);
    }
    ish_embed_session_release(call->session);
    atomic_store(&call->done, 1);
    return NULL;
}

static int start_bounded_queue_call(struct bounded_queue_call *call,
                                    pthread_t *thread,
                                    ish_embed_session_t *session,
                                    enum bounded_queue_call_kind kind,
                                    const uint8_t *bytes, size_t len) {
    if (ish_embed_session_retain(session) != ISH_OK) return -1;
    *call = (struct bounded_queue_call) {
        .session = session,
        .kind = kind,
        .bytes = bytes,
        .len = len,
        .rc = 123,
    };
    atomic_init(&call->done, 0);
    if (pthread_create(thread, NULL, bounded_queue_call_thread, call) != 0) {
        ish_embed_session_release(session);
        return -1;
    }
    return 0;
}

static int wait_bounded_calls(struct bounded_queue_call *calls, size_t count,
                              size_t wanted, uint32_t timeout_ms) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    do {
        size_t done = 0;
        for (size_t i = 0; i < count; i++)
            if (atomic_load(&calls[i].done)) done++;
        if (done >= wanted) return 1;
        usleep(1000);
    } while (monotonic_ms() < deadline);
    return 0;
}

static int wait_control_frame_usage(ish_embed_instance_t *inst,
                                    size_t wanted_frames,
                                    uint32_t timeout_ms) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    do {
        size_t frames = 0;
        ish_embed_test_control_usage(inst, NULL, &frames);
        if (frames == wanted_frames) return 1;
        usleep(1000);
    } while (monotonic_ms() < deadline);
    return 0;
}

static int wait_control_usage(ish_embed_instance_t *inst,
                              size_t wanted_bytes, size_t wanted_frames,
                              uint32_t timeout_ms) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    do {
        size_t bytes = 0, frames = 0;
        ish_embed_test_control_usage(inst, &bytes, &frames);
        if (bytes == wanted_bytes && frames == wanted_frames) return 1;
        usleep(1000);
    } while (monotonic_ms() < deadline);
    return 0;
}

struct close_stdin_arg {
    ish_embed_session_t *session;
    int rc;
};

static void *close_stdin_thread(void *raw) {
    struct close_stdin_arg *arg = raw;
    arg->rc = ish_embed_session_close_stdin(arg->session);
    ish_embed_session_release(arg->session);
    pthread_mutex_lock(&g_fake_lock);
    g_stdin_close_call_done = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    return NULL;
}

static int test_stdin_close_order(void) {
    g_mode = FAKE_STDIN_CLOSE_ORDER;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    struct pressure_write_arg writer;
    pthread_t writer_thread;
    if (start_pressure_writer(session, &writer, &writer_thread) != 0) return 1;
    if (!wait_fake_flag(&g_stdin_first_data_seen, 3000)) {
        fprintf(stderr, "stdin close order: first DATA not observed\n");
        return 1;
    }

    if (ish_embed_session_retain(session) != ISH_OK) return 1;
    struct close_stdin_arg closer = {.session = session, .rc = 123};
    pthread_t close_thread;
    if (pthread_create(&close_thread, NULL, close_stdin_thread, &closer) != 0)
        return 1;
    int close_finished_while_blocked =
        wait_fake_flag(&g_stdin_close_call_done, 100);

    pthread_mutex_lock(&g_fake_lock);
    g_release_stdin_drain = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    pthread_join(writer_thread, NULL);
    pthread_join(close_thread, NULL);
    free(writer.bytes);
    if (!wait_fake_flag(&g_stdin_close_seen, 3000)) {
        fprintf(stderr, "stdin close order: CLOSE not observed\n");
        return 1;
    }

    pthread_mutex_lock(&g_fake_lock);
    int order_ok = g_stdin_close_order_ok;
    size_t bytes_seen = g_stdin_bytes_seen;
    pthread_mutex_unlock(&g_fake_lock);
    int ok = !close_finished_while_blocked && writer.rc == ISH_OK &&
        closer.rc == ISH_OK && order_ok;
    if (!ok) {
        fprintf(stderr,
                "stdin close order: writer=%d close=%d early=%d order=%d bytes=%zu\n",
                writer.rc, closer.rc, close_finished_while_blocked,
                order_ok, bytes_seen);
    }
    ish_embed_session_close(session);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok) fprintf(stderr, "multi-frame stdin precedes CLOSE atomically: OK\n");
    return ok ? 0 : 1;
}

static void release_blocked_control_reader(int abort_control) {
    pthread_mutex_lock(&g_fake_lock);
    g_abort_blocked_control = abort_control;
    g_release_control_reader = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
}

struct bounded_close_call {
    ish_embed_session_t *session;
    atomic_int done;
    uint64_t elapsed_ms;
};

static void *bounded_close_thread(void *raw) {
    struct bounded_close_call *call = raw;
    uint64_t start = monotonic_ms();
    ish_embed_session_close(call->session);
    call->elapsed_ms = monotonic_ms() - start;
    atomic_store(&call->done, 1);
    return NULL;
}

static int test_control_frame_queue_limit(void) {
    enum { resize_call_count = 8 };
    g_mode = FAKE_CONTROL_FRAME_LIMIT;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "control frame limit: fake reader did not block\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'q', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, session,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr, "control frame limit: blocker did not remain in flight\n");
        return 1;
    }

    struct bounded_queue_call calls[resize_call_count];
    pthread_t threads[resize_call_count];
    for (size_t i = 0; i < resize_call_count; i++) {
        if (start_bounded_queue_call(&calls[i], &threads[i], session,
                                     BOUNDED_QUEUE_RESIZE, NULL, 0) != 0)
            return 1;
    }
    size_t normal_frame_ceiling = ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES -
        ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES;
    size_t expected_rejected =
        1u + resize_call_count - normal_frame_ceiling;
    if (!wait_bounded_calls(calls, resize_call_count,
                            expected_rejected, 2000) ||
        !wait_control_frame_usage(inst,
                                  normal_frame_ceiling, 2000)) {
        fprintf(stderr, "control frame limit: ceiling was not reached\n");
        return 1;
    }

    /* Close the peer instead of draining it. This forces the current frame and
     * every queued frame through writer stop/drain accounting. */
    release_blocked_control_reader(1);
    pthread_join(blocker_thread, NULL);
    for (size_t i = 0; i < resize_call_count; i++)
        pthread_join(threads[i], NULL);

    size_t limited = 0;
    size_t cancelled = 0;
    for (size_t i = 0; i < resize_call_count; i++) {
        if (calls[i].rc == ISH_ERR_CONTROL_LIMIT) limited++;
        else if (calls[i].rc == ISH_ERR_BROKEN_PIPE ||
                 calls[i].rc == ISH_ERR_NOT_RUNNING) cancelled++;
    }
    if (blocker.rc == ISH_ERR_BROKEN_PIPE ||
        blocker.rc == ISH_ERR_NOT_RUNNING)
        cancelled++;

    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int ok = limited == expected_rejected &&
        cancelled == normal_frame_ceiling &&
        bytes == 0 && frames == 0;
    if (!ok) {
        fprintf(stderr,
                "control frame limit: limited=%zu cancelled=%zu usage=%zu/%zu\n",
                limited, cancelled, bytes, frames);
    }

    free(input);
    ish_embed_session_close(session);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok) fprintf(stderr, "bounded control frame queue stop/drain: OK\n");
    return ok ? 0 : 1;
}

static int test_control_critical_close_reserve(void) {
    enum { resize_call_count = 6 };
    g_mode = FAKE_CONTROL_CRITICAL_CLOSE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *victim = spawn_echo(inst);
    ish_embed_session_t *pressure = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "critical close: fake reader did not block\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'c', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, pressure,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr, "critical close: blocker did not remain in flight\n");
        return 1;
    }

    size_t normal_frame_ceiling = ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES -
        ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES;
    if (normal_frame_ceiling <= 1) {
        fprintf(stderr, "critical close: test normal frame budget too small\n");
        return 1;
    }
    struct bounded_queue_call calls[resize_call_count];
    pthread_t threads[resize_call_count];
    for (size_t i = 0; i < resize_call_count; i++) {
        if (start_bounded_queue_call(&calls[i], &threads[i], pressure,
                                     BOUNDED_QUEUE_RESIZE, NULL, 0) != 0)
            return 1;
    }
    size_t expected_limited =
        1u + resize_call_count - normal_frame_ceiling;
    if (!wait_bounded_calls(calls, resize_call_count,
                            expected_limited, 2000) ||
        !wait_control_frame_usage(inst, normal_frame_ceiling, 2000)) {
        fprintf(stderr, "critical close: normal budget did not saturate\n");
        return 1;
    }

    struct bounded_close_call close_call = {
        .session = victim,
        .elapsed_ms = 0,
    };
    atomic_init(&close_call.done, 0);
    pthread_t close_thread;
    if (pthread_create(&close_thread, NULL,
                       bounded_close_thread, &close_call) != 0)
        return 1;

    if (!wait_control_frame_usage(inst,
                                  ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES, 800) ||
        atomic_load(&close_call.done)) {
        fprintf(stderr,
                "critical close: reserved async frame was not accounted\n");
        return 1;
    }
    size_t peak_bytes = 0, peak_frames = 0;
    ish_embed_test_control_usage(inst, &peak_bytes, &peak_frames);
    if (peak_bytes > ISH_EMBED_MAX_CONTROL_QUEUE_BYTES ||
        peak_frames != ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES) {
        fprintf(stderr, "critical close: peak usage=%zu/%zu\n",
                peak_bytes, peak_frames);
        return 1;
    }

    uint64_t close_deadline = monotonic_ms() + 2500;
    while (!atomic_load(&close_call.done) &&
           monotonic_ms() < close_deadline)
        usleep(1000);
    if (!atomic_load(&close_call.done)) {
        fprintf(stderr, "critical close: bounded fallback did not return\n");
        return 1;
    }
    pthread_join(close_thread, NULL);
    pthread_join(blocker_thread, NULL);
    for (size_t i = 0; i < resize_call_count; i++)
        pthread_join(threads[i], NULL);

    size_t limited = 0, cancelled = 0;
    for (size_t i = 0; i < resize_call_count; i++) {
        if (calls[i].rc == ISH_ERR_CONTROL_LIMIT) limited++;
        else if (calls[i].rc == ISH_ERR_BROKEN_PIPE ||
                 calls[i].rc == ISH_ERR_NOT_RUNNING) cancelled++;
    }
    if (blocker.rc == ISH_ERR_BROKEN_PIPE ||
        blocker.rc == ISH_ERR_NOT_RUNNING)
        cancelled++;
    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(inst, "/srv/vms/after-close");
    int ok = limited == expected_limited &&
        cancelled == normal_frame_ceiling &&
        close_call.elapsed_ms <= 2000 &&
        bytes == 0 && frames == 0 && probe_rc == ISH_ERR_NOT_RUNNING;
    if (!ok) {
        fprintf(stderr,
                "critical close: limited=%zu cancelled=%zu elapsed=%llums "
                "usage=%zu/%zu probe=%d\n",
                limited, cancelled,
                (unsigned long long)close_call.elapsed_ms,
                bytes, frames, probe_rc);
    }

    free(input);
    release_blocked_control_reader(1);
    ish_embed_session_close(pressure);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "reserved async close stayed bounded and drained: OK\n");
    return ok ? 0 : 1;
}

static int run_control_same_session_close_bound(enum fake_mode mode,
                                                int guest_already_exited) {
    const char *label = guest_already_exited
        ? "exited same-session close" : "same-session close";
    g_mode = mode;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "%s: fake reader did not block\n", label);
        return 1;
    }
    if (guest_already_exited) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int kind = 0;
        uint64_t seq = 0;
        int32_t exit_code = -1, signal_v = -1;
        int read_rc = ish_embed_session_read(session, 2000, &buf, &len,
                                             &kind, &seq,
                                             &exit_code, &signal_v);
        ish_embed_free(buf);
        if (read_rc != ISH_OK || kind != ISH_STREAM_EXITED ||
            exit_code != 0 || signal_v != 0) {
            fprintf(stderr,
                    "%s: terminal state was not published "
                    "(%d/%d/%d/%d)\n",
                    label, read_rc, kind, exit_code, signal_v);
            return 1;
        }
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'w', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, session,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr,
                "%s: retained writer did not stay blocked\n", label);
        return 1;
    }

    struct bounded_close_call close_call = {
        .session = session,
        .elapsed_ms = 0,
    };
    atomic_init(&close_call.done, 0);
    pthread_t close_thread;
    if (pthread_create(&close_thread, NULL,
                       bounded_close_thread, &close_call) != 0)
        return 1;

    uint64_t close_deadline = monotonic_ms() + 2000;
    while (!atomic_load(&close_call.done) &&
           monotonic_ms() < close_deadline)
        usleep(1000);
    if (!atomic_load(&close_call.done)) {
        fprintf(stderr,
                "%s: stdin owner trapped close fallback\n", label);
        return 1;
    }
    pthread_join(close_thread, NULL);
    pthread_join(blocker_thread, NULL);

    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(inst,
                                           "/srv/vms/after-same-close");
    int writer_cancelled = blocker.rc == ISH_ERR_BROKEN_PIPE ||
        blocker.rc == ISH_ERR_NOT_RUNNING;
    int ok = writer_cancelled && close_call.elapsed_ms <= 1500 &&
        bytes == 0 && frames == 0 && probe_rc == ISH_ERR_NOT_RUNNING;
    if (!ok) {
        fprintf(stderr,
                "%s: writer=%d elapsed=%llums "
                "usage=%zu/%zu probe=%d\n",
                label, blocker.rc,
                (unsigned long long)close_call.elapsed_ms,
                bytes, frames, probe_rc);
    }

    free(input);
    release_blocked_control_reader(1);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "%s retained writer cannot trap close: OK\n", label);
    return ok ? 0 : 1;
}

static int test_control_same_session_close_bound(void) {
    return run_control_same_session_close_bound(
        FAKE_CONTROL_SAME_SESSION_CLOSE, 0);
}

static int test_control_exited_same_session_close_bound(void) {
    return run_control_same_session_close_bound(
        FAKE_CONTROL_EXITED_SAME_SESSION_CLOSE, 1);
}

enum retained_control_kind {
    RETAINED_CONTROL_SIGNAL,
    RETAINED_CONTROL_RESIZE,
    RETAINED_CONTROL_TERMINATE,
};

struct retained_control_call {
    ish_embed_session_t *session;
    enum retained_control_kind kind;
    atomic_int done;
    int rc;
};

static void *retained_control_thread(void *raw) {
    struct retained_control_call *call = raw;
    if (call->kind == RETAINED_CONTROL_SIGNAL)
        call->rc = ish_embed_session_signal(call->session, 2);
    else if (call->kind == RETAINED_CONTROL_RESIZE)
        call->rc = ish_embed_session_resize(call->session, 24, 80, 0, 0);
    else
        call->rc = ish_embed_session_terminate(call->session, 500);
    ish_embed_session_release(call->session);
    atomic_store(&call->done, 1);
    return NULL;
}

static int wait_session_closing(ish_embed_session_t *session,
                                uint32_t timeout_ms) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    do {
        int rc = ish_embed_session_retain(session);
        if (rc == ISH_ERR_NO_SESSION) return 1;
        if (rc != ISH_OK) return 0;
        ish_embed_session_release(session);
        usleep(1000);
    } while (monotonic_ms() < deadline);
    return 0;
}

static int test_session_control_close_order(enum retained_control_kind kind,
                                            uint8_t type,
                                            const char *label) {
    g_mode = FAKE_SESSION_CONTROL_CLOSE_RACE;
    g_session_control_race_type = type;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    if (ish_embed_session_retain(session) != ISH_OK) return 1;

    struct retained_control_call control = {
        .session = session,
        .kind = kind,
        .rc = 123,
    };
    atomic_init(&control.done, 0);
    pthread_t control_thread;
    if (pthread_create(&control_thread, NULL,
                       retained_control_thread, &control) != 0)
        return 1;
    if (!wait_fake_flag(&g_session_control_admitted, 2000)) {
        fprintf(stderr, "%s/close: control was not admitted\n", label);
        return 1;
    }

    struct bounded_close_call close_call = {
        .session = session,
        .elapsed_ms = 0,
    };
    atomic_init(&close_call.done, 0);
    pthread_t close_thread;
    if (pthread_create(&close_thread, NULL,
                       bounded_close_thread, &close_call) != 0)
        return 1;
    if (!wait_session_closing(session, 1000)) {
        fprintf(stderr, "%s/close: close did not publish closing\n", label);
        return 1;
    }

    pthread_mutex_lock(&g_fake_lock);
    g_release_session_control = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    pthread_join(control_thread, NULL);
    pthread_join(close_thread, NULL);

    int *delivered_flag = type == ISH_FT_SIGNAL ? &g_signal_count :
        type == ISH_FT_RESIZE ? &g_resize_count : &g_terminate_count;
    if (control.rc == ISH_OK)
        (void)wait_fake_flag(delivered_flag, 1000);
    pthread_mutex_lock(&g_fake_lock);
    int delivered = type == ISH_FT_SIGNAL ? g_signal_count :
        type == ISH_FT_RESIZE ? g_resize_count : g_terminate_count;
    int close_delivered = g_session_close_count;
    pthread_mutex_unlock(&g_fake_lock);
    int probe_rc = ish_embed_setup_vm_root(inst,
                                           "/srv/vms/after-control-close");
    int rejected = control.rc == ISH_ERR_BROKEN_PIPE ||
        control.rc == ISH_ERR_NOT_RUNNING;
    int ordered = (rejected && delivered == 0) ||
        (control.rc == ISH_OK && delivered == 1);
    int ok = ordered && close_delivered == 0 &&
        close_call.elapsed_ms <= 1500 &&
        probe_rc == ISH_ERR_NOT_RUNNING;
    if (!ok) {
        fprintf(stderr,
                "%s/close: control=%d delivered=%d close=%d "
                "elapsed=%llums probe=%d\n",
                label, control.rc, delivered, close_delivered,
                (unsigned long long)close_call.elapsed_ms, probe_rc);
    }
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr, "%s cannot follow SESSION_CLOSE: OK\n", label);
    return ok ? 0 : 1;
}

struct bounded_oneshot_call {
    ish_embed_instance_t *inst;
    ish_embed_spawn_opts_t opts;
    ish_embed_oneshot_result_t result;
    atomic_int done;
    uint64_t elapsed_ms;
    int rc;
};

static void *bounded_oneshot_thread(void *raw) {
    struct bounded_oneshot_call *call = raw;
    uint64_t start = monotonic_ms();
    call->rc = ish_embed_run_oneshot(call->inst, &call->opts,
                                     &call->result);
    call->elapsed_ms = monotonic_ms() - start;
    atomic_store(&call->done, 1);
    return NULL;
}

static int test_control_critical_oneshot_bound(void) {
    enum { resize_call_count = 6 };
    g_mode = FAKE_CONTROL_CRITICAL_ONESHOT;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *pressure = spawn_echo(inst);

    const char *oneshot_argv[] = {"/bin/sleep", "forever", NULL};
    struct bounded_oneshot_call oneshot = {
        .inst = inst,
        .opts = {
            .argv = oneshot_argv,
            .timeout_ms = 1000,
        },
        .rc = 123,
    };
    atomic_init(&oneshot.done, 0);
    pthread_t oneshot_thread;
    if (pthread_create(&oneshot_thread, NULL,
                       bounded_oneshot_thread, &oneshot) != 0)
        return 1;
    if (!wait_fake_flag(&g_control_reader_blocked, 2000) ||
        !wait_fake_flag(&g_oneshot_close_stdin_attempts, 2000)) {
        fprintf(stderr, "critical oneshot: startup lifecycle did not run\n");
        return 1;
    }
    pthread_mutex_lock(&g_fake_lock);
    int close_stdin_status = g_oneshot_close_stdin_status;
    pthread_mutex_unlock(&g_fake_lock);
    if (close_stdin_status != ISH_OK ||
        !wait_control_frame_usage(inst, 0, 2000) ||
        atomic_load(&oneshot.done)) {
        fprintf(stderr,
                "critical oneshot: close-stdin=%d or command ended early\n",
                close_stdin_status);
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'o', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, pressure,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr, "critical oneshot: blocker did not remain in flight\n");
        return 1;
    }

    size_t normal_frame_ceiling = ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES -
        ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES;
    struct bounded_queue_call calls[resize_call_count];
    pthread_t threads[resize_call_count];
    for (size_t i = 0; i < resize_call_count; i++) {
        if (start_bounded_queue_call(&calls[i], &threads[i], pressure,
                                     BOUNDED_QUEUE_RESIZE, NULL, 0) != 0)
            return 1;
    }
    size_t expected_limited =
        1u + resize_call_count - normal_frame_ceiling;
    if (!wait_bounded_calls(calls, resize_call_count,
                            expected_limited, 2000) ||
        !wait_control_frame_usage(inst, normal_frame_ceiling, 2000)) {
        fprintf(stderr, "critical oneshot: normal budget did not saturate\n");
        return 1;
    }

    if (!wait_fake_flag(&g_oneshot_terminate_attempts, 2000) ||
        !wait_control_frame_usage(inst,
                                  ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES, 1000)) {
        fprintf(stderr,
                "critical oneshot: async terminate did not use reserve\n");
        return 1;
    }
    uint64_t done_deadline = monotonic_ms() + 3500;
    while (!atomic_load(&oneshot.done) && monotonic_ms() < done_deadline)
        usleep(1000);
    if (!atomic_load(&oneshot.done)) {
        fprintf(stderr,
                "critical oneshot: finite timeout blocked on control write\n");
        return 1;
    }
    pthread_join(oneshot_thread, NULL);
    pthread_join(blocker_thread, NULL);
    for (size_t i = 0; i < resize_call_count; i++)
        pthread_join(threads[i], NULL);

    pthread_mutex_lock(&g_fake_lock);
    int terminate_attempts = g_oneshot_terminate_attempts;
    int terminate_status = g_oneshot_terminate_status;
    int signal_attempts = g_oneshot_signal_attempts;
    int signal_status = g_oneshot_signal_status;
    pthread_mutex_unlock(&g_fake_lock);
    size_t limited = 0, cancelled = 0;
    for (size_t i = 0; i < resize_call_count; i++) {
        if (calls[i].rc == ISH_ERR_CONTROL_LIMIT) limited++;
        else if (calls[i].rc == ISH_ERR_BROKEN_PIPE ||
                 calls[i].rc == ISH_ERR_NOT_RUNNING) cancelled++;
    }
    if (blocker.rc == ISH_ERR_BROKEN_PIPE ||
        blocker.rc == ISH_ERR_NOT_RUNNING)
        cancelled++;
    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(inst, "/srv/vms/after-oneshot");
    int ok = oneshot.rc == ISH_ERR_CONTROL_LIMIT &&
        oneshot.elapsed_ms <= 4500 &&
        oneshot.result.stdout_buf == NULL &&
        oneshot.result.stderr_buf == NULL &&
        oneshot.result.stdout_len == 0 &&
        oneshot.result.stderr_len == 0 &&
        terminate_attempts == 1 && terminate_status == ISH_OK &&
        signal_attempts == 1 && signal_status == ISH_ERR_CONTROL_LIMIT &&
        limited == expected_limited &&
        cancelled == normal_frame_ceiling &&
        bytes == 0 && frames == 0 && probe_rc == ISH_ERR_NOT_RUNNING;
    if (!ok) {
        fprintf(stderr,
                "critical oneshot: rc=%d elapsed=%llums term=%d/%d "
                "signal=%d/%d limited=%zu cancelled=%zu "
                "usage=%zu/%zu probe=%d\n",
                oneshot.rc, (unsigned long long)oneshot.elapsed_ms,
                terminate_attempts, terminate_status,
                signal_attempts, signal_status,
                limited, cancelled,
                bytes, frames, probe_rc);
    }

    free(input);
    release_blocked_control_reader(1);
    ish_embed_session_close(pressure);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "finite oneshot lifecycle stayed bounded under saturation: OK\n");
    return ok ? 0 : 1;
}

static int test_control_preblocked_oneshot_spawn(void) {
    g_mode = FAKE_CONTROL_PREBLOCKED_ONESHOT;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *pressure = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "preblocked oneshot: fake reader did not block\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'p', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, pressure,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr, "preblocked oneshot: ordinary frame did not stall\n");
        return 1;
    }

    const char *oneshot_argv[] = {"/bin/sleep", "forever", NULL};
    struct bounded_oneshot_call oneshot = {
        .inst = inst,
        .opts = {
            .argv = oneshot_argv,
            .timeout_ms = 500,
        },
        .rc = 123,
    };
    atomic_init(&oneshot.done, 0);
    pthread_t oneshot_thread;
    if (pthread_create(&oneshot_thread, NULL,
                       bounded_oneshot_thread, &oneshot) != 0)
        return 1;

    if (!wait_fake_flag(&g_oneshot_close_stdin_attempts, 1000)) {
        fprintf(stderr,
                "preblocked oneshot: SPAWN waited for the stalled pipe\n");
        return 1;
    }
    pthread_mutex_lock(&g_fake_lock);
    int close_stdin_status = g_oneshot_close_stdin_status;
    pthread_mutex_unlock(&g_fake_lock);
    if (close_stdin_status != ISH_OK ||
        !wait_control_frame_usage(inst, 3, 500) ||
        atomic_load(&oneshot.done)) {
        fprintf(stderr,
                "preblocked oneshot: async SPAWN/EOF admission failed (%d)\n",
                close_stdin_status);
        return 1;
    }

    if (!wait_fake_flag(&g_oneshot_terminate_attempts, 1200) ||
        !wait_control_frame_usage(inst,
                                  ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES, 500)) {
        fprintf(stderr,
                "preblocked oneshot: entry deadline did not reach terminate\n");
        return 1;
    }
    uint64_t done_deadline = monotonic_ms() + 3000;
    while (!atomic_load(&oneshot.done) && monotonic_ms() < done_deadline)
        usleep(1000);
    if (!atomic_load(&oneshot.done)) {
        fprintf(stderr,
                "preblocked oneshot: finite call remained stuck in SPAWN\n");
        return 1;
    }
    pthread_join(oneshot_thread, NULL);
    pthread_join(blocker_thread, NULL);

    pthread_mutex_lock(&g_fake_lock);
    int terminate_attempts = g_oneshot_terminate_attempts;
    int terminate_status = g_oneshot_terminate_status;
    int signal_attempts = g_oneshot_signal_attempts;
    int signal_status = g_oneshot_signal_status;
    pthread_mutex_unlock(&g_fake_lock);
    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(inst,
                                           "/srv/vms/after-preblocked-spawn");
    int writer_cancelled = blocker.rc == ISH_ERR_BROKEN_PIPE ||
        blocker.rc == ISH_ERR_NOT_RUNNING;
    int ok = oneshot.rc == ISH_ERR_CONTROL_LIMIT &&
        oneshot.elapsed_ms <= 4000 &&
        oneshot.result.stdout_buf == NULL &&
        oneshot.result.stderr_buf == NULL &&
        oneshot.result.stdout_len == 0 &&
        oneshot.result.stderr_len == 0 &&
        terminate_attempts == 1 && terminate_status == ISH_OK &&
        signal_attempts == 1 && signal_status == ISH_ERR_CONTROL_LIMIT &&
        writer_cancelled && bytes == 0 && frames == 0 &&
        probe_rc == ISH_ERR_NOT_RUNNING;
    if (!ok) {
        fprintf(stderr,
                "preblocked oneshot: rc=%d elapsed=%llums close=%d "
                "term=%d/%d signal=%d/%d writer=%d usage=%zu/%zu probe=%d\n",
                oneshot.rc, (unsigned long long)oneshot.elapsed_ms,
                close_stdin_status, terminate_attempts, terminate_status,
                signal_attempts, signal_status, blocker.rc,
                bytes, frames, probe_rc);
    }

    free(input);
    release_blocked_control_reader(1);
    ish_embed_session_close(pressure);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "oneshot deadline covers preblocked SPAWN safely: OK\n");
    return ok ? 0 : 1;
}

static int test_control_byte_queue_limit(void) {
    g_mode = FAKE_CONTROL_BYTE_LIMIT;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *sessions[2] = {
        spawn_echo(inst),
        spawn_echo(inst),
    };
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "control byte limit: fake reader did not block\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'b', 65536u);
    struct bounded_queue_call calls[2];
    pthread_t threads[2];
    for (size_t i = 0; i < 2; i++) {
        if (start_bounded_queue_call(&calls[i], &threads[i], sessions[i],
                                     BOUNDED_QUEUE_WRITE,
                                     input, 65536u) != 0)
            return 1;
    }
    if (!wait_bounded_calls(calls, 2, 1, 2000) ||
        !wait_control_frame_usage(inst, 1, 2000)) {
        fprintf(stderr, "control byte limit: byte ceiling was not reached\n");
        return 1;
    }
    size_t bytes = 0, frames = 0;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    size_t expected_wire_bytes = ISH_PROTO_HDR_SIZE + 65536u;
    if (bytes != expected_wire_bytes || frames != 1) {
        fprintf(stderr, "control byte limit: usage=%zu/%zu expected=%zu/1\n",
                bytes, frames, expected_wire_bytes);
        return 1;
    }

    release_blocked_control_reader(0);
    for (size_t i = 0; i < 2; i++) pthread_join(threads[i], NULL);
    size_t succeeded = 0, limited = 0;
    for (size_t i = 0; i < 2; i++) {
        if (calls[i].rc == ISH_OK) succeeded++;
        if (calls[i].rc == ISH_ERR_CONTROL_LIMIT) limited++;
    }
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_session_resize(sessions[0], 24, 80, 0, 0);
    int ok = succeeded == 1 && limited == 1 && bytes == 0 && frames == 0 &&
        probe_rc == ISH_OK;
    if (!ok) {
        fprintf(stderr,
                "control byte limit: ok=%zu limited=%zu usage=%zu/%zu probe=%d\n",
                succeeded, limited, bytes, frames, probe_rc);
    }

    free(input);
    for (size_t i = 0; i < 2; i++) ish_embed_session_close(sessions[i]);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok) fprintf(stderr, "bounded control byte queue finish/reuse: OK\n");
    return ok ? 0 : 1;
}

static int test_control_byte_reserve(void) {
    g_mode = FAKE_CONTROL_BYTE_RESERVE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *victim = spawn_echo(inst);
    ish_embed_session_t *pressure[2] = {
        spawn_echo(inst),
        spawn_echo(inst),
    };
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "critical byte reserve: fake reader did not block\n");
        return 1;
    }

    const size_t normal_byte_ceiling =
        ISH_EMBED_MAX_CONTROL_QUEUE_BYTES -
        ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES;
    const size_t first_payload = 65536u;
    const size_t first_wire = ISH_PROTO_HDR_SIZE + first_payload;
    if (normal_byte_ceiling <= first_wire + ISH_PROTO_HDR_SIZE) {
        fprintf(stderr, "critical byte reserve: test byte budget too small\n");
        return 1;
    }
    const size_t second_wire = normal_byte_ceiling - first_wire;
    const size_t second_payload = second_wire - ISH_PROTO_HDR_SIZE;
    if (second_payload > first_payload) {
        fprintf(stderr,
                "critical byte reserve: second payload exceeds test buffer\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(first_payload);
    if (!input) return 1;
    memset(input, 'r', first_payload);
    struct bounded_queue_call writes[2];
    pthread_t write_threads[2];
    if (start_bounded_queue_call(&writes[0], &write_threads[0], pressure[0],
                                 BOUNDED_QUEUE_WRITE,
                                 input, first_payload) != 0)
        return 1;
    if (!wait_control_usage(inst, first_wire, 1, 2000) ||
        atomic_load(&writes[0].done)) {
        fprintf(stderr,
                "critical byte reserve: first write did not stay in flight\n");
        return 1;
    }
    if (start_bounded_queue_call(&writes[1], &write_threads[1], pressure[1],
                                 BOUNDED_QUEUE_WRITE,
                                 input, second_payload) != 0)
        return 1;
    if (!wait_control_usage(inst, normal_byte_ceiling, 2, 2000) ||
        atomic_load(&writes[1].done)) {
        fprintf(stderr,
                "critical byte reserve: normal byte budget did not saturate\n");
        return 1;
    }

    int resize_rc = ish_embed_session_resize(pressure[0], 24, 80, 0, 0);
    if (resize_rc != ISH_ERR_CONTROL_LIMIT) {
        fprintf(stderr,
                "critical byte reserve: normal frame bypassed ceiling (%d)\n",
                resize_rc);
        return 1;
    }

    struct bounded_close_call close_call = {
        .session = victim,
        .elapsed_ms = 0,
    };
    atomic_init(&close_call.done, 0);
    pthread_t close_thread;
    if (pthread_create(&close_thread, NULL,
                       bounded_close_thread, &close_call) != 0)
        return 1;
    const size_t critical_peak = normal_byte_ceiling + ISH_PROTO_HDR_SIZE;
    if (!wait_control_usage(inst, critical_peak, 3, 800) ||
        atomic_load(&close_call.done)) {
        fprintf(stderr,
                "critical byte reserve: SESSION_CLOSE was not admitted\n");
        return 1;
    }

    uint64_t close_deadline = monotonic_ms() + 2500;
    while (!atomic_load(&close_call.done) &&
           monotonic_ms() < close_deadline)
        usleep(1000);
    if (!atomic_load(&close_call.done)) {
        fprintf(stderr,
                "critical byte reserve: close fallback did not return\n");
        return 1;
    }
    pthread_join(close_thread, NULL);
    for (size_t i = 0; i < 2; i++) pthread_join(write_threads[i], NULL);

    size_t cancelled = 0;
    for (size_t i = 0; i < 2; i++) {
        if (writes[i].rc == ISH_ERR_BROKEN_PIPE ||
            writes[i].rc == ISH_ERR_NOT_RUNNING)
            cancelled++;
    }
    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(inst,
                                           "/srv/vms/after-byte-reserve");
    int ok = critical_peak <= ISH_EMBED_MAX_CONTROL_QUEUE_BYTES &&
        resize_rc == ISH_ERR_CONTROL_LIMIT && cancelled == 2 &&
        close_call.elapsed_ms <= 2000 && bytes == 0 && frames == 0 &&
        probe_rc == ISH_ERR_NOT_RUNNING;
    if (!ok) {
        fprintf(stderr,
                "critical byte reserve: resize=%d cancelled=%zu "
                "elapsed=%llums peak=%zu usage=%zu/%zu probe=%d\n",
                resize_rc, cancelled,
                (unsigned long long)close_call.elapsed_ms,
                critical_peak, bytes, frames, probe_rc);
    }

    free(input);
    release_blocked_control_reader(1);
    for (size_t i = 0; i < 2; i++)
        ish_embed_session_close(pressure[i]);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "critical close uses byte reserve at normal saturation: OK\n");
    return ok ? 0 : 1;
}

struct concurrent_spawn_call {
    ish_embed_instance_t *inst;
    const ish_embed_spawn_opts_t *opts;
    ish_embed_session_t *session;
    atomic_int done;
    int rc;
};

static void *concurrent_spawn_thread(void *raw) {
    struct concurrent_spawn_call *call = raw;
    call->rc = ish_embed_spawn(call->inst, call->opts, &call->session);
    atomic_store(&call->done, 1);
    return NULL;
}

static int test_control_spawn_staging_gate(void) {
    enum { spawn_call_count = 4 };
    g_mode = FAKE_CONTROL_SPAWN_GATE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *trigger = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "spawn staging gate: fake reader did not block\n");
        return 1;
    }

    size_t large_len = 70000u;
    char *large_arg = (char *)malloc(large_len + 1);
    if (!large_arg) return 1;
    memset(large_arg, 's', large_len);
    large_arg[large_len] = '\0';
    const char *argv[] = {"/bin/echo", large_arg, NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;

    struct concurrent_spawn_call calls[spawn_call_count];
    pthread_t threads[spawn_call_count];
    for (size_t i = 0; i < spawn_call_count; i++) {
        calls[i] = (struct concurrent_spawn_call) {
            .inst = inst,
            .opts = &opts,
            .session = NULL,
            .rc = 123,
        };
        atomic_init(&calls[i].done, 0);
        if (pthread_create(&threads[i], NULL,
                           concurrent_spawn_thread, &calls[i]) != 0)
            return 1;
    }

    if (!wait_control_frame_usage(inst, 1, 2000)) {
        fprintf(stderr, "spawn staging gate: first SPAWN was not in flight\n");
        return 1;
    }
    usleep(100 * 1000);
    size_t premature = 0;
    for (size_t i = 0; i < spawn_call_count; i++)
        if (atomic_load(&calls[i].done)) premature++;
    if (premature != 0) {
        fprintf(stderr, "spawn staging gate: %zu callers bypassed gate\n",
                premature);
        return 1;
    }

    release_blocked_control_reader(0);
    size_t succeeded = 0;
    for (size_t i = 0; i < spawn_call_count; i++) {
        pthread_join(threads[i], NULL);
        if (calls[i].rc == ISH_OK && calls[i].session) succeeded++;
    }
    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int ok = succeeded == spawn_call_count && bytes == 0 && frames == 0;
    if (!ok) {
        fprintf(stderr,
                "spawn staging gate: succeeded=%zu usage=%zu/%zu\n",
                succeeded, bytes, frames);
    }

    for (size_t i = 0; i < spawn_call_count; i++)
        if (calls[i].session) ish_embed_session_close(calls[i].session);
    ish_embed_session_close(trigger);
    free(large_arg);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok) fprintf(stderr, "serialized SPAWN staging allocation: OK\n");
    return ok ? 0 : 1;
}

static int test_control_oneshot_spawn_lock_deadline(void) {
    g_mode = FAKE_CONTROL_SPAWN_GATE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *trigger = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "oneshot spawn lock: fake reader did not block\n");
        return 1;
    }

    size_t large_len = 70000u;
    char *large_arg = (char *)malloc(large_len + 1);
    if (!large_arg) return 1;
    memset(large_arg, 'g', large_len);
    large_arg[large_len] = '\0';
    const char *holder_argv[] = {"/bin/echo", large_arg, NULL};
    ish_embed_spawn_opts_t holder_opts = {0};
    holder_opts.argv = holder_argv;
    struct concurrent_spawn_call holder = {
        .inst = inst,
        .opts = &holder_opts,
        .session = NULL,
        .rc = 123,
    };
    atomic_init(&holder.done, 0);
    pthread_t holder_thread;
    if (pthread_create(&holder_thread, NULL,
                       concurrent_spawn_thread, &holder) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&holder.done)) {
        fprintf(stderr,
                "oneshot spawn lock: synchronous SPAWN did not hold gate\n");
        return 1;
    }

    const char *oneshot_argv[] = {"/bin/true", NULL};
    ish_embed_spawn_opts_t oneshot_opts = {0};
    oneshot_opts.argv = oneshot_argv;
    oneshot_opts.timeout_ms = 250;
    ish_embed_oneshot_result_t result = {0};
    uint64_t start = monotonic_ms();
    int oneshot_rc = ish_embed_run_oneshot(inst, &oneshot_opts, &result);
    uint64_t elapsed_ms = monotonic_ms() - start;

    size_t bytes = 0, frames = 0;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(inst,
                                           "/srv/vms/while-spawn-gated");
    int ok = oneshot_rc == ISH_ERR_TIMEOUT && elapsed_ms >= 150 &&
        elapsed_ms <= 1000 && !atomic_load(&holder.done) && frames == 1 &&
        result.stdout_buf == NULL && result.stderr_buf == NULL &&
        result.stdout_len == 0 && result.stderr_len == 0 &&
        probe_rc == ISH_OK;
    if (!ok) {
        fprintf(stderr,
                "oneshot spawn lock: rc=%d elapsed=%llums holder=%d "
                "usage=%zu/%zu probe=%d\n",
                oneshot_rc, (unsigned long long)elapsed_ms,
                atomic_load(&holder.done), bytes, frames, probe_rc);
    }

    release_blocked_control_reader(0);
    pthread_join(holder_thread, NULL);
    ish_embed_test_control_usage(inst, &bytes, &frames);
    if (holder.rc != ISH_OK || !holder.session || bytes != 0 || frames != 0)
        ok = 0;
    if (holder.session) ish_embed_session_close(holder.session);
    ish_embed_session_close(trigger);
    free(large_arg);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "oneshot deadline covers the SPAWN staging gate: OK\n");
    return ok ? 0 : 1;
}

static int test_control_streaming_spawn_lock_deadline(void) {
    g_mode = FAKE_CONTROL_SPAWN_GATE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *trigger = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "streaming spawn lock: fake reader did not block\n");
        return 1;
    }

    size_t large_len = 70000u;
    char *large_arg = (char *)malloc(large_len + 1);
    if (!large_arg) return 1;
    memset(large_arg, 'd', large_len);
    large_arg[large_len] = '\0';
    const char *holder_argv[] = {"/bin/echo", large_arg, NULL};
    ish_embed_spawn_opts_t holder_opts = {0};
    holder_opts.argv = holder_argv;
    struct concurrent_spawn_call holder = {
        .inst = inst,
        .opts = &holder_opts,
        .session = NULL,
        .rc = 123,
    };
    atomic_init(&holder.done, 0);
    pthread_t holder_thread;
    if (pthread_create(&holder_thread, NULL,
                       concurrent_spawn_thread, &holder) != 0)
        return 1;
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&holder.done)) {
        fprintf(stderr,
                "streaming spawn lock: synchronous SPAWN did not hold gate\n");
        return 1;
    }

    const char *stream_argv[] = {"/bin/true", NULL};
    ish_embed_spawn_opts_t stream_opts = {0};
    stream_opts.argv = stream_argv;
    stream_opts.timeout_ms = 250;
    ish_embed_session_t *stream = NULL;
    uint64_t start = monotonic_ms();
    int stream_rc = ish_embed_spawn(inst, &stream_opts, &stream);
    uint64_t elapsed_ms = monotonic_ms() - start;

    size_t bytes = 0, frames = 0;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int probe_rc = ish_embed_setup_vm_root(
        inst, "/srv/vms/while-streaming-spawn-gated");
    int ok = stream_rc == ISH_ERR_TIMEOUT && stream == NULL &&
        elapsed_ms >= 150 && elapsed_ms <= 1000 &&
        !atomic_load(&holder.done) && frames == 1 && probe_rc == ISH_OK;
    if (!ok) {
        fprintf(stderr,
                "streaming spawn lock: rc=%d elapsed=%llums holder=%d "
                "usage=%zu/%zu probe=%d\n",
                stream_rc, (unsigned long long)elapsed_ms,
                atomic_load(&holder.done), bytes, frames, probe_rc);
    }

    release_blocked_control_reader(0);
    pthread_join(holder_thread, NULL);
    ish_embed_test_control_usage(inst, &bytes, &frames);
    if (holder.rc != ISH_OK || !holder.session || bytes != 0 || frames != 0)
        ok = 0;

    if (holder.session) ish_embed_session_close(holder.session);
    ish_embed_session_close(trigger);
    free(large_arg);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr, "finite streaming SPAWN respected staging deadline: OK\n");
    return ok ? 0 : 1;
}

static int test_control_finite_streaming_admission(void) {
    g_mode = FAKE_CONTROL_SPAWN_GATE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *trigger = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr, "finite streaming: fake reader did not block\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'f', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, trigger,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0) {
        free(input);
        return 1;
    }
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr,
                "finite streaming: writer did not remain backpressured\n");
        release_blocked_control_reader(0);
        pthread_join(blocker_thread, NULL);
        free(input);
        ish_embed_session_close(trigger);
        (void)ish_embed_shutdown(inst, 2000);
        return 1;
    }

    const char *argv[] = {"/bin/sleep", "forever", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 1000;
    ish_embed_session_t *session = NULL;

    uint64_t spawn_start = monotonic_ms();
    int spawn_rc = ish_embed_spawn(inst, &opts, &session);
    uint64_t spawn_elapsed = monotonic_ms() - spawn_start;
    uint64_t stdin_start = monotonic_ms();
    int stdin_rc = session
        ? ish_embed_session_close_stdin(session) : ISH_ERR_INTERNAL;
    uint64_t stdin_elapsed = monotonic_ms() - stdin_start;
    uint64_t terminate_start = monotonic_ms();
    int terminate_rc = session
        ? ish_embed_session_terminate(session, 1500) : ISH_ERR_INTERNAL;
    uint64_t terminate_elapsed = monotonic_ms() - terminate_start;

    size_t bytes = 0, frames = 0;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int ok = spawn_rc == ISH_OK && session != NULL &&
        stdin_rc == ISH_OK && terminate_rc == ISH_OK &&
        spawn_elapsed <= 500 && stdin_elapsed <= 500 &&
        terminate_elapsed <= 500 && frames == 4 &&
        !atomic_load(&blocker.done);
    if (!ok) {
        fprintf(stderr,
                "finite streaming admission: spawn=%d/%llums stdin=%d/%llums "
                "terminate=%d/%llums usage=%zu/%zu\n",
                spawn_rc, (unsigned long long)spawn_elapsed,
                stdin_rc, (unsigned long long)stdin_elapsed,
                terminate_rc, (unsigned long long)terminate_elapsed,
                bytes, frames);
    }

    release_blocked_control_reader(0);
    pthread_join(blocker_thread, NULL);
    if (blocker.rc != ISH_OK) ok = 0;
    free(input);
    if (session) {
        uint8_t *buffer = NULL;
        size_t length = 0;
        int kind = 0;
        uint64_t sequence = 0;
        int32_t exit_code = 0;
        int32_t signal_value = 0;
        int read_rc = ish_embed_session_read(
            session, 2000, &buffer, &length, &kind, &sequence,
            &exit_code, &signal_value);
        ish_embed_free(buffer);
        if (read_rc != ISH_OK || kind != ISH_STREAM_EXITED ||
            exit_code != 137 || signal_value != 9)
            ok = 0;
        ish_embed_session_close(session);
    }
    ish_embed_session_close(trigger);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "finite streaming controls admitted without writer wait: OK\n");
    return ok ? 0 : 1;
}

static int test_control_finite_streaming_write_busy(void) {
    g_mode = FAKE_CONTROL_CRITICAL_CLOSE;
    ish_embed_instance_t *inst = boot_instance();
    const char *argv[] = {"/bin/sleep", "forever", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 1000;
    ish_embed_session_t *session = NULL;
    if (ish_embed_spawn(inst, &opts, &session) != ISH_OK || !session)
        return 1;

    ish_embed_session_t *trigger = spawn_echo(inst);
    if (!wait_fake_flag(&g_control_reader_blocked, 2000)) {
        fprintf(stderr,
                "finite streaming write: fake reader did not block\n");
        return 1;
    }

    uint8_t *input = (uint8_t *)malloc(65536u);
    if (!input) return 1;
    memset(input, 'w', 65536u);
    struct bounded_queue_call blocker;
    pthread_t blocker_thread;
    if (start_bounded_queue_call(&blocker, &blocker_thread, session,
                                 BOUNDED_QUEUE_WRITE, input, 65536u) != 0) {
        free(input);
        return 1;
    }
    if (!wait_control_frame_usage(inst, 1, 2000) ||
        atomic_load(&blocker.done)) {
        fprintf(stderr,
                "finite streaming write: writer did not hold stdin order\n");
        release_blocked_control_reader(0);
        pthread_join(blocker_thread, NULL);
        free(input);
        return 1;
    }

    uint64_t close_start = monotonic_ms();
    int close_rc = ish_embed_session_close_stdin(session);
    uint64_t close_elapsed = monotonic_ms() - close_start;
    uint64_t terminate_start = monotonic_ms();
    int terminate_rc = ish_embed_session_terminate(session, 1500);
    uint64_t terminate_elapsed = monotonic_ms() - terminate_start;
    size_t frames = 0;
    ish_embed_test_control_usage(inst, NULL, &frames);
    int ok = close_rc == ISH_ERR_BUSY &&
        strcmp(ish_embed_strerror(close_rc),
               "operation cannot proceed while runtime state is busy") == 0 &&
        close_elapsed <= 500 &&
        terminate_rc == ISH_OK && terminate_elapsed <= 500 &&
        frames == 2 && !atomic_load(&blocker.done);
    if (!ok) {
        fprintf(stderr,
                "finite streaming write: close=%d/%llums terminate=%d/%llums "
                "frames=%zu writer=%d\n",
                close_rc, (unsigned long long)close_elapsed,
                terminate_rc, (unsigned long long)terminate_elapsed,
                frames, atomic_load(&blocker.done));
    }

    release_blocked_control_reader(0);
    pthread_join(blocker_thread, NULL);
    if (blocker.rc != ISH_OK ||
        ish_embed_session_close_stdin(session) != ISH_OK)
        ok = 0;
    free(input);

    uint8_t *buffer = NULL;
    size_t length = 0;
    int kind = 0;
    uint64_t sequence = 0;
    int32_t exit_code = 0;
    int32_t signal_value = 0;
    int read_rc = ish_embed_session_read(
        session, 2000, &buffer, &length, &kind, &sequence,
        &exit_code, &signal_value);
    ish_embed_free(buffer);
    if (read_rc != ISH_OK || kind != ISH_STREAM_EXITED ||
        exit_code != 137 || signal_value != 9)
        ok = 0;
    ish_embed_session_close(session);
    ish_embed_session_close(trigger);
    if (ish_embed_shutdown(inst, 2000) != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "finite stdin close stays bounded behind active write: OK\n");
    return ok ? 0 : 1;
}

static int test_control_streaming_queue_deadline(void) {
    g_mode = FAKE_WRITER_LOCK_HOLD;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *trigger = spawn_echo(inst);
    struct bounded_queue_call holder;
    pthread_t holder_thread;
    if (start_bounded_queue_call(&holder, &holder_thread, trigger,
                                 BOUNDED_QUEUE_RESIZE, NULL, 0) != 0)
        return 1;
    if (!wait_fake_flag(&g_writer_lock_held, 2000)) {
        fprintf(stderr,
                "streaming queue deadline: writer lock was not held\n");
        pthread_mutex_lock(&g_fake_lock);
        g_release_writer_lock = 1;
        pthread_cond_broadcast(&g_fake_cond);
        pthread_mutex_unlock(&g_fake_lock);
        pthread_join(holder_thread, NULL);
        return 1;
    }

    const char *argv[] = {"/bin/true", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 100;
    ish_embed_session_t *session = NULL;
    uint64_t start = monotonic_ms();
    int spawn_rc = ish_embed_spawn(inst, &opts, &session);
    uint64_t elapsed_ms = monotonic_ms() - start;
    int ok = spawn_rc == ISH_ERR_TIMEOUT && session == NULL &&
        elapsed_ms >= 50 && elapsed_ms <= 500 &&
        !atomic_load(&holder.done);
    if (!ok) {
        fprintf(stderr,
                "streaming queue deadline: rc=%d session=%p elapsed=%llums "
                "holder=%d\n",
                spawn_rc, (void *)session,
                (unsigned long long)elapsed_ms,
                atomic_load(&holder.done));
    }

    pthread_mutex_lock(&g_fake_lock);
    g_release_writer_lock = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    pthread_join(holder_thread, NULL);
    if (holder.rc != ISH_OK) ok = 0;
    ish_embed_session_close(trigger);
    if (ish_embed_shutdown(inst, 2000) != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "finite streaming SPAWN bounded queue admission: OK\n");
    return ok ? 0 : 1;
}

static int test_control_streaming_precommit_deadline(void) {
    g_mode = FAKE_WRITER_PRECOMMIT_DEADLINE;
    ish_embed_instance_t *inst = boot_instance();
    const char *argv[] = {"/bin/true", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 100;
    ish_embed_session_t *session = NULL;

    uint64_t start = monotonic_ms();
    int spawn_rc = ish_embed_spawn(inst, &opts, &session);
    uint64_t elapsed_ms = monotonic_ms() - start;
    size_t bytes = 1, frames = 1;
    ish_embed_test_control_usage(inst, &bytes, &frames);
    int ok = spawn_rc == ISH_ERR_TIMEOUT && session == NULL &&
        elapsed_ms >= 100 && elapsed_ms <= 500 &&
        bytes == 0 && frames == 0;
    if (!ok) {
        fprintf(stderr,
                "streaming precommit deadline: rc=%d session=%p "
                "elapsed=%llums usage=%zu/%zu\n",
                spawn_rc, (void *)session,
                (unsigned long long)elapsed_ms, bytes, frames);
    }

    if (ish_embed_shutdown(inst, 2000) != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "finite streaming SPAWN rejected after precommit expiry: OK\n");
    return ok ? 0 : 1;
}

static int test_supervisor_error_status(void) {
    g_mode = FAKE_SUPERVISOR_ERROR;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    uint8_t *buf = NULL;
    size_t len = 0;
    int kind = 0;
    uint64_t seq = 0;
    int32_t code = 0, sig = 0;
    int rc = ish_embed_session_read(session, 2000, &buf, &len, &kind,
                                    &seq, &code, &sig);
    ish_embed_free(buf);
    int ok = rc == ISH_ERR_SUPERVISOR;
    if (!ok) fprintf(stderr, "supervisor error status: rc=%d\n", rc);
    ish_embed_session_close(session);
    if (ish_embed_shutdown(inst, 2000) != ISH_OK) ok = 0;
    if (ok) fprintf(stderr, "supervisor ERROR remains typed: OK\n");
    return ok ? 0 : 1;
}

static int test_custom_supervisor_path(void) {
    g_mode = FAKE_CUSTOM_SUPERVISOR;
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.supervisor_guest_path = "/custom/ishsv";
    opts.kernel_log_fd = -1;
    ish_embed_instance_t *inst = NULL;
    int rc = ish_embed_boot(&opts, &inst);
    if (rc != ISH_OK || !inst || g_install_count != 0 ||
        strcmp(g_exec_path, "/custom/ishsv") != 0) {
        fprintf(stderr, "custom-supervisor: rc=%d installs=%d exec=%s\n",
                rc, g_install_count, g_exec_path);
        return 1;
    }
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "custom-supervisor shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "custom supervisor skips bundle injection: OK\n");
    return 0;
}

static int test_boot_requires_output_handle(void) {
    g_mode = FAKE_CUSTOM_SUPERVISOR;
    ish_embed_boot_opts_t opts = {0};
    opts.rootfs_path = "/fake/rootfs";
    opts.workdir = "/";
    opts.kernel_log_fd = -1;

    int rc = ish_embed_boot(&opts, NULL);
    if (rc != ISH_ERR_INVALID_ARG || g_kernel_started || g_install_count != 0) {
        fprintf(stderr,
                "boot output validation: rc=%d kernel=%d installs=%d\n",
                rc, g_kernel_started, g_install_count);
        return 1;
    }

    /* Rejection must precede g_boot_consumed and every runtime side effect. */
    ish_embed_instance_t *inst = boot_instance();
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "boot output validation shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "boot requires a reachable output handle: OK\n");
    return 0;
}

struct read_arg {
    ish_embed_session_t *session;
    int rc;
};

static void *borrowed_read(void *raw) {
    struct read_arg *arg = raw;
    uint8_t *buf = NULL;
    size_t len = 0;
    int kind = 0;
    uint64_t seq = 0;
    int32_t code = 0, sig = 0;
    arg->rc = ish_embed_session_read(arg->session, UINT32_MAX, &buf, &len,
                                     &kind, &seq, &code, &sig);
    ish_embed_free(buf);
    if (g_mode == FAKE_BORROW_HOLD) {
        pthread_mutex_lock(&g_fake_lock);
        g_borrow_waiting = 1;
        pthread_cond_broadcast(&g_fake_cond);
        while (!g_release_borrow)
            pthread_cond_wait(&g_fake_cond, &g_fake_lock);
        pthread_mutex_unlock(&g_fake_lock);
    }
    ish_embed_session_release(arg->session);
    return NULL;
}

static void *close_session_thread(void *raw) {
    ish_embed_session_close(raw);
    pthread_mutex_lock(&g_fake_lock);
    g_close_done = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    return NULL;
}

static int test_close_race(void) {
    g_mode = FAKE_CLOSE_RACE;
    ish_embed_instance_t *inst = boot_instance();
    for (int i = 0; i < 200; i++) {
        ish_embed_session_t *session = spawn_echo(inst);
        if (ish_embed_session_retain(session) != ISH_OK) return 1;
        struct read_arg arg = {.session = session, .rc = 0};
        pthread_t reader;
        if (pthread_create(&reader, NULL, borrowed_read, &arg) != 0) return 1;
        if ((i & 1) == 0) sched_yield(); else usleep(50);
        ish_embed_session_close(session);
        pthread_join(reader, NULL);
        if (arg.rc != ISH_OK && arg.rc != ISH_ERR_NO_SESSION) {
            fprintf(stderr, "close-race iteration %d: read rc=%d\n", i, arg.rc);
            return 1;
        }
    }
    int rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "close-race shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "close/read lifetime: OK\n");
    return 0;
}

static int test_backlog_limit(void) {
    g_mode = FAKE_BACKLOG;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);

    pthread_mutex_lock(&g_fake_lock);
    while (!g_burst_done) pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);

    size_t accepted = 0;
    int rc = ISH_OK;
    while (rc == ISH_OK) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int kind = 0;
        uint64_t seq = 0;
        int32_t code = 0, sig = 0;
        rc = ish_embed_session_read(session, 2000, &buf, &len, &kind,
                                    &seq, &code, &sig);
        if (rc == ISH_OK && kind != ISH_STREAM_EXITED) accepted += len;
        ish_embed_free(buf);
    }
    if (rc != ISH_ERR_OUTPUT_LIMIT ||
        accepted != ISH_EMBED_MAX_SESSION_BACKLOG_BYTES) {
        fprintf(stderr, "backlog: rc=%d accepted=%zu expected=%u\n",
                rc, accepted, ISH_EMBED_MAX_SESSION_BACKLOG_BYTES);
        return 1;
    }

    ish_embed_session_close(session);
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "backlog shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "bounded backlog: OK (%zu bytes)\n", accepted);
    return 0;
}

static int test_frame_backlog_limit(void) {
    g_mode = FAKE_BACKLOG_FRAMES;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);

    pthread_mutex_lock(&g_fake_lock);
    while (!g_burst_done) pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);

    size_t accepted_frames = 0;
    int rc = ISH_OK;
    while (rc == ISH_OK) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int kind = 0;
        uint64_t seq = 0;
        int32_t code = 0, sig = 0;
        rc = ish_embed_session_read(session, 2000, &buf, &len, &kind,
                                    &seq, &code, &sig);
        if (rc == ISH_OK && kind != ISH_STREAM_EXITED) {
            if (len != 1) {
                fprintf(stderr, "frame backlog: unexpected len=%zu\n", len);
                ish_embed_free(buf);
                return 1;
            }
            accepted_frames++;
        }
        ish_embed_free(buf);
    }
    if (rc != ISH_ERR_OUTPUT_LIMIT ||
        accepted_frames != ISH_EMBED_MAX_SESSION_BACKLOG_FRAMES) {
        fprintf(stderr, "frame backlog: rc=%d accepted=%zu expected=%u\n",
                rc, accepted_frames, ISH_EMBED_MAX_SESSION_BACKLOG_FRAMES);
        return 1;
    }

    ish_embed_session_close(session);
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "frame backlog shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "bounded frame backlog: OK (%zu frames)\n", accepted_frames);
    return 0;
}

static int pressure_writer_result_ok(int rc) {
    return rc == ISH_ERR_BROKEN_PIPE || rc == ISH_ERR_NOT_RUNNING;
}

static int test_backlog_control_pressure(void) {
    g_mode = FAKE_BACKLOG_CONTROL_PRESSURE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    struct pressure_write_arg writer;
    pthread_t writer_thread;
    if (start_pressure_writer(session, &writer, &writer_thread) != 0) return 1;

    if (!wait_fake_flag(&g_stdin_header_seen, 3000)) {
        fprintf(stderr, "backlog pressure: stdin header was not observed\n");
        return 1;
    }
    pthread_mutex_lock(&g_fake_lock);
    int prematurely_done = g_pressure_writer_done;
    g_start_pressure_events = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);

    int completed_without_watchdog = wait_pressure_with_watchdog();
    pthread_join(writer_thread, NULL);
    free(writer.bytes);

    size_t accepted = 0;
    int rc = ISH_OK;
    while (rc == ISH_OK) {
        uint8_t *buf = NULL;
        size_t len = 0;
        int kind = 0;
        uint64_t seq = 0;
        int32_t code = 0, sig = 0;
        rc = ish_embed_session_read(session, 2000, &buf, &len, &kind,
                                    &seq, &code, &sig);
        if (rc == ISH_OK && kind != ISH_STREAM_EXITED) accepted += len;
        ish_embed_free(buf);
    }

    pthread_mutex_lock(&g_fake_lock);
    int protocol_ok = g_pressure_protocol_ok;
    int watchdog = g_pressure_watchdog_fired;
    pthread_mutex_unlock(&g_fake_lock);
    int ok = !prematurely_done && completed_without_watchdog && !watchdog &&
        protocol_ok && pressure_writer_result_ok(writer.rc) &&
        rc == ISH_ERR_OUTPUT_LIMIT &&
        accepted == ISH_EMBED_MAX_SESSION_BACKLOG_BYTES;
    if (!ok) {
        fprintf(stderr,
                "backlog pressure: writer=%d read=%d accepted=%zu proto=%d "
                "premature=%d watchdog=%d\n",
                writer.rc, rc, accepted, protocol_ok, prematurely_done, watchdog);
    }

    ish_embed_session_close(session);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) {
        fprintf(stderr, "backlog pressure shutdown: rc=%d\n", shutdown_rc);
        ok = 0;
    }
    if (ok) fprintf(stderr, "bidirectional backlog pressure drains: OK\n");
    return ok ? 0 : 1;
}

static int test_protocol_fatal_control_pressure(void) {
    g_mode = FAKE_PROTOCOL_FATAL_CONTROL_PRESSURE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    struct pressure_write_arg writer;
    pthread_t writer_thread;
    if (start_pressure_writer(session, &writer, &writer_thread) != 0) return 1;

    if (!wait_fake_flag(&g_stdin_header_seen, 3000)) {
        fprintf(stderr, "fatal pressure: stdin header was not observed\n");
        return 1;
    }
    pthread_mutex_lock(&g_fake_lock);
    int prematurely_done = g_pressure_writer_done;
    g_start_pressure_events = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);

    int completed_without_watchdog = wait_pressure_with_watchdog();
    pthread_join(writer_thread, NULL);
    free(writer.bytes);

    uint8_t *buf = NULL;
    size_t len = 0;
    int kind = 0;
    uint64_t seq = 0;
    int32_t code = 0, sig = 0;
    int rc = ish_embed_session_read(session, 4000, &buf, &len, &kind,
                                    &seq, &code, &sig);
    ish_embed_free(buf);

    pthread_mutex_lock(&g_fake_lock);
    int protocol_ok = g_pressure_protocol_ok;
    int event_epipe = g_event_epipe_seen;
    int watchdog = g_pressure_watchdog_fired;
    pthread_mutex_unlock(&g_fake_lock);
    int ok = !prematurely_done && completed_without_watchdog && !watchdog &&
        protocol_ok && event_epipe && pressure_writer_result_ok(writer.rc) &&
        rc == ISH_ERR_PROTOCOL;
    if (!ok) {
        fprintf(stderr,
                "fatal pressure: writer=%d read=%d proto=%d event_epipe=%d "
                "premature=%d watchdog=%d\n",
                writer.rc, rc, protocol_ok, event_epipe, prematurely_done, watchdog);
    }

    ish_embed_session_close(session);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) {
        fprintf(stderr, "fatal pressure shutdown: rc=%d\n", shutdown_rc);
        ok = 0;
    }
    if (ok) fprintf(stderr, "fatal protocol cancels pressured control writer: OK\n");
    return ok ? 0 : 1;
}

static int test_borrow_blocks_shutdown(void) {
    g_mode = FAKE_BORROW_HOLD;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    if (ish_embed_session_retain(session) != ISH_OK) return 1;

    struct read_arg arg = {.session = session, .rc = 0};
    pthread_t reader, closer;
    if (pthread_create(&reader, NULL, borrowed_read, &arg) != 0) return 1;
    if (pthread_create(&closer, NULL, close_session_thread, session) != 0) return 1;

    pthread_mutex_lock(&g_fake_lock);
    while (!g_borrow_waiting) pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    if (g_close_done) {
        pthread_mutex_unlock(&g_fake_lock);
        fprintf(stderr, "borrow/close gate: close returned before borrow release\n");
        return 1;
    }
    pthread_mutex_unlock(&g_fake_lock);

    int rc = ish_embed_shutdown(inst, 100);
    if (rc != ISH_ERR_BUSY) {
        fprintf(stderr, "borrow/shutdown gate: rc=%d\n", rc);
        return 1;
    }

    pthread_mutex_lock(&g_fake_lock);
    g_release_borrow = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    pthread_join(reader, NULL);
    pthread_join(closer, NULL);

    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "borrow final shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "borrow keeps instance alive: OK\n");
    return 0;
}

struct shutdown_arg {
    ish_embed_instance_t *inst;
    int rc;
};

static void *direct_shutdown_thread(void *raw) {
    struct shutdown_arg *arg = raw;
    arg->rc = ish_embed_shutdown(arg->inst, 2000);
    return NULL;
}

static void *shutdown_thread(void *raw) {
    struct shutdown_arg *arg = raw;
    pthread_mutex_lock(&g_fake_lock);
    g_shutdown_callers_ready++;
    pthread_cond_broadcast(&g_fake_cond);
    while (!g_release_shutdown_callers)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);
    arg->rc = ish_embed_shutdown(arg->inst, 2000);
    return NULL;
}

static int test_double_shutdown(void) {
    g_mode = FAKE_DOUBLE_SHUTDOWN;
    ish_embed_instance_t *inst = boot_instance();
    struct shutdown_arg a = {.inst = inst, .rc = 123};
    struct shutdown_arg b = {.inst = inst, .rc = 123};
    pthread_t ta, tb;
    if (pthread_create(&ta, NULL, shutdown_thread, &a) != 0) return 1;
    if (pthread_create(&tb, NULL, shutdown_thread, &b) != 0) return 1;

    pthread_mutex_lock(&g_fake_lock);
    while (g_shutdown_callers_ready != 2)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    g_release_shutdown_callers = 1;
    pthread_cond_broadcast(&g_fake_cond);
    while (!g_shutdown_seen)
        pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    /* Keep the first shutdown inside the fake kernel long enough for the
     * second caller to contend on the singleton gate. */
    pthread_mutex_unlock(&g_fake_lock);
    usleep(50 * 1000);
    pthread_mutex_lock(&g_fake_lock);
    g_release_shutdown = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    int ok = (a.rc == ISH_OK && b.rc == ISH_ERR_INVALID_ARG) ||
             (b.rc == ISH_OK && a.rc == ISH_ERR_INVALID_ARG);
    if (!ok) {
        fprintf(stderr, "double-shutdown: rc=(%d,%d)\n", a.rc, b.rc);
        return 1;
    }
    fprintf(stderr, "single shutdown owner: OK\n");
    return 0;
}

static int test_streaming_instance_gate_deadline(void) {
    g_mode = FAKE_DOUBLE_SHUTDOWN;
    ish_embed_instance_t *inst = boot_instance();
    struct shutdown_arg shutdown = {.inst = inst, .rc = 123};
    pthread_t shutdown_worker;
    if (pthread_create(&shutdown_worker, NULL,
                       direct_shutdown_thread, &shutdown) != 0)
        return 1;
    if (!wait_fake_flag(&g_shutdown_seen, 2000)) {
        fprintf(stderr,
                "streaming instance gate: shutdown did not hold singleton\n");
        pthread_mutex_lock(&g_fake_lock);
        g_release_shutdown = 1;
        pthread_cond_broadcast(&g_fake_cond);
        pthread_mutex_unlock(&g_fake_lock);
        pthread_join(shutdown_worker, NULL);
        return 1;
    }

    const char *argv[] = {"/bin/true", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 100;
    ish_embed_session_t *session = NULL;
    uint64_t start = monotonic_ms();
    int spawn_rc = ish_embed_spawn(inst, &opts, &session);
    uint64_t elapsed_ms = monotonic_ms() - start;
    int ok = spawn_rc == ISH_ERR_TIMEOUT && session == NULL &&
        elapsed_ms >= 50 && elapsed_ms <= 500;
    if (!ok) {
        fprintf(stderr,
                "streaming instance gate: rc=%d session=%p elapsed=%llums\n",
                spawn_rc, (void *)session,
                (unsigned long long)elapsed_ms);
    }

    pthread_mutex_lock(&g_fake_lock);
    g_release_shutdown = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    pthread_join(shutdown_worker, NULL);
    if (shutdown.rc != ISH_OK) ok = 0;
    if (ok)
        fprintf(stderr,
                "finite streaming SPAWN bounded singleton admission: OK\n");
    return ok ? 0 : 1;
}

struct setup_arg {
    ish_embed_instance_t *inst;
    int rc;
};

static void *setup_thread(void *raw) {
    struct setup_arg *arg = raw;
    arg->rc = ish_embed_setup_vm_root(arg->inst, "/srv/vms/test");
    return NULL;
}

static int test_active_call_blocks_shutdown(void) {
    g_mode = FAKE_ACTIVE_CALL_HOLD;
    ish_embed_instance_t *inst = boot_instance();
    struct setup_arg arg = {.inst = inst, .rc = 123};
    pthread_t worker;
    if (pthread_create(&worker, NULL, setup_thread, &arg) != 0) return 1;

    pthread_mutex_lock(&g_fake_lock);
    while (!g_active_call_waiting) pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);

    int rc = ish_embed_shutdown(inst, 100);
    if (rc != ISH_ERR_BUSY) {
        fprintf(stderr, "active-call shutdown gate: rc=%d\n", rc);
        return 1;
    }

    pthread_mutex_lock(&g_fake_lock);
    g_release_active_call = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    pthread_join(worker, NULL);
    if (arg.rc != ISH_OK) {
        fprintf(stderr, "active-call setup: rc=%d\n", arg.rc);
        return 1;
    }
    if (ish_embed_setup_vm_root(inst, "") != ISH_ERR_INVALID_ARG ||
        ish_embed_setup_vm_root(inst, "relative/vm") != ISH_ERR_INVALID_ARG ||
        ish_embed_setup_vm_root(inst, NULL) != ISH_ERR_INVALID_ARG ||
        ish_embed_setup_vm_root(NULL, "/srv/vms/test") != ISH_ERR_INVALID_ARG ||
        ish_embed_setup_vm_root(inst, "/srv/vms/test") != ISH_OK) {
        fprintf(stderr, "setup_vm_root compatibility validation failed\n");
        return 1;
    }
    ish_embed_session_t *session = spawn_echo(inst);
    uint8_t byte = 'x';
    if (ish_embed_session_close_stdin(session) != ISH_OK ||
        ish_embed_session_write(session, &byte, 1) != ISH_ERR_BROKEN_PIPE) {
        fprintf(stderr, "write-after-stdin-close validation failed\n");
        return 1;
    }
    ish_embed_session_close(session);
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "active-call final shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "active instance call keeps instance alive: OK\n");
    return 0;
}

static int test_broken_control_pipe(void) {
    g_mode = FAKE_BROKEN_CONTROL;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);

    pthread_mutex_lock(&g_fake_lock);
    while (!g_control_closed) pthread_cond_wait(&g_fake_cond, &g_fake_lock);
    pthread_mutex_unlock(&g_fake_lock);

    int rc = ish_embed_session_signal(session, 2);
    if (rc != ISH_ERR_BROKEN_PIPE) {
        fprintf(stderr, "broken control pipe: rc=%d\n", rc);
        return 1;
    }

    pthread_mutex_lock(&g_fake_lock);
    g_release_control = 1;
    pthread_cond_broadcast(&g_fake_cond);
    pthread_mutex_unlock(&g_fake_lock);
    ish_embed_session_close(session);
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "broken control shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "broken control pipe returns EPIPE without SIGPIPE: OK\n");
    return 0;
}

static int test_protocol_fatal_drain(void) {
    g_mode = FAKE_PROTOCOL_FATAL;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    uint8_t *buf = NULL;
    size_t len = 0;
    int kind = 0;
    uint64_t seq = 0;
    int32_t code = 0, sig = 0;
    uint64_t start = monotonic_ms();
    int rc = ish_embed_session_read(session, 4000, &buf, &len, &kind,
                                    &seq, &code, &sig);
    uint64_t elapsed = monotonic_ms() - start;
    ish_embed_free(buf);
    if (rc != ISH_ERR_PROTOCOL || elapsed > 5000) {
        fprintf(stderr, "protocol fatal drain: rc=%d elapsed=%llums\n",
                rc, (unsigned long long)elapsed);
        return 1;
    }
    ish_embed_session_close(session);
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "protocol fatal shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "fatal protocol pipe aborted without writer deadlock: OK\n");
    return 0;
}

static int read_session_terminal_status(ish_embed_session_t *session) {
    uint8_t *buf = NULL;
    size_t len = 0;
    int kind = 0;
    uint64_t seq = 0;
    int32_t code = 0, sig = 0;
    int rc = ish_embed_session_read(session, 4000, &buf, &len, &kind,
                                    &seq, &code, &sig);
    ish_embed_free(buf);
    return rc;
}

static int test_malformed_event_rejected(enum malformed_event_kind kind,
                                         const char *name) {
    g_mode = FAKE_MALFORMED_EVENT;
    g_malformed_event_kind = kind;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    int rc = read_session_terminal_status(session);
    int ok = rc == ISH_ERR_PROTOCOL;
    if (!ok)
        fprintf(stderr, "malformed %s event: rc=%d\n", name, rc);
    ish_embed_session_close(session);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) {
        fprintf(stderr, "malformed %s shutdown: rc=%d\n", name, shutdown_rc);
        ok = 0;
    }
    if (ok) fprintf(stderr, "malformed %s event rejected: OK\n", name);
    return ok ? 0 : 1;
}

static int test_output_allocation_failure(void) {
    g_mode = FAKE_OUTPUT_ALLOCATION_FAILURE;
    ish_embed_instance_t *inst = boot_instance();
    ish_embed_session_t *session = spawn_echo(inst);
    int rc = read_session_terminal_status(session);
    int ok = rc == ISH_ERR_OOM;
    if (!ok) fprintf(stderr, "output allocation failure: rc=%d\n", rc);
    ish_embed_session_close(session);
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) {
        fprintf(stderr, "output allocation shutdown: rc=%d\n", shutdown_rc);
        ok = 0;
    }
    if (ok) fprintf(stderr, "output allocation failure is explicit: OK\n");
    return ok ? 0 : 1;
}

static int test_spawn_argument_bound(void) {
    g_mode = FAKE_SPAWN_ARGUMENT_BOUND;
    ish_embed_instance_t *inst = boot_instance();
    size_t oversized_len = ISH_EMBED_MAX_PROTOCOL_FRAME_BYTES + 1u;
    char *oversized = (char *)malloc(oversized_len + 1u);
    if (!oversized) return 1;
    memset(oversized, 'x', oversized_len);
    oversized[oversized_len] = 0;
    const char *oversized_argv[] = {oversized, NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = oversized_argv;
    ish_embed_session_t *session = NULL;
    int rc_large = ish_embed_spawn(inst, &opts, &session);
    free(oversized);

    size_t vector_count = 4097;
    const char **many = (const char **)calloc(vector_count + 1,
                                               sizeof(*many));
    if (!many) return 1;
    for (size_t i = 0; i < vector_count; i++) many[i] = "x";
    opts.argv = many;
    int rc_count = ish_embed_spawn(inst, &opts, &session);
    free(many);

    int ok = rc_large == ISH_ERR_INVALID_ARG &&
             rc_count == ISH_ERR_INVALID_ARG && session == NULL;
    if (!ok) {
        fprintf(stderr, "spawn argument bound: large=%d count=%d session=%p\n",
                rc_large, rc_count, (void *)session);
    }
    int shutdown_rc = ish_embed_shutdown(inst, 2000);
    if (shutdown_rc != ISH_OK) {
        fprintf(stderr, "spawn argument bound shutdown: rc=%d\n", shutdown_rc);
        ok = 0;
    }
    if (ok) fprintf(stderr, "SPAWN payload/count bounds enforced: OK\n");
    return ok ? 0 : 1;
}

static int test_shutdown_drain(void) {
    g_mode = FAKE_SHUTDOWN_DRAIN;
    ish_embed_instance_t *inst = boot_instance();
    uint64_t start = monotonic_ms();
    int rc = ish_embed_shutdown(inst, 4000);
    uint64_t elapsed = monotonic_ms() - start;
    if (rc != ISH_OK || elapsed > 5000) {
        fprintf(stderr, "shutdown-drain: rc=%d elapsed=%llums\n",
                rc, (unsigned long long)elapsed);
        return 1;
    }
    if (g_log_sink_fd >= 0) {
        close(g_log_sink_fd);
        g_log_sink_fd = -1;
    }
    fprintf(stderr, "shutdown pump drain: OK (%llums)\n",
            (unsigned long long)elapsed);
    return 0;
}

static int test_log_sink_backpressure(void) {
    g_mode = FAKE_LOG_BACKPRESSURE;
    ish_embed_instance_t *inst = boot_instance();
    if (!wait_fake_flag(&g_log_write_started, 2000)) {
        fprintf(stderr, "log-backpressure: sink writer did not block\n");
        return 1;
    }
    uint64_t start = monotonic_ms();
    int rc = ish_embed_shutdown(inst, 4000);
    uint64_t elapsed = monotonic_ms() - start;
    if (rc != ISH_OK || elapsed > 5000) {
        fprintf(stderr, "log-backpressure: rc=%d elapsed=%llums\n",
                rc, (unsigned long long)elapsed);
        return 1;
    }
    if (g_log_sink_fd >= 0) {
        close(g_log_sink_fd);
        g_log_sink_fd = -1;
    }
    if (g_log_sink_read_fd >= 0) {
        close(g_log_sink_read_fd);
        g_log_sink_read_fd = -1;
    }
    fprintf(stderr, "full log sink did not block shutdown: OK (%llums)\n",
            (unsigned long long)elapsed);
    return 0;
}

static int test_oneshot_hard_timeout(void) {
    g_mode = FAKE_ONESHOT_HANG;
    ish_embed_instance_t *inst = boot_instance();
    const char *argv[] = {"/bin/sleep", "forever", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    opts.timeout_ms = 25;
    ish_embed_oneshot_result_t result;
    uint64_t start = monotonic_ms();
    int rc = ish_embed_run_oneshot(inst, &opts, &result);
    uint64_t elapsed = monotonic_ms() - start;
    if (rc != ISH_ERR_TIMEOUT || elapsed > 6000) {
        fprintf(stderr, "oneshot-hard-timeout: rc=%d elapsed=%llums\n",
                rc, (unsigned long long)elapsed);
        return 1;
    }
    pthread_mutex_lock(&g_fake_lock);
    int terminate_count = g_terminate_count;
    int signal_count = g_signal_count;
    int session_close_count = g_session_close_count;
    pthread_mutex_unlock(&g_fake_lock);
    if (terminate_count != 1 || signal_count != 1 ||
        session_close_count != 1) {
        fprintf(stderr,
                "oneshot phases: terminate=%d signal=%d session-close=%d\n",
                terminate_count, signal_count, session_close_count);
        return 1;
    }
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "oneshot shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "oneshot hard timeout: OK (%llums)\n",
            (unsigned long long)elapsed);
    return 0;
}

static int test_oneshot_output_limit(void) {
    g_mode = FAKE_ONESHOT_OUTPUT;
    ish_embed_instance_t *inst = boot_instance();
    const char *argv[] = {"/bin/yes", NULL};
    ish_embed_spawn_opts_t opts = {0};
    opts.argv = argv;
    ish_embed_oneshot_result_t result;
    uint64_t start = monotonic_ms();
    int rc = ish_embed_run_oneshot(inst, &opts, &result);
    uint64_t elapsed = monotonic_ms() - start;
    if (rc != ISH_ERR_OUTPUT_LIMIT || elapsed > 5000 ||
        result.stdout_buf != NULL || result.stderr_buf != NULL ||
        result.stdout_len != 0 || result.stderr_len != 0) {
        fprintf(stderr, "oneshot output limit: rc=%d elapsed=%llums out=%zu err=%zu\n",
                rc, (unsigned long long)elapsed,
                result.stdout_len, result.stderr_len);
        return 1;
    }
    rc = ish_embed_shutdown(inst, 2000);
    if (rc != ISH_OK) {
        fprintf(stderr, "oneshot output shutdown: rc=%d\n", rc);
        return 1;
    }
    fprintf(stderr, "oneshot aggregate output bound: OK (%llums)\n",
            (unsigned long long)elapsed);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "malformed-event") == 0) {
        if (strcmp(argv[2], "spawned") == 0)
            return test_malformed_event_rejected(MALFORMED_SPAWNED, argv[2]);
        if (strcmp(argv[2], "stdout") == 0)
            return test_malformed_event_rejected(MALFORMED_STDOUT, argv[2]);
        if (strcmp(argv[2], "stderr") == 0)
            return test_malformed_event_rejected(MALFORMED_STDERR, argv[2]);
        if (strcmp(argv[2], "exited") == 0)
            return test_malformed_event_rejected(MALFORMED_EXITED, argv[2]);
        if (strcmp(argv[2], "shutdown-ack") == 0)
            return test_malformed_event_rejected(MALFORMED_SHUTDOWN_ACK, argv[2]);
        if (strcmp(argv[2], "pong") == 0)
            return test_malformed_event_rejected(MALFORMED_PONG, argv[2]);
        if (strcmp(argv[2], "unknown") == 0)
            return test_malformed_event_rejected(MALFORMED_UNKNOWN, argv[2]);
        fprintf(stderr, "unknown malformed event: %s\n", argv[2]);
        return 2;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s boot-timeout|bad-hello-ack|install-failure|bundled-supervisor-digest-mismatch|bundled-supervisor-path-mismatch|custom-supervisor|boot-null-output|stdin-close-order|control-frame-limit|control-critical-close|control-same-session-close|control-exited-same-session-close|signal-close-order|resize-close-order|terminate-close-order|control-critical-oneshot|control-preblocked-oneshot|control-byte-limit|control-byte-reserve|control-spawn-gate|control-oneshot-spawn-lock|control-streaming-spawn-lock|control-finite-streaming|control-finite-streaming-write|control-streaming-queue-deadline|control-streaming-precommit-deadline|streaming-instance-gate|supervisor-error|close-race|backlog|frame-backlog|backlog-control-pressure|borrow-shutdown|double-shutdown|active-call|broken-control|protocol-fatal|protocol-fatal-control-pressure|malformed-event TYPE|output-allocation-failure|spawn-argument-bound|shutdown-drain|log-backpressure|oneshot-timeout|oneshot-output\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "boot-timeout") == 0) return test_boot_timeout_cleanup();
    if (strcmp(argv[1], "bad-hello-ack") == 0) return test_bad_hello_ack();
    if (strcmp(argv[1], "install-failure") == 0) return test_install_failure();
    if (strcmp(argv[1], "bundled-supervisor-digest-mismatch") == 0)
        return test_bundled_supervisor_metadata_mismatch(
            FAKE_BUNDLED_DIGEST_MISMATCH, "bundled supervisor digest mismatch");
    if (strcmp(argv[1], "bundled-supervisor-path-mismatch") == 0)
        return test_bundled_supervisor_metadata_mismatch(
            FAKE_BUNDLED_PATH_MISMATCH, "bundled supervisor path mismatch");
    if (strcmp(argv[1], "custom-supervisor") == 0) return test_custom_supervisor_path();
    if (strcmp(argv[1], "boot-null-output") == 0) return test_boot_requires_output_handle();
    if (strcmp(argv[1], "stdin-close-order") == 0) return test_stdin_close_order();
    if (strcmp(argv[1], "control-frame-limit") == 0) return test_control_frame_queue_limit();
    if (strcmp(argv[1], "control-critical-close") == 0) return test_control_critical_close_reserve();
    if (strcmp(argv[1], "control-same-session-close") == 0) return test_control_same_session_close_bound();
    if (strcmp(argv[1], "control-exited-same-session-close") == 0) return test_control_exited_same_session_close_bound();
    if (strcmp(argv[1], "signal-close-order") == 0)
        return test_session_control_close_order(
            RETAINED_CONTROL_SIGNAL, ISH_FT_SIGNAL, "signal");
    if (strcmp(argv[1], "resize-close-order") == 0)
        return test_session_control_close_order(
            RETAINED_CONTROL_RESIZE, ISH_FT_RESIZE, "resize");
    if (strcmp(argv[1], "terminate-close-order") == 0)
        return test_session_control_close_order(
            RETAINED_CONTROL_TERMINATE, ISH_FT_TERMINATE, "terminate");
    if (strcmp(argv[1], "control-critical-oneshot") == 0) return test_control_critical_oneshot_bound();
    if (strcmp(argv[1], "control-preblocked-oneshot") == 0) return test_control_preblocked_oneshot_spawn();
    if (strcmp(argv[1], "control-byte-limit") == 0) return test_control_byte_queue_limit();
    if (strcmp(argv[1], "control-byte-reserve") == 0) return test_control_byte_reserve();
    if (strcmp(argv[1], "control-spawn-gate") == 0) return test_control_spawn_staging_gate();
    if (strcmp(argv[1], "control-oneshot-spawn-lock") == 0) return test_control_oneshot_spawn_lock_deadline();
    if (strcmp(argv[1], "control-streaming-spawn-lock") == 0) return test_control_streaming_spawn_lock_deadline();
    if (strcmp(argv[1], "control-finite-streaming") == 0) return test_control_finite_streaming_admission();
    if (strcmp(argv[1], "control-finite-streaming-write") == 0) return test_control_finite_streaming_write_busy();
    if (strcmp(argv[1], "control-streaming-queue-deadline") == 0) return test_control_streaming_queue_deadline();
    if (strcmp(argv[1], "control-streaming-precommit-deadline") == 0)
        return test_control_streaming_precommit_deadline();
    if (strcmp(argv[1], "streaming-instance-gate") == 0) return test_streaming_instance_gate_deadline();
    if (strcmp(argv[1], "supervisor-error") == 0) return test_supervisor_error_status();
    if (strcmp(argv[1], "close-race") == 0) return test_close_race();
    if (strcmp(argv[1], "backlog") == 0) return test_backlog_limit();
    if (strcmp(argv[1], "frame-backlog") == 0) return test_frame_backlog_limit();
    if (strcmp(argv[1], "backlog-control-pressure") == 0) return test_backlog_control_pressure();
    if (strcmp(argv[1], "borrow-shutdown") == 0) return test_borrow_blocks_shutdown();
    if (strcmp(argv[1], "double-shutdown") == 0) return test_double_shutdown();
    if (strcmp(argv[1], "active-call") == 0) return test_active_call_blocks_shutdown();
    if (strcmp(argv[1], "broken-control") == 0) return test_broken_control_pipe();
    if (strcmp(argv[1], "protocol-fatal") == 0) return test_protocol_fatal_drain();
    if (strcmp(argv[1], "protocol-fatal-control-pressure") == 0) return test_protocol_fatal_control_pressure();
    if (strcmp(argv[1], "output-allocation-failure") == 0) return test_output_allocation_failure();
    if (strcmp(argv[1], "spawn-argument-bound") == 0) return test_spawn_argument_bound();
    if (strcmp(argv[1], "shutdown-drain") == 0) return test_shutdown_drain();
    if (strcmp(argv[1], "log-backpressure") == 0) return test_log_sink_backpressure();
    if (strcmp(argv[1], "oneshot-timeout") == 0) return test_oneshot_hard_timeout();
    if (strcmp(argv[1], "oneshot-output") == 0) return test_oneshot_output_limit();
    fprintf(stderr, "unknown test: %s\n", argv[1]);
    return 2;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ishembed.h — public C ABI for embedding iSH as a runtime in a host process.
 *
 * One iSH instance per host process. The kernel uses process-global / TLS
 * state, so do not attempt to create more than one IshInstance.
 *
 * Threading model:
 *   - Boot is synchronous on whatever thread calls ish_embed_boot().
 *   - The kernel task is scheduled on a dedicated pthread (created
 *     internally), driven by task_run_current().
 *   - All host API calls below are safe to invoke from any host thread
 *     after ish_embed_boot() returns 0.
 *
 * Stdio model:
 *   - The guest's stdin/stdout/stderr are wired to host pipes that carry
 *     a framed multiplexed protocol (see protocol/proto.h).
 *   - The host never touches the process-global stdin/stdout/stderr.
 *
 * Concurrency:
 *   - Many concurrent sessions are supported. A hung child cannot block
 *     other sessions or the host: the supervisor poll()s per-child pipes
 *     and dispatches on session_id.
 */

#ifndef ISHEMBED_H
#define ISHEMBED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISH_EMBED_ABI_VERSION 1

/* Native protocol/inbox safety ceilings. The reader drains and drops output
 * after a session reaches either backlog ceiling, then reports
 * ISH_ERR_OUTPUT_LIMIT after already-accepted frames are consumed. The control
 * ceilings count complete wire bytes and frames from admission through actual
 * free, including the writer's current in-flight frame. A small portion of the
 * total control budget is unavailable to ordinary calls so internal lifecycle
 * frames can still be admitted without making the total allocation unbounded. */
#define ISH_EMBED_MAX_PROTOCOL_FRAME_BYTES    (1024u * 1024u)
#define ISH_EMBED_MAX_SESSION_BACKLOG_BYTES   (4u * 1024u * 1024u)
#define ISH_EMBED_MAX_SESSION_BACKLOG_FRAMES  4096u
#define ISH_EMBED_MAX_ONESHOT_STDOUT_BYTES    (8u * 1024u * 1024u)
#define ISH_EMBED_MAX_ONESHOT_STDERR_BYTES    (4u * 1024u * 1024u)
#ifndef ISH_EMBED_MAX_CONTROL_QUEUE_BYTES
#define ISH_EMBED_MAX_CONTROL_QUEUE_BYTES     (4u * 1024u * 1024u)
#endif
#ifndef ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES
#define ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES    256u
#endif
#ifndef ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES
#define ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES  (4u * 1024u)
#endif
#ifndef ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES
#define ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES 16u
#endif

/* SIGNAL is the largest lifecycle-class control frame: 12-byte protocol
 * header plus a 4-byte payload. Keep the byte reserve large enough for at
 * least one such frame under every build-time queue configuration. */
#define ISH_EMBED_MIN_CONTROL_CRITICAL_RESERVE_BYTES 16u

#if ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES == 0 || \
    ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES >= ISH_EMBED_MAX_CONTROL_QUEUE_BYTES
#error "critical control byte reserve must be nonzero and below the total budget"
#endif
#if ISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES < \
    ISH_EMBED_MIN_CONTROL_CRITICAL_RESERVE_BYTES
#error "critical control byte reserve must fit the largest lifecycle frame"
#endif
#if ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES == 0 || \
    ISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES >= ISH_EMBED_MAX_CONTROL_QUEUE_FRAMES
#error "critical control frame reserve must be nonzero and below the total budget"
#endif

typedef struct ish_embed_instance ish_embed_instance_t;
typedef struct ish_embed_session ish_embed_session_t;

typedef enum {
    ISH_OK                  = 0,
    ISH_ERR_BOOT            = -1,  /* generic boot failure                 */
    ISH_ERR_MOUNT           = -2,  /* fakefs mount failed                  */
    ISH_ERR_BECOME_INIT     = -3,  /* become_first_process failed          */
    ISH_ERR_STDIO           = -4,  /* stdio install failed                 */
    ISH_ERR_CHDIR           = -5,
    ISH_ERR_EXECVE          = -6,  /* PID1 supervisor exec failed          */
    ISH_ERR_PIPE            = -7,
    ISH_ERR_THREAD          = -8,
    ISH_ERR_NOT_RUNNING     = -9,
    ISH_ERR_ALREADY_BOOTED  = -10,
    ISH_ERR_PROTOCOL        = -11,
    ISH_ERR_TIMEOUT         = -12,
    ISH_ERR_INVALID_ARG     = -13,
    ISH_ERR_NO_SESSION      = -14,
    ISH_ERR_SUPERVISOR      = -15, /* supervisor reported error frame      */
    ISH_ERR_OOM             = -16,
    ISH_ERR_BROKEN_PIPE     = -17,
    ISH_ERR_OUTPUT_LIMIT    = -18, /* native session backlog ceiling reached */
    ISH_ERR_BUSY            = -19, /* operation cannot proceed while state busy */
    ISH_ERR_SUPERVISOR_INSTALL = -20, /* bundled PID 1 install/verification failed */
    ISH_ERR_CONTROL_LIMIT   = -21, /* host-to-guest control queue ceiling reached */
    ISH_ERR_UNSUPPORTED     = -22, /* feature is unavailable in the linked native binary */
    ISH_ERR_INTERNAL        = -99,
} ish_embed_status_t;

typedef struct ish_embed_boot_opts {
    const char *rootfs_path;            /* writable host path to fakefs root (the dir with data/ + meta.db) */
    const char *workdir;                /* guest cwd for PID1 (e.g. "/")                                  */
    /* In a release XCFramework, NULL verifies the SHA-256 of the actual
     * embedded PID 1 bytes against generated metadata before atomically
     * installing them at a private content-addressed path. It also verifies
     * the installed bytes and mode, and does not replace a RootFS-owned
     * /sbin/ishsv. SHA-256 here is an integrity check, not a signature or
     * authentication of source/provenance. Source builds without a bundled
     * blob retain /sbin/ishsv as a compatibility fallback. A non-NULL
     * override is executed as-is and skips bundled installation. */
    const char *supervisor_guest_path;
    /* Optional best-effort supervisor/kernel log sink. The runtime duplicates
     * this descriptor during boot, never takes ownership of the caller's fd,
     * and may drop log bytes instead of blocking protocol progress. -1 selects
     * a private duplicate of stderr; an unavailable sink disables logs. */
    int kernel_log_fd;
    int reserved_flags;
} ish_embed_boot_opts_t;

/* Boot the kernel. out_instance is required and receives the only reachable
 * instance handle. Returns ISH_OK on success. After this returns 0, the
 * supervisor is running as PID 1 and ready to accept SPAWN frames. */
int ish_embed_boot(const ish_embed_boot_opts_t *opts,
                   ish_embed_instance_t **out_instance);

/* Spawn options for a single command/session. */
typedef struct ish_embed_spawn_opts {
    const char *const *argv;     /* NULL-terminated                                                   */
    const char *cwd;             /* NULL = "/"                                                        */
    const char *const *envp;     /* NULL-terminated "K=V" array, NULL = inherit minimal default       */
    int allocate_tty;            /* 1 = guest sees a TTY (rare)                                       */
    int merge_stderr_into_stdout;/* 1 = guest dup2(stdout, stderr); host receives only STDOUT events  */
    uint32_t timeout_ms;         /* 0 = legacy synchronous streaming controls; nonzero also bounds
                                  * run_oneshot and streaming SPAWN admission                         */
    /* If non-NULL/non-empty, the child chroot()s to this guest path
     * before exec. Useful for VM-style isolation: each VM is a directory
     * tree under e.g. /srv/vms/<name>/ in the shared fakefs. */
    const char *chroot_path;
    int reserved_flags;
    /* Initial pty winsize. Only honored when allocate_tty != 0; pipe
     * spawns ignore it. 0 = use supervisor default (24x80 for rows /
     * cols, unknown for pixels). This field was introduced in proto v3;
     * the current host and supervisor require the same exact wire version. */
    uint16_t init_rows;
    uint16_t init_cols;
    uint16_t init_xpixel;
    uint16_t init_ypixel;
} ish_embed_spawn_opts_t;

typedef struct ish_embed_oneshot_result {
    int32_t exit_code;
    int32_t signal;        /* nonzero if killed by signal */
    uint8_t *stdout_buf;   /* malloc'd; free with ish_embed_free                                  */
    size_t   stdout_len;
    uint8_t *stderr_buf;   /* malloc'd; free with ish_embed_free; NULL/0 if merge_stderr_into_stdout */
    size_t   stderr_len;
    int32_t  timed_out;
} ish_embed_oneshot_result_t;

/* Run a single command and return after it exits (or timeout). A nonzero
 * timeout starts at API entry and includes waiting for internal SPAWN staging
 * and control-queue admission. If cleanup cannot be admitted or reaped within
 * its bounded grace period, the instance is stopped so no unowned guest
 * command can survive the call. */
int ish_embed_run_oneshot(ish_embed_instance_t *inst,
                          const ish_embed_spawn_opts_t *opts,
                          ish_embed_oneshot_result_t *out_result);

/* Atomically rename one guest-absolute path without replacing an existing
 * destination. The operation runs inside the guest through the exact
 * supervisor selected at boot; it does not invoke a shell or compose a
 * check-then-rename sequence.
 *
 * Returns an ISH_* transport/lifecycle status. On ISH_OK,
 * *out_guest_errno is 0 for success or a positive Linux errno (for example,
 * EEXIST=17 when destination already exists). timeout_ms uses the same
 * API-entry deadline semantics as run_oneshot; 0 retains the unbounded legacy
 * behavior. source and destination must be non-empty guest-absolute paths. */
int ish_embed_rename_noreplace(ish_embed_instance_t *inst,
                               const char *source,
                               const char *destination,
                               uint32_t timeout_ms,
                               int32_t *out_guest_errno);

void ish_embed_free(void *p);

/* Spawn and return a session handle for streaming I/O. With timeout_ms == 0,
 * SPAWN and later controls retain legacy synchronous delivery: ISH_OK means
 * the complete frame reached the supervisor pipe. A finite timeout starts at
 * API entry, includes waiting for the SPAWN staging gate, and returns after
 * ordered queue admission. That finite session also admits stdin writes,
 * stdin-close, and terminate asynchronously, so a stalled writer cannot
 * consume the product deadline. If a concurrent stdin write owns the ordering
 * gate, stdin-close returns ISH_ERR_BUSY instead of waiting behind that write;
 * retry it after the writer finishes or continue with terminate/close. Read
 * the authoritative EXITED event before treating termination as complete. */
int ish_embed_spawn(ish_embed_instance_t *inst,
                    const ish_embed_spawn_opts_t *opts,
                    ish_embed_session_t **out_session);

/* Borrow a session across a concurrent API call. The handle returned by
 * ish_embed_spawn owns one reference. ish_embed_session_close consumes that
 * owner reference; every successful retain must be paired with release.
 * Retain/release do not make it valid to start a new borrow after close. */
int ish_embed_session_retain(ish_embed_session_t *s);
void ish_embed_session_release(ish_embed_session_t *s);

/* Read available stdout/stderr bytes. Blocks up to wait_ms (0 = nonblock,
 * UINT32_MAX = wait forever). Output:
 *   *out_kind: 1=stdout, 2=stderr, 3=session_exited
 *   *out_seq:  monotonic per-session sequence number
 * If session_exited, *exit_code and *signal are set, and the session
 * handle is now drained — call ish_embed_session_close to free it. */
int ish_embed_session_read(ish_embed_session_t *s,
                           uint32_t wait_ms,
                           uint8_t **out_buf,    /* malloc'd; free with ish_embed_free; NULL on session_exited */
                           size_t   *out_len,
                           int      *out_kind,
                           uint64_t *out_seq,
                           int32_t  *out_exit_code,
                           int32_t  *out_signal);

/* Queue stdin bytes for ordered delivery. The host transport splits large
 * writes into bounded frames; guest PID 1 maintains a 1 MiB per-session stdin
 * queue and handles partial nonblocking child writes. Queue overflow or a hard
 * child write error is terminal for that session. After close_stdin, writes
 * return ISH_ERR_BROKEN_PIPE. A finite-timeout session reuses the original
 * streaming-SPAWN admission deadline for its stdin lock and every frame
 * admission, returning ISH_ERR_TIMEOUT rather than waiting past that deadline.
 * Zero-timeout sessions preserve legacy synchronous pipe-delivery semantics.
 * If a call fails after admitting an earlier chunk, that prefix remains
 * ordered for delivery; callers needing transactional input must stage it. */
int ish_embed_session_write(ish_embed_session_t *s,
                            const uint8_t *buf, size_t len);

/* Queue stdin bytes with a deadline relative to this API entry. timeout_ms
 * must be nonzero. The effective deadline is the earlier of this call's
 * deadline and the session's original finite SPAWN deadline, if any. Unlike
 * the legacy write entry point, this remains bounded even for a session
 * spawned with timeout_ms == 0. A timeout may follow successful admission of
 * an earlier frame from the same call. */
int ish_embed_session_write_timeout(ish_embed_session_t *s,
                                    const uint8_t *buf, size_t len,
                                    uint32_t timeout_ms);

/* Outbound operations on one retained session are synchronized with close.
 * For legacy zero-timeout sessions, a successful write, signal, resize,
 * terminate, or stdin close is completely written before SESSION_CLOSE. For a
 * finite-timeout session, writes, stdin close, and terminate are instead
 * completely admitted in queue order before SESSION_CLOSE; stdin close returns
 * ISH_ERR_BUSY when an active write prevents immediate ordered admission.
 * Calls that overlap close either finish first or fail without reporting a
 * later control frame as admitted. */

/* Send a Unix signal to the tracked command's process group. signum is the
 * standard Linux signal number (SIGINT=2, SIGTERM=15, ...). For a TTY shell,
 * this direct operation deliberately does not retarget a foreground job;
 * terminal-control signals should be written as control bytes instead. */
int ish_embed_session_signal(ish_embed_session_t *s, int signum);

/* Resize the session's pty (if any) and deliver SIGWINCH to the
 * foreground process group. Pipe sessions silently accept and ignore.
 * `xpixel` / `ypixel` are informational; pass 0 if unknown. */
int ish_embed_session_resize(ish_embed_session_t *s,
                              uint16_t rows, uint16_t cols,
                              uint16_t xpixel, uint16_t ypixel);

/* SIGTERM, then SIGKILL after the supervisor's fixed ~1.5s interval. For TTY
 * sessions this targets both the tracked shell group and its current foreground
 * job group. grace_ms is retained for source compatibility and is ignored. */
int ish_embed_session_terminate(ish_embed_session_t *s, uint32_t grace_ms);

/* Close stdin (EOF). A session created with finite timeout_ms reuses the
 * original streaming-SPAWN admission deadline; expiry returns ISH_ERR_TIMEOUT
 * without publishing a late STDIN_CLOSE frame. */
int ish_embed_session_close_stdin(ish_embed_session_t *s);

/* Close stdin with a nonzero deadline relative to this API entry. The
 * effective deadline is the earlier of this call's deadline and the session's
 * original finite SPAWN deadline. A timeout does not publish a late EOF and
 * leaves stdin open so the caller can retry or terminate the session. */
int ish_embed_session_close_stdin_timeout(ish_embed_session_t *s,
                                          uint32_t timeout_ms);

/* Close the session and consume its owner reference. If the tracked command is
 * still running, the supervisor force-closes its transport, terminates the
 * tracked group plus any TTY foreground job, and the host waits up to 1s for
 * reap. SESSION_CLOSE uses reserved, asynchronous control capacity so a blocked
 * writer cannot trap this void API indefinitely. If admission fails or reap is
 * not observed within 1s, the instance enters shutdown and closes the control
 * direction; this prevents a live guest command from outliving its last host
 * handle. Existing retained calls may finish safely; new calls fail with
 * ISH_ERR_NO_SESSION. */
void ish_embed_session_close(ish_embed_session_t *s);

/* Politely shut down the supervisor and join the kernel pthread. All sessions
 * must be closed and all instance calls must have returned first, otherwise
 * ISH_ERR_BUSY is returned. After successful shutdown, the IshInstance is
 * invalidated; you cannot boot another one in this process. ISH_ERR_TIMEOUT or
 * another terminal cleanup error leaves ordinary instance admission closed but
 * preserves the handle so the caller can retry shutdown. */
int ish_embed_shutdown(ish_embed_instance_t *inst, uint32_t grace_ms);

/* Legacy compatibility entry point. It validates a live instance and a
 * non-empty guest-absolute vm_root, but performs no filesystem operation.
 * Before every SPAWN using an absolute chroot_path, PID 1 revalidates device
 * nodes, devpts/procfs, and the conventional root home, repairing missing
 * state where safe. This legacy function itself does not check whether vm_root
 * already exists. */
int ish_embed_setup_vm_root(ish_embed_instance_t *inst, const char *vm_root);

const char *ish_embed_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif /* ISHEMBED_H */

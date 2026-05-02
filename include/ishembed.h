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
 *     a framed multiplexed protocol (see embed/protocol/proto.h).
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
    ISH_ERR_INTERNAL        = -99,
} ish_embed_status_t;

typedef struct ish_embed_boot_opts {
    const char *rootfs_path;            /* host fs path to fakefs root (the dir that has data/ + meta.db) */
    const char *workdir;                /* guest cwd for PID1 (e.g. "/")                                  */
    const char *supervisor_guest_path;  /* guest path to PID1 binary (default: "/sbin/ishsv")             */
    int kernel_log_fd;                  /* host fd that receives iSH printk; -1 to send to stderr         */
    int reserved_flags;
} ish_embed_boot_opts_t;

/* Boot the kernel. Returns ISH_OK on success. After this returns 0, the
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
    uint32_t timeout_ms;         /* 0 = no timeout (only honored by run_oneshot)                      */
    /* If non-NULL/non-empty, the child chroot()s to this guest path
     * before exec. Useful for VM-style isolation: each VM is a directory
     * tree under e.g. /srv/vms/<name>/ in the shared fakefs. */
    const char *chroot_path;
    int reserved_flags;
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

/* Run a single command and return after it exits (or timeout). */
int ish_embed_run_oneshot(ish_embed_instance_t *inst,
                          const ish_embed_spawn_opts_t *opts,
                          ish_embed_oneshot_result_t *out_result);

void ish_embed_free(void *p);

/* Spawn and return a session handle for streaming I/O. */
int ish_embed_spawn(ish_embed_instance_t *inst,
                    const ish_embed_spawn_opts_t *opts,
                    ish_embed_session_t **out_session);

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

int ish_embed_session_write(ish_embed_session_t *s,
                            const uint8_t *buf, size_t len);

/* Send a Unix signal to the session's process group. signum is the standard
 * Linux signal number (SIGINT=2, SIGTERM=15, ...). */
int ish_embed_session_signal(ish_embed_session_t *s, int signum);

/* SIGTERM, then SIGKILL after grace_ms. */
int ish_embed_session_terminate(ish_embed_session_t *s, uint32_t grace_ms);

/* Close stdin (EOF). */
int ish_embed_session_close_stdin(ish_embed_session_t *s);

/* Free the session handle. If the child is still running, sends SIGKILL
 * and waits up to 1s for reap. */
void ish_embed_session_close(ish_embed_session_t *s);

/* Politely shut down the supervisor and join the kernel pthread.
 * After this, the IshInstance is invalidated; you cannot boot another
 * one in this process. */
int ish_embed_shutdown(ish_embed_instance_t *inst, uint32_t grace_ms);

/* Initialise device nodes + devpts mount inside a VM subtree so that
 * processes chrooted into <vm_root> can allocate ptys. Call once per
 * newly-created VM, after the VM's directory tree has been populated.
 * vm_root is a guest-absolute path like "/srv/vms/playground". */
int ish_embed_setup_vm_root(ish_embed_instance_t *inst, const char *vm_root);

const char *ish_embed_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif /* ISHEMBED_H */

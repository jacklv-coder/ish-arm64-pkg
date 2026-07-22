/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stable C ABI between the embedding host and the iSH kernel internals.
 * Only this header is allowed to expose iSH symbols to the embed/host
 * layer; iSH internal headers (kernel/init.h, fs/...) MUST NOT be
 * pulled into bindgen / Swift modulemap.
 *
 * All functions return 0 on success or a negative iSH errno-like value.
 * Strings are null-terminated UTF-8.
 *
 * Threading: ish_ffi_mount_fakefs / ish_ffi_become_init / ish_ffi_install_pipe_stdio /
 * ish_ffi_install_executable / ish_ffi_chdir / ish_ffi_execve MUST run on the
 * thread that will be promoted to PID1 (typically the kernel pthread before
 * task_run_current() starts the loop).
 *
 * ish_ffi_task_start spawns the dedicated kernel pthread that runs
 * task_run_current() forever. After that, the host must communicate
 * with the guest only through the pipe fds installed by
 * ish_ffi_install_pipe_stdio.
 */

#ifndef ISH_FFI_H
#define ISH_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount fakefs at the root. rootfs_path is the host path to the fakefs
 * directory (the parent of data/ and the meta.db sqlite file). */
int ish_ffi_mount_fakefs(const char *rootfs_path);

/* Become PID 1 (construct task, install signal handlers). */
int ish_ffi_become_init(void);

/* Install pipe-backed stdio into PID1's fd table.
 * Each argument is a host fd that the kernel will treat as guest 0/1/2.
 * The kernel takes ownership of these fds — do not close them on the host.
 * Returns 0 on success. */
int ish_ffi_install_pipe_stdio(int in_rd, int out_wr_a, int out_wr_b);

/* chdir the current task. */
int ish_ffi_chdir(const char *guest_path);

/* Install an executable into fakefs using a same-directory temporary file,
 * fsync, checked metadata update, rename, and full read-back verification.
 * The final path is never truncated in place. `mode` is the Unix mode bits
 * (e.g. 0755). */
int ish_ffi_install_executable(const char *guest_path,
                               const uint8_t *bytes, size_t len,
                               uint32_t mode);

/* Create essential device nodes (/dev/null, tty, ptmx, etc.) under the
 * real fs root, and mount devpts at /dev/pts. This startup-only entry point
 * must run after ish_ffi_become_init and before ish_ffi_task_start. */
int ish_ffi_create_devices(void);

/* execve the supervisor as PID1. argv_packed and envp_packed are
 * NUL-separated, NUL-terminated buffers (i.e. "arg0\0arg1\0\0").
 * The kernel does the heavy lifting; this just calls do_execve. */
int ish_ffi_execve(const char *guest_path,
                   size_t argc, const char *argv_packed,
                   const char *envp_packed);

/* Spawn the dedicated, joinable pthread that runs task_run_current(). */
int ish_ffi_task_start(void);

/* Join the dedicated kernel pthread after the PID 1 exit callback fires. */
int ish_ffi_task_join(void);

/* Register a callback fired when PID1 exits. The callback is invoked
 * from the kernel pthread; do not block. */
typedef void (*ish_ffi_exit_cb)(int exit_code, void *ctx);
void ish_ffi_register_exit_hook(ish_ffi_exit_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ISH_FFI_H */

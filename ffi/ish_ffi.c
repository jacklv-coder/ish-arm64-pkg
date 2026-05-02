/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implementation of the embed FFI shims. This file lives outside the
 * iSH source tree and is added to libish's source list via meson.
 *
 * It is the ONLY file that includes both <ish_ffi.h> and iSH internal
 * headers. All other embed/ code talks to iSH only through this file.
 */

#define _GNU_SOURCE
/* System headers FIRST so libc types (timespec, clockid_t, ...) are
 * fully defined before iSH internal headers redeclare wrapper types.
 *
 * Order matters on macOS: <pthread.h> forward-declares struct timespec
 * but does not pull in <time.h>; if iSH headers reach for `struct
 * timespec` later they need to find a complete definition. We pull in
 * <time.h> + <sys/time.h> + <sys/types.h> first so libc types are
 * complete by the time pthread.h / iSH headers see them. */
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* iSH internals — order matters: fs/fd.h must come before kernel/task.h
 * (which transitively pulls kernel/time.h, which references fd_t).
 * Avoid kernel/calls.h directly; we forward-decl the few symbols we need. */
#include "misc.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "kernel/init.h"

/* Forward decls of the only iSH calls.h symbols we actually use. */
extern int do_execve(const char *file, size_t argc, const char *argv, const char *envp);
extern void task_run_current(void);
struct task;
extern void (*exit_hook)(struct task *task, int code);

/* errno_map / IS_ERR / PTR_ERR live in kernel/errno.h via misc.h - it's
 * already pulled by misc.h chain. */

#include "ish_ffi.h"

/* ------------------------------------------------------------------ *
 *  exit hook glue                                                    *
 * ------------------------------------------------------------------ */

static ish_ffi_exit_cb g_exit_cb = NULL;
static void           *g_exit_ctx = NULL;

static void embed_exit_handler(struct task *task, int code) {
    /* Match iSH convention: code is (status<<8) | (signal&0xff). */
    if (task->parent != NULL) return;
    int exit_code;
    if (code & 0xff) exit_code = 128 + (code & 0xff);
    else             exit_code = code >> 8;
    if (g_exit_cb) g_exit_cb(exit_code, g_exit_ctx);
}

void ish_ffi_register_exit_hook(ish_ffi_exit_cb cb, void *ctx) {
    g_exit_cb  = cb;
    g_exit_ctx = ctx;
    exit_hook  = embed_exit_handler;
}

/* ------------------------------------------------------------------ *
 *  mount fakefs                                                      *
 * ------------------------------------------------------------------ */

int ish_ffi_mount_fakefs(const char *rootfs_path) {
    if (!rootfs_path) return -EINVAL;
    /* The fakefs source path that iSH expects is <rootfs>/data
     * (matching xX_main_Xx behavior when fs == &fakefs). */
    char buf[MAX_PATH + 1];
    int n = snprintf(buf, sizeof(buf), "%s/data", rootfs_path);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return -ENAMETOOLONG;
    return mount_root(&fakefs, buf);
}

/* ------------------------------------------------------------------ *
 *  become PID 1                                                      *
 * ------------------------------------------------------------------ */

int ish_ffi_become_init(void) {
    int err = become_first_process();
    if (err < 0) return err;
    current->thread = pthread_self();
    return 0;
}

/* ------------------------------------------------------------------ *
 *  pipe-backed stdio                                                 *
 *                                                                    *
 *  This mirrors create_piped_stdio() (kernel/init.c) but takes       *
 *  caller-provided fds instead of inheriting STDIN/OUT/ERR_FILENO,   *
 *  so the host process keeps its real stdio intact.                  *
 * ------------------------------------------------------------------ */

extern const struct fd_ops realfs_fdops;
struct fd *adhoc_fd_create(const struct fd_ops *ops);

static struct fd *open_fd_from_real(int real_fd) {
    struct fd *fd = adhoc_fd_create((const struct fd_ops *)&realfs_fdops);
    if (!fd) return NULL;
    fd->real_fd = real_fd;
    fd->dir = NULL;
    struct stat st;
    if (fstat(real_fd, &st) == 0) {
        fd->stat.mode  = st.st_mode;
        fd->stat.rdev  = dev_fake_from_real(st.st_rdev);
        fd->stat.inode = st.st_ino;
        fd->stat.size  = st.st_size;
        /* Host pipes appear as FIFOs to fstat. Surface them to the guest
         * as character devices so libuv-style runtimes don't reject the
         * fd on uv_guess_handle. */
        if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
            fd->stat.mode = S_IFCHR | 0620;
            fd->stat.rdev = dev_make(TTY_PSEUDO_SLAVE_MAJOR, 0);
        }
    }
    return fd;
}

int ish_ffi_install_pipe_stdio(int in_rd, int out_wr_a, int out_wr_b) {
    if (in_rd < 0 || out_wr_a < 0 || out_wr_b < 0) return -EINVAL;
    /* iSH FD limit is around 256 inside the kernel; do NOT F_DUPFD with
     * a high min, that breaks with EINVAL. */
    struct fd *f0 = open_fd_from_real(in_rd);
    if (!f0) return -ENOMEM;
    struct fd *f1 = open_fd_from_real(out_wr_a);
    if (!f1) { fd_close(f0); return -ENOMEM; }
    struct fd *f2 = open_fd_from_real(out_wr_b);
    if (!f2) { fd_close(f0); fd_close(f1); return -ENOMEM; }
    current->files->files[0] = f0;
    current->files->files[1] = f1;
    current->files->files[2] = f2;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  chdir                                                             *
 * ------------------------------------------------------------------ */

int ish_ffi_chdir(const char *guest_path) {
    if (!guest_path || !guest_path[0]) return -EINVAL;
    struct fd *pwd = generic_open(guest_path, O_RDONLY_, 0);
    if (IS_ERR(pwd)) return (int)PTR_ERR(pwd);
    fs_chdir(current->fs, pwd);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  install device nodes                                              *
 * ------------------------------------------------------------------ */

extern const struct fs_ops devptsfs;
extern int do_mount(const struct fs_ops *fs, const char *source,
                    const char *point, const char *info, int flags);

/* Idempotently create the standard device nodes under <root>/dev/ and
 * mount devpts at <root>/dev/pts. Called both for the real fs root
 * (during boot) and for every per-VM directory that will host chrooted
 * processes.
 *
 * Path argument is a guest-visible mount-absolute prefix (e.g. "" for
 * the real root, or "/srv/vms/playground" for VM 'playground'). The
 * trailing slash must be absent.
 */
static int create_devices_under(const char *prefix) {
    char path[MAX_PATH];

    #define MK(suffix, mode, dev) do {                            \
        snprintf(path, sizeof(path), "%s" suffix, prefix);        \
        generic_mknodat(AT_PWD, path, (mode), (dev));             \
    } while (0)

    /* /dev itself. mknodat won't make a directory; use generic_mkdirat
     * on the actual prefix. The prefix's parents must already exist
     * (they come from the bundled fakefs). */
    {
        char dev_dir[MAX_PATH];
        snprintf(dev_dir, sizeof(dev_dir), "%s/dev", prefix);
        generic_mkdirat(AT_PWD, dev_dir, 0755);
        char pts_dir[MAX_PATH];
        snprintf(pts_dir, sizeof(pts_dir), "%s/dev/pts", prefix);
        generic_mkdirat(AT_PWD, pts_dir, 0755);
    }

    MK("/dev/null",    S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
    MK("/dev/zero",    S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
    MK("/dev/full",    S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
    MK("/dev/random",  S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
    MK("/dev/urandom", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
    MK("/dev/tty",     S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
    MK("/dev/console", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
    MK("/dev/ptmx",    S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));

    #undef MK

    /* Mount the kernel's devpts so /dev/pts/N nodes exist on demand
     * when posix_openpt allocates them. iSH's devptsfs is a synthetic
     * filesystem (no on-disk state), so this is cheap and safe to
     * call multiple times: mount_remove() / re-mount the same path
     * are idempotent inside iSH's mount table. */
    {
        char pts_dir[MAX_PATH];
        snprintf(pts_dir, sizeof(pts_dir), "%s/dev/pts", prefix);
        do_mount(&devptsfs, "devpts", pts_dir, "", 0);
    }
    return 0;
}

int ish_ffi_create_devices(void) {
    return create_devices_under("");
}

int ish_ffi_setup_vm_root(const char *vm_root) {
    if (!vm_root || !vm_root[0]) return -EINVAL;
    /* vm_root is something like "/srv/vms/playground" — guest absolute. */
    return create_devices_under(vm_root);
}

/* ------------------------------------------------------------------ *
 *  install_executable: write bytes into fakefs through generic_open  *
 * ------------------------------------------------------------------ */

int ish_ffi_install_executable(const char *guest_path,
                               const uint8_t *bytes, size_t len,
                               uint32_t mode) {
    if (!guest_path || !bytes) return -EINVAL;
    /* Make sure parent exists; we do a best-effort mkdir of /sbin etc. */
    /* Caller is expected to pre-create directories during rootfs build. */

    struct fd *fd = generic_open(guest_path, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode & 0777);
    if (IS_ERR(fd)) return (int)PTR_ERR(fd);

    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > 64 * 1024) chunk = 64 * 1024;
        if (!fd->ops || !fd->ops->write) {
            fd_close(fd);
            return -EINVAL;
        }
        ssize_t w = fd->ops->write(fd, bytes + written, chunk);
        if (w < 0) { fd_close(fd); return (int)w; }
        if (w == 0) { fd_close(fd); return -EIO; }
        written += (size_t)w;
    }
    fd_close(fd);

    /* set executable bits via the metadata-aware setattr path */
    struct attr a = { .type = attr_mode };
    a.mode = (mode_t_)(mode & 0777);
    generic_setattrat(AT_PWD, guest_path, a, false);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  execve PID1                                                       *
 * ------------------------------------------------------------------ */

int ish_ffi_execve(const char *guest_path,
                   size_t argc, const char *argv_packed,
                   const char *envp_packed) {
    if (!guest_path || !argv_packed) return -EINVAL;
    if (!envp_packed) envp_packed = "\0";
    return do_execve(guest_path, argc, argv_packed, envp_packed);
}

/* ------------------------------------------------------------------ *
 *  task_start                                                        *
 * ------------------------------------------------------------------ */

int ish_ffi_task_start(void) {
    task_start(current);
    return 0;
}

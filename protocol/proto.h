/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wire protocol between host and PID1 supervisor inside iSH.
 *
 * Transport:
 *   host_to_guest_pipe  -> guest stdin   (host writes frames)
 *   guest_to_host_pipe  <- guest stdout  (supervisor writes frames)
 *
 * Supervisor's stderr carries only opaque LOG bytes (free-form), used
 * for supervisor-internal diagnostics. It MUST NOT carry framed protocol
 * because some libcs print to stderr unframed before exec hand-off.
 *
 * Frame format (network byte order):
 *
 *     +-------+-------+-------+-------+
 *     | 0xE5  |  ver  | type  | flags |
 *     +-------+-------+-------+-------+
 *     |          payload_len          |  u32
 *     +-------------------------------+
 *     |          session_id           |  u32  (0 = control / instance-wide)
 *     +-------------------------------+
 *     |          payload (LE)         |  payload_len bytes
 *     +-------------------------------+
 *
 * Inside payloads, fixed-width ints are explicitly little-endian. The current
 * AArch64 musl supervisor is little-endian, and the encoding remains fixed for
 * protocol compatibility rather than depending on a native struct layout.
 *
 * The 12-byte header fields up to payload_len are big-endian so the
 * frame can be parsed identically on either side without endian guesswork
 * for sync detection.
 *
 * Compatibility is exact-version: ish_proto_parse_hdr rejects any header
 * version other than ISH_PROTO_VERSION, and HELLO/HELLO_ACK repeat the same
 * exact check. Version labels on payload fields below record their historical
 * introduction; they do not imply runtime interoperability with older peers.
 * Host and bundled supervisor must therefore be upgraded together.
 */

#ifndef ISH_EMBED_PROTO_H
#define ISH_EMBED_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define ISH_PROTO_MAGIC      0xE5u
#define ISH_PROTO_VERSION    4u
#define ISH_PROTO_HDR_SIZE   12   /* magic+ver+type+flags + u32 len + u32 sid  */
#define ISH_PROTO_MAX_PAYLOAD (1u * 1024u * 1024u)  /* 1 MiB hard cap */

/* Host -> Supervisor */
#define ISH_FT_HELLO         0x01
#define ISH_FT_SPAWN         0x02
#define ISH_FT_STDIN_DATA    0x03
#define ISH_FT_STDIN_CLOSE   0x04
#define ISH_FT_SIGNAL        0x05
#define ISH_FT_TERMINATE     0x06
#define ISH_FT_SHUTDOWN      0x07
#define ISH_FT_PING          0x08
#define ISH_FT_RESIZE        0x09  /* set pty winsize for a session (v3) */
#define ISH_FT_SESSION_CLOSE 0x0A  /* force-close the complete session (v4) */

/* Supervisor -> Host */
#define ISH_FT_HELLO_ACK     0x40
#define ISH_FT_SPAWNED       0x41
#define ISH_FT_STDOUT_DATA   0x42
#define ISH_FT_STDERR_DATA   0x43
#define ISH_FT_EXITED        0x44
#define ISH_FT_ERROR         0x45
#define ISH_FT_PONG          0x46
#define ISH_FT_LOG           0x7F
#define ISH_FT_SHUTDOWN_ACK  0x7E

/* Flags */
#define ISH_FF_TTY           (1u << 0)
#define ISH_FF_MERGE_STDERR  (1u << 1)
#define ISH_FF_SEQ_PRESENT   (1u << 2)  /* first 8 bytes of payload = u64 seq */

/* Stream kinds returned to the host API */
#define ISH_STREAM_STDOUT 1
#define ISH_STREAM_STDERR 2
#define ISH_STREAM_EXITED 3

/* SPAWN payload layout:
 *
 *   u32 cwd_len; u8 cwd[cwd_len]
 *   u32 argc;
 *   for i in 0..argc:  u32 len; u8 bytes[len]
 *   u32 envc;
 *   for i in 0..envc:  u32 len; u8 bytes[len]   (each entry = "K=V")
 *   u32 chroot_len; u8 chroot[chroot_len]       (0 = no chroot; v2+)
 *   u16 init_rows;  u16 init_cols;              (v3+ optional initial pty
 *   u16 init_xpix;  u16 init_ypix;               winsize; all-zero means
 *                                                "use supervisor default")
 *
 * Historical field evolution was append-only: chroot was added in v2 and the
 * winsize tail in v3. The current v4 host always emits both fields. The v4
 * decoder remains shape-tolerant when an internal test/fixture omits a known
 * tail, but exact header and HELLO checks reject actual v1/v2/v3 peers before
 * SPAWN, so this leniency is not cross-version negotiation.
 *
 * SPAWNED payload:
 *   u32 guest_pid
 *
 * STDOUT_DATA / STDERR_DATA payload:
 *   u64 seq                     (only if ISH_FF_SEQ_PRESENT)
 *   u8  bytes[..]
 *
 * EXITED payload:
 *   i32 exit_code
 *   i32 signal
 *
 * ERROR payload:
 *   i32 errno_value
 *   u32 msg_len; u8 msg[msg_len]
 *
 * SIGNAL payload:
 *   i32 signum
 *
 * SESSION_CLOSE payload:
 *   empty; force-closes all transports and, while the direct child remains an
 *   unreaped identity, terminates its tracked group plus a TTY foreground job.
 *
 * RESIZE payload (v3+):
 *   u16 rows
 *   u16 cols
 *   u16 xpixel   (informational; 0 = unknown)
 *   u16 ypixel   (informational; 0 = unknown)
 *
 *   On receipt the supervisor issues TIOCSWINSZ on the session's pty
 *   master, which causes the kernel tty layer to deliver SIGWINCH to
 *   the foreground process group. No reply frame is generated; the
 *   host fires-and-forgets.
 *
 * HELLO payload:
 *   u32 abi_version
 *   u8  proto_version
 *   u8  reserved[3]
 *   u32 greeting_len; u8 greeting[greeting_len]
 *
 * HELLO_ACK payload:
 *   u32 abi_version
 *   u8  proto_version
 *   u8  reserved[3]
 *   u32 max_concurrent
 */

/* Helpers — header-only, no allocation, safe to use from supervisor and host. */

static inline void ish_proto_pack_hdr(uint8_t hdr[ISH_PROTO_HDR_SIZE],
                                      uint8_t type, uint8_t flags,
                                      uint32_t payload_len, uint32_t session_id) {
    hdr[0] = ISH_PROTO_MAGIC;
    hdr[1] = ISH_PROTO_VERSION;
    hdr[2] = type;
    hdr[3] = flags;
    hdr[4] = (uint8_t)((payload_len >> 24) & 0xff);
    hdr[5] = (uint8_t)((payload_len >> 16) & 0xff);
    hdr[6] = (uint8_t)((payload_len >>  8) & 0xff);
    hdr[7] = (uint8_t)((payload_len >>  0) & 0xff);
    hdr[8] = (uint8_t)((session_id >> 24) & 0xff);
    hdr[9] = (uint8_t)((session_id >> 16) & 0xff);
    hdr[10]= (uint8_t)((session_id >>  8) & 0xff);
    hdr[11]= (uint8_t)((session_id >>  0) & 0xff);
}

static inline int ish_proto_parse_hdr(const uint8_t hdr[ISH_PROTO_HDR_SIZE],
                                      uint8_t *type, uint8_t *flags,
                                      uint32_t *payload_len, uint32_t *session_id) {
    if (hdr[0] != ISH_PROTO_MAGIC) return -1;
    if (hdr[1] != ISH_PROTO_VERSION) return -2;
    *type = hdr[2];
    *flags = hdr[3];
    *payload_len = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
                   ((uint32_t)hdr[6] <<  8) | ((uint32_t)hdr[7]);
    *session_id  = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                   ((uint32_t)hdr[10]<<  8) | ((uint32_t)hdr[11]);
    if (*payload_len > ISH_PROTO_MAX_PAYLOAD) return -3;
    return 0;
}

/* Little-endian payload helpers. */
static inline void ish_proto_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline uint16_t ish_proto_get_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void ish_proto_put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline void ish_proto_put_u64(uint8_t *p, uint64_t v) {
    for (int i=0;i<8;i++) p[i] = (uint8_t)(v >> (i*8));
}
static inline void ish_proto_put_i32(uint8_t *p, int32_t v) {
    ish_proto_put_u32(p, (uint32_t)v);
}
static inline uint32_t ish_proto_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint64_t ish_proto_get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i=0;i<8;i++) v |= ((uint64_t)p[i]) << (i*8);
    return v;
}
static inline int32_t ish_proto_get_i32(const uint8_t *p) {
    return (int32_t)ish_proto_get_u32(p);
}

#endif /* ISH_EMBED_PROTO_H */

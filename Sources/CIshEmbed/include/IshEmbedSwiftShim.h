#ifndef ISH_EMBED_SWIFT_SHIM_H
#define ISH_EMBED_SWIFT_SHIM_H

#include "ishembed.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SwiftPM source-target bridge. C embedders should call the public
 * ish_embed_rename_noreplace symbol directly from abi.7 or newer. */
int ish_embed_swift_rename_noreplace(ish_embed_instance_t *inst,
                                     const char *source,
                                     const char *destination,
                                     uint32_t timeout_ms,
                                     int32_t *out_guest_errno);

/* Swift source compatibility bridge for the bounded-write ABI. */
int ish_embed_swift_session_write_timeout(ish_embed_session_t *session,
                                          const uint8_t *buf,
                                          size_t len,
                                          uint32_t timeout_ms);
int ish_embed_swift_session_close_stdin_timeout(
    ish_embed_session_t *session,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

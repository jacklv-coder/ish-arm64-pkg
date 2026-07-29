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

#ifdef __cplusplus
}
#endif

#endif

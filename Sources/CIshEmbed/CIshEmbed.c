/* Source compatibility bridge for the short interval between merging a new
 * Swift API and publishing its matching XCFramework. The weak fallback keeps
 * Package.swift resolvable against the previous release; the bridge reports
 * unsupported until the new binary is linked. */

#include "ishembed.h"
#include "IshEmbedSwiftShim.h"

enum { ISH_SWIFT_ERR_UNSUPPORTED = -22 };

/* A weak definition keeps source builds linkable while Package.swift still
 * points at the previous XCFramework. The new archive contains the strong
 * implementation in the same member as the established runtime API, so it is
 * loaded normally and overrides this fallback after publication. */
__attribute__((weak))
int ish_embed_rename_noreplace(ish_embed_instance_t *inst,
                               const char *source,
                               const char *destination,
                               uint32_t timeout_ms,
                               int32_t *out_guest_errno) {
    (void)inst;
    (void)source;
    (void)destination;
    (void)timeout_ms;
    if (out_guest_errno) *out_guest_errno = 0;
    return ISH_SWIFT_ERR_UNSUPPORTED;
}

int ish_embed_swift_rename_noreplace(ish_embed_instance_t *inst,
                                     const char *source,
                                     const char *destination,
                                     uint32_t timeout_ms,
                                     int32_t *out_guest_errno) {
    return ish_embed_rename_noreplace(
        inst, source, destination, timeout_ms, out_guest_errno);
}

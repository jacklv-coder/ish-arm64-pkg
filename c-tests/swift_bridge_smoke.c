/* Link-only probe for the Swift source target's weak compatibility fallback.
 *
 * ish_embed_strerror forces the established host/ishembed archive member into
 * the final link. That same member contains the strong rename implementation,
 * which must override CIshEmbed.c's fallback definition.
 */

#include "ishembed.h"
#include "IshEmbedSwiftShim.h"

int main(void) {
    const char *status = ish_embed_strerror(ISH_OK);
    int32_t guest_errno = 0;
    int rename_rc = ish_embed_swift_rename_noreplace(
        NULL, "/source", "/destination", 1, &guest_errno);
    uint8_t byte = 0;
    int write_rc = ish_embed_swift_session_write_timeout(
        NULL, &byte, sizeof(byte), 1);
    int close_rc = ish_embed_swift_session_close_stdin_timeout(NULL, 1);
    return status != NULL &&
        rename_rc == ISH_ERR_INVALID_ARG &&
        write_rc == ISH_ERR_INVALID_ARG &&
        close_rc == ISH_ERR_INVALID_ARG ? 0 : 1;
}

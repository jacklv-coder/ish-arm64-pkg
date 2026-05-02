/* Pure C unit tests for the wire protocol header (no iSH dep). */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../protocol/proto.h"

static void test_pack_unpack_hdr(void) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    /* Use a valid payload size (under MAX_PAYLOAD) and an arbitrary session id. */
    ish_proto_pack_hdr(hdr, ISH_FT_SPAWN, ISH_FF_TTY|ISH_FF_SEQ_PRESENT,
                       12345u, 0xcafebabeu);

    uint8_t t, f;
    uint32_t plen, sid;
    int rc = ish_proto_parse_hdr(hdr, &t, &f, &plen, &sid);
    assert(rc == 0);
    assert(t == ISH_FT_SPAWN);
    assert(f == (ISH_FF_TTY | ISH_FF_SEQ_PRESENT));
    assert(plen == 12345u);
    assert(sid == 0xcafebabeu);
}

static void test_bad_magic(void) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE] = {0};
    hdr[1] = ISH_PROTO_VERSION;
    uint8_t t, f; uint32_t plen, sid;
    assert(ish_proto_parse_hdr(hdr, &t, &f, &plen, &sid) == -1);
}

static void test_bad_version(void) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, 1, 0, 0, 0);
    hdr[1] = 0xFF;
    uint8_t t, f; uint32_t plen, sid;
    assert(ish_proto_parse_hdr(hdr, &t, &f, &plen, &sid) == -2);
}

static void test_max_payload(void) {
    uint8_t hdr[ISH_PROTO_HDR_SIZE];
    ish_proto_pack_hdr(hdr, 1, 0, ISH_PROTO_MAX_PAYLOAD + 1, 0);
    uint8_t t, f; uint32_t plen, sid;
    assert(ish_proto_parse_hdr(hdr, &t, &f, &plen, &sid) == -3);
}

static void test_int_helpers(void) {
    uint8_t buf[8];
    ish_proto_put_u32(buf, 0x12345678u);
    assert(ish_proto_get_u32(buf) == 0x12345678u);
    ish_proto_put_i32(buf, -123);
    assert(ish_proto_get_i32(buf) == -123);
    ish_proto_put_u64(buf, 0x1122334455667788ull);
    assert(ish_proto_get_u64(buf) == 0x1122334455667788ull);
}

int main(void) {
    test_pack_unpack_hdr();
    test_bad_magic();
    test_bad_version();
    test_max_payload();
    test_int_helpers();
    printf("proto_test: OK\n");
    return 0;
}

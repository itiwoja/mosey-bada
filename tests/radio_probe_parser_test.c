/* Malformed/truncated netlink input must not escape the supplied buffer. */
#define main radio_probe_entry
#include "../src/fold5/radio_probe.c"
#undef main
#include <assert.h>
#include <stdlib.h>

int main(void) {
    unsigned char valid[8] = {0};
    struct nlattr *attr = (void *)valid;
    attr->nla_type = CTRL_ATTR_FAMILY_ID;
    attr->nla_len = NLA_HDRLEN + sizeof(uint16_t);
    uint16_t family = 42;
    memcpy(valid + NLA_HDRLEN, &family, sizeof(family));
    assert(parse_attrs(valid, sizeof(valid), 1) == 42);
    assert(parse_attrs(valid, 5, 1) == -1); /* truncated family ID */
    attr->nla_len = 3; /* shorter than attribute header */
    assert(parse_attrs(valid, sizeof(valid), 1) == -1);
    attr->nla_len = 65535; /* exceeds received message */
    assert(parse_attrs(valid, sizeof(valid), 1) == -1);
    assert(parse_attrs(valid, 0, 1) == -1);
    /* Each tail length gets its own allocation so ASan catches over-reads. */
    srand(246);
    for (size_t length = 1; length < 256; length++) {
        for (int repeat = 0; repeat < 16; repeat++) {
            unsigned char *bytes = malloc(length);
            assert(bytes);
            for (size_t n = 0; n < length; n++) bytes[n] = (unsigned char)rand();
            (void)parse_attrs(bytes, length, 1);
            free(bytes);
        }
    }
    puts("radio probe parser: truncation and malformed attribute tests passed");
    return 0;
}

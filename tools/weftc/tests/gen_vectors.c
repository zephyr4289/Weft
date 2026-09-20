/* gen_vectors.c — maintainer tool: prints WH64 reference vectors for
 * pinning in test_hash.c (and re-pinning when WEFTC_IR_VERSION bumps).
 *
 * Not part of the run.sh gate; exists so every pinned constant in the
 * test suite has a one-command provenance:
 *
 *     cc -O2 -std=c11 -Isrc -o /tmp/genv tests/gen_vectors.c \
 *        src/hash.c src/layout.c src/parse.c src/lex.c src/diag.c && /tmp/genv
 */
#include "weftc.h"

#include <string.h>

int main(void)
{
    static const char *vecs[] = {
        "", "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg",
        "abcdefgh", "abcdefghi",
        "The quick brown fox jumps over the lazy dog",
        "WABI-manifest-shaped-input-0123456789abcdef",
    };
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++)
        printf("WH64(\"%s\") = 0x%016llx\n", vecs[i],
               (unsigned long long)wh64((const uint8_t *)vecs[i],
                                        strlen(vecs[i])));
    /* explicit-length vectors (C strings cannot carry NUL bytes) */
    static const uint8_t z8[8] = { 0 };
    static const uint8_t f8[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    printf("WH64(00 x8) = 0x%016llx\n",
           (unsigned long long)wh64(z8, 8));
    printf("WH64(ff x8) = 0x%016llx\n",
           (unsigned long long)wh64(f8, 8));
    /* chunk-order sensitivity pairs */
    const char *x = "AAAABBBB", *y = "BBBBAAAA";
    printf("WH64(\"%s\") = 0x%016llx\n", x,
           (unsigned long long)wh64((const uint8_t *)x, 8));
    printf("WH64(\"%s\") = 0x%016llx\n", y,
           (unsigned long long)wh64((const uint8_t *)y, 8));
    return 0;
}

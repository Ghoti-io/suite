/* Cross-architecture probe for cutil's murmur3.  Prints, in a form that can be
 * diffed between targets:
 *
 *   1. the three SMHasher verification values;
 *   2. a per-length table of raw output BYTES (not integers -- printing an
 *      integer would hide the byte order, which is the thing under test);
 *   3. the same hash taken from an aligned and a deliberately misaligned key.
 *
 * Built against src/string.c directly, so no shared library or install is
 * involved on any target.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ghoti.io/cutil/string.h>

typedef void (*pfHash)(const void *, size_t, uint32_t, void *);

static uint32_t verification(pfHash hash, int hashbits) {
  const int hashbytes = hashbits / 8;
  uint8_t key[256], hashes[16 * 256], final[16];
  memset(key, 0, sizeof key);
  memset(hashes, 0, sizeof hashes);
  memset(final, 0, sizeof final);
  for (int i = 0; i < 256; i++) {
    key[i] = (uint8_t)i;
    hash(key, (size_t)i, (uint32_t)(256 - i), &hashes[i * hashbytes]);
  }
  hash(hashes, (size_t)(hashbytes * 256), 0, final);
  return ((uint32_t)final[0]) | ((uint32_t)final[1] << 8)
       | ((uint32_t)final[2] << 16) | ((uint32_t)final[3] << 24);
}

static void print_bytes(const char * label, const void * p, size_t n) {
  const uint8_t * b = (const uint8_t *)p;
  printf("%s", label);
  for (size_t i = 0; i < n; ++i) printf("%02x", b[i]);
  printf("\n");
}

int main(void) {
  printf("size_t=%d-bit\n", (int)(sizeof(size_t) * 8));

  printf("verify.murmur3_32      %08X\n", verification(gcu_string_murmur3_32, 32));
  printf("verify.murmur3_x86_128 %08X\n", verification(gcu_string_murmur3_x86_128, 128));
  printf("verify.murmur3_x64_128 %08X\n", verification(gcu_string_murmur3_x64_128, 128));

  static const char k[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static const size_t lens[] = { 0,1,3,4,7,8,15,16,17,32,36 };
  for (size_t i = 0; i < sizeof lens / sizeof *lens; ++i) {
    uint8_t o32[4], o86[16], o64[16];
    char label[64];
    gcu_string_murmur3_32(k, lens[i], 0, o32);
    gcu_string_murmur3_x86_128(k, lens[i], 0, o86);
    gcu_string_murmur3_x64_128(k, lens[i], 0, o64);
    sprintf(label, "len%02u.h32   ", (unsigned)lens[i]); print_bytes(label, o32, 4);
    sprintf(label, "len%02u.x86   ", (unsigned)lens[i]); print_bytes(label, o86, 16);
    sprintf(label, "len%02u.x64   ", (unsigned)lens[i]); print_bytes(label, o64, 16);
  }

  /* gcu_string_hash_64() picks its algorithm off SIZE_MAX, so this line is a
   * different function on a 32-bit target than on a 64-bit one. */
  printf("hash_64(\"hello\")       %016llX\n",
    (unsigned long long)gcu_string_hash_64("hello", 5));
  printf("hash_32(\"hello\")       %08X\n", gcu_string_hash_32("hello", 5));

  /* The alignment question.  Same bytes, odd address.  On a strict-alignment
   * target the old pointer-cast version does not return a wrong answer here --
   * it takes SIGBUS. */
  {
    static uint8_t buf[64];
    for (size_t i = 0; i < sizeof buf; ++i) buf[i] = (uint8_t)(i * 7 + 1);
    uint8_t a[16], m[16];
    gcu_string_murmur3_x64_128(buf, 32, 0, a);
    printf("aligned.x64            "); for (int i=0;i<16;++i) printf("%02x", a[i]); printf("\n");
    fflush(stdout);
    gcu_string_murmur3_x64_128(buf + 1, 31, 0, m);
    printf("misaligned.x64         "); for (int i=0;i<16;++i) printf("%02x", m[i]); printf("\n");
    fflush(stdout);
    /* As BYTES.  Printing `out` as an integer asks the host to interpret the
     * byte string the function wrote, so a byte-order change shows up here as
     * a difference even when the bytes are identical -- which is the probe
     * lying, not the library.  Every other line here prints bytes for the
     * same reason; this one did not, and said so. */
    uint8_t m32[4];
    gcu_string_murmur3_32(buf + 1, 31, 0, m32);
    print_bytes("misaligned.h32         ", m32, 4);
  }
  printf("survived\n");
  return 0;
}

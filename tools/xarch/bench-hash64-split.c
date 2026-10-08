/* Is x86_128 actually faster than x64_128 on a 32-bit machine?  That is the
 * only thing the SIZE_MAX split can be for.  Both variants are called
 * directly, so nothing depends on which one gcu_string_hash_64() picks. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/string.h>

#ifndef SCALE
#define SCALE 1
#endif

#ifndef MODE
#define MODE 0
#endif

#ifndef VARIANT
#define VARIANT 0
#endif

int main(void) {
  static uint8_t big[65536];
  for (size_t i = 0; i < sizeof big; ++i) big[i] = (uint8_t)(i * 31 + 7);
  uint64_t acc = 0;
  uint8_t out[16];

#if VARIANT == 1          /* x86_128: four 32-bit lanes */
# define H(p, n) gcu_string_murmur3_x86_128((p), (n), 0, out)
#elif VARIANT == 2        /* x64_128: two 64-bit lanes */
# define H(p, n) gcu_string_murmur3_x64_128((p), (n), 0, out)
#else
# error "define VARIANT=1 or 2"
#endif

  /* Identifier-sized keys, the shape ctang actually hashes.  The accumulator
   * folds in `r` and every output byte, so it is input-dependent: a run that
   * printed a constant would mean the calls had been optimised away. */
#if MODE != 2
  for (int r = 0; r < 20000 * SCALE; ++r)
    for (size_t n = 1; n <= 24; ++n) {
      H(big + ((n + (size_t)r) % 7), n);
      for (int b = 0; b < 16; ++b) acc = acc * 31 + out[b];
    }
#endif
  /* And a bulk pass, where the block loop dominates. */
#if MODE != 1
  for (int r = 0; r < 200 * SCALE; ++r) {
    H(big + (r % 3), sizeof big - 3);
    for (int b = 0; b < 16; ++b) acc = acc * 31 + out[b];
  }
#endif
  printf("%016llx\n", (unsigned long long)acc);
  return 0;
}

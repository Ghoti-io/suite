/*
 * The cross-target hash fixture for the shape family (CAP-12, AD-17).
 *
 * A tiny deterministic computation over doubles, hashed with FNV-1a 64 over
 * the little-endian bytes of each result.  Every target must print the same
 * hash: x86-64, arm64, i686 (built with SSE), big-endian (s390x, powerpc,
 * sparc64) and Windows.  The libraries that follow this one's scaffold replace
 * the computation with their own intermediates (CAP-13 specifies the hash once,
 * in shape-geometry); what this fixture pins is the gate itself, and the three
 * ways a build can make a double differ:
 *
 *   x87 excess precision  the first sensitive sum, 1 + 2^-53 + 2^-80
 *   fused multiply-add    the second, a*b - 1 with a*b not exact
 *   byte order            the bytes are written least-significant first
 *                         explicitly, never through a pointer cast
 *
 * Operands that matter are volatile so the compiler cannot fold the answer at
 * compile time (a folded constant is computed by the build machine, not by
 * the target, and would pass for the wrong reason).
 *
 * -DSHAPE_PLANT_BIT flips one bit of one result before it is hashed: the
 * planted differing bit that the gate has to catch on the target it is built
 * for.  Built by suite/tools/xarch/shape.sh and suite/tools/xwin/shape-win.sh.
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t hash = 0xcbf29ce484222325ull; /* FNV-1a 64 offset basis */
static unsigned count;

static void feed_byte(uint8_t b) {
  hash ^= b;
  hash *= 0x100000001b3ull; /* FNV-1a 64 prime */
}

static void feed_u64(uint64_t v) {
  for (int i = 0; i < 8; i++) {
    feed_byte((uint8_t)(v >> (8 * i))); /* little-endian, by arithmetic */
  }
}

static uint64_t bits_of(double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof bits); /* same-width reinterpretation, no cast */
  return bits;
}

static void feed_double(double d) {
  uint64_t bits = bits_of(d);
#ifdef SHAPE_PLANT_BIT
  if (count == 17) {
    bits ^= 1ull;
  }
#endif
  count++;
  feed_u64(bits);
}

int main(void) {
  /* 1 + 2^-53 + 2^-80: x87 rounds this twice and gets 1.0; SSE gets 1+2^-52. */
  volatile double one = 1.0;
  volatile double small = 0x1p-53 + 0x1p-80;
  double x87_sum = one + small;
  feed_double(x87_sum);

  /* (1 + 2^-27)(1 - 2^-27) - 1: 0 when the product is rounded first, -2^-54
   * when it is fused. */
  volatile double fa = 1.0 + 0x1p-27;
  volatile double fb = 1.0 - 0x1p-27;
  volatile double fc = -1.0;
  double fma_expr = fa * fb + fc;
  feed_double(fma_expr);

  /* A stream of doubles in [1, 2) from an integer generator (exact on every
   * target), pushed through the operations a rasterizer uses. */
  volatile uint64_t seed = 0x9E3779B97F4A7C15ull;
  uint64_t s = seed;
  double acc = 0.0;
  for (int i = 0; i < 64; i++) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t mant = s >> 12;
    uint64_t bits = 0x3FF0000000000000ull | mant; /* [1, 2) */
    double a;
    memcpy(&a, &bits, sizeof a);
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    bits = 0x3FF0000000000000ull | (s >> 12);
    double b;
    memcpy(&b, &bits, sizeof b);
    acc = acc + a * b;      /* the accumulating multiply-add shape */
    feed_double(acc);
    feed_double(a / b);
    feed_double(sqrt(a + b)); /* sqrt is exactly specified (IEEE 754) */
    feed_double(a * a - b * b);
  }
  feed_u64((uint64_t)count);

  printf("shape.x87_sum  %016" PRIx64 "\n", bits_of(x87_sum));
  printf("shape.fma_expr  %016" PRIx64 "\n", bits_of(fma_expr));
  printf("shape.count  %u\n", count);
  printf("shape.hash  %016" PRIx64 "\n", hash);
  return 0;
}

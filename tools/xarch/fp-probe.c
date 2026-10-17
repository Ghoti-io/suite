/*
 * A double that comes out differently under x87 and under SSE.
 *
 * The sum 1 + 2^-53 + 2^-80 is, as a double, 1 + 2^-52: it lies above the
 * halfway point between 1 and the next double.  x87 holds the sum in a 64-bit
 * mantissa, which cannot see 2^-80, so it holds exactly the halfway point 1 +
 * 2^-53, and rounding that to a double on the way out is a tie, which goes to
 * even: 1.0.  Two roundings, two different answers; the volatile operands keep
 * the compiler from folding the sum at compile time.
 *
 * Prints the result's bits and FLT_EVAL_METHOD (2 is x87 excess precision, 0
 * is "double operations are done in double").  Built by suite/tools/xarch/fp.sh.
 */
#include <float.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  volatile double one = 1.0;
  /* 2^-53 + 2^-80 spans 28 bits of mantissa, so it is exact as a double. */
  volatile double small = 0x1p-53 + 0x1p-80;
  double sum = one + small;
  uint64_t bits;
  memcpy(&bits, &sum, sizeof bits);
  printf("fp.sum_bits  %016" PRIx64 "\n", bits);
  printf("fp.eval_method  %d\n", (int)FLT_EVAL_METHOD);
  return 0;
}

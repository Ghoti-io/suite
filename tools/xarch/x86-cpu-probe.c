/*
 * What a CPU (or a qemu CPU model) says it has, and what its instructions do, for the four optional
 * x86-64 features runtime-jit uses (SSE4.1 `roundsd`, POPCNT, LZCNT, BMI1 `tzcnt`).
 *
 * Written on its own, in plain C with the CPUID instruction and four hand-encoded instructions, and
 * sharing nothing with runtime-jit: it is the independent reading jit-x86-cpu.sh compares both runtime-jit's
 * detection and its own table of what qemu does against. One line of `key=value` words on stdout.
 *
 * `lzcnt` and `tzcnt` do not fault on a CPU without LZCNT or BMI1: they run as `bsr` and `bsf`, which is
 * the hazard. So each is run and its result classified: `lzcnt rcx, rax` with rax = 0x10 and rcx = 0xFFFF
 * gives 59 as `lzcnt` and 4 as `bsr`; `tzcnt rcx, rax` with rax = 0 gives 64 as `tzcnt` and leaves rcx
 * (0xFFFF) as `bsf`. `popcnt` and `roundsd` fault with SIGILL where they are missing.
 */
#include <cpuid.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static sigjmp_buf env;
static void on_sigill(int sig) {
  (void)sig;
  siglongjmp(env, 1);
}

int main(void) {
  unsigned a, b, c, d;
  __cpuid_count(0, 0, a, b, c, d);
  const unsigned max_basic = a;
  __cpuid_count(1, 0, a, b, c, d);
  const unsigned leaf1_ecx = c;
  __cpuid_count(7, 0, a, b, c, d);
  const unsigned leaf7_ebx = b;
  __cpuid_count(0x80000000u, 0, a, b, c, d);
  const unsigned max_ext = a;
  __cpuid_count(0x80000001u, 0, a, b, c, d);
  const unsigned ext1_ecx = c;
  /* The bits as the CPU reports them, with the same defined-only-up-to-the-maximum rule runtime-jit applies,
   * written here independently. */
  printf("max_basic=0x%x max_ext=0x%x vmx_bit=%u sse4_1_bit=%u popcnt_bit=%u lzcnt_bit=%u bmi1_bit=%u", max_basic, max_ext,
      (leaf1_ecx >> 5) & 1u, (leaf1_ecx >> 19) & 1u, (leaf1_ecx >> 23) & 1u,
      max_ext >= 0x80000001u ? (ext1_ecx >> 5) & 1u : 0u, max_basic >= 7 ? (leaf7_ebx >> 3) & 1u : 0u);

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigill;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGILL, &sa, NULL);

  uint64_t r = 0xFFFF;
  if (sigsetjmp(env, 1) == 0) {
    __asm__ volatile("mov $0x10, %%eax\n\t.byte 0xf3, 0x48, 0x0f, 0xbd, 0xc8" : "+c"(r)::"rax", "cc");
    printf(" lzcnt_0x10=%llu lzcnt_as=%s", (unsigned long long)r, r == 59 ? "lzcnt" : r == 4 ? "bsr" : "other");
  } else {
    printf(" lzcnt_0x10=SIGILL lzcnt_as=sigill");
  }
  r = 0xFFFF;
  if (sigsetjmp(env, 1) == 0) {
    __asm__ volatile("xor %%eax, %%eax\n\t.byte 0xf3, 0x48, 0x0f, 0xbc, 0xc8" : "+c"(r)::"rax", "cc");
    printf(" tzcnt_0=%llu tzcnt_as=%s", (unsigned long long)r, r == 64 ? "tzcnt" : r == 0xFFFF ? "bsf" : "other");
  } else {
    printf(" tzcnt_0=SIGILL tzcnt_as=sigill");
  }
  r = 0;
  if (sigsetjmp(env, 1) == 0) {
    __asm__ volatile("mov $0xF0F0, %%eax\n\t.byte 0xf3, 0x48, 0x0f, 0xb8, 0xc8" : "+c"(r)::"rax", "cc");
    printf(" popcnt=%s", r == 8 ? "ok" : "wrong");
  } else {
    printf(" popcnt=SIGILL");
  }
  double x = -2.5, y = 0;
  if (sigsetjmp(env, 1) == 0) {
    __asm__ volatile("movsd %1, %%xmm0\n\t.byte 0x66, 0x0f, 0x3a, 0x0b, 0xc0, 0x09\n\tmovsd %%xmm0, %0"
                     : "=m"(y)
                     : "m"(x)
                     : "xmm0");
    printf(" roundsd=%s", y == -3.0 ? "ok" : "wrong");
  } else {
    printf(" roundsd=SIGILL");
  }
  printf("\n");
  return 0;
}

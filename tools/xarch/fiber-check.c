/*
 * The fiber checks that run under qemu-user, in plain C.
 *
 * test-fiber.cpp is the full suite, but it is C++ and GoogleTest, and the
 * ghoti-xarch image has only C cross compilers.  This is the part of it that
 * is about the architecture: the switch, the floating-point control state,
 * the guard page and the thread pin.  suite/tools/xarch/fiber.sh builds it for
 * aarch64 and runs it under qemu-aarch64, and builds it for the host as the
 * control.
 *
 * Exit status: 0 when every check passed, otherwise the number that failed.
 * Each check prints one line, `ok` or `FAIL`, so a run that did not execute a
 * check cannot be mistaken for one that passed it.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ghoti.io/cutil/fiber.h>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

static int failures = 0;

static void check(int ok, const char * name) {
  printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

//
// The rounding mode, read from the arithmetic and not only from fegetround().
// Adding numbers whose result depends on the mode is what shows that the
// hardware register, not just a copy of it, was switched.
//

enum { kNearest = 0, kUp = 1, kDown = 2, kZero = 3, kBad = -1 };

static int fe_value(int mode) {
  switch (mode) {
    case kUp: return FE_UPWARD;
    case kDown: return FE_DOWNWARD;
    case kZero: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
  }
}

__attribute__((noinline)) static int double_mode(void) {
  volatile double one = 1.0;
  volatile double minus_one = -1.0;
  volatile double tiny = 0x1p-60;
  volatile double three_quarters = 0x1.8p-53;
  volatile double x = one + tiny;
  volatile double y = minus_one - tiny;
  volatile double z = one + three_quarters;
  int up_x = x > 1.0;
  int down_y = y < -1.0;
  int up_z = z > 1.0;
  if (!up_x && !down_y && up_z) return kNearest;
  if (up_x && !down_y && up_z) return kUp;
  if (!up_x && down_y && !up_z) return kDown;
  if (!up_x && !down_y && !up_z) return kZero;
  return kBad;
}

static int mode_says(int expected) {
  int fe = fegetround() == fe_value(expected);
  int arith = double_mode() == expected;
#if defined(__x86_64__)
  int mx;
  switch (_MM_GET_ROUNDING_MODE()) {
    case _MM_ROUND_UP: mx = kUp; break;
    case _MM_ROUND_DOWN: mx = kDown; break;
    case _MM_ROUND_TOWARD_ZERO: mx = kZero; break;
    default: mx = kNearest; break;
  }
  arith = arith && mx == expected;
#endif
  return fe && arith;
}

//
// Round trip.
//

static int step;

static void round_trip_entry(void * arg) {
  (void)arg;
  step = 1;
  gcu_fiber_yield();
  step = 2;
  gcu_fiber_yield();
  step = 3;
}

static void check_round_trip(void) {
  GCU_Fiber * f = NULL;
  int ok = gcu_fiber_create(&f, round_trip_entry, NULL,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  ok = ok && step == 0 && !gcu_fiber_is_finished(f);
  ok = ok && gcu_fiber_switch_to(f) == GCU_FIBER_OK && step == 1;
  ok = ok && gcu_fiber_switch_to(f) == GCU_FIBER_OK && step == 2;
  ok = ok && !gcu_fiber_is_finished(f);
  ok = ok && gcu_fiber_switch_to(f) == GCU_FIBER_OK && step == 3;
  ok = ok && gcu_fiber_is_finished(f);
  ok = ok && gcu_fiber_switch_to(f) == GCU_FIBER_ERR_STATE;
  // Unconditionally: a failed chain above must not leak the mapping.
  ok = (gcu_fiber_destroy(f) == GCU_FIBER_OK) && ok;
  check(ok, "round trip: each leg runs once, finished is reported");
}

//
// Callee-saved registers survive a switch.  A small assembly routine loads a
// known value into every register the ABI says a call must preserve, calls
// through a function pointer, and reports what each held afterwards: `register`
// and `volatile` do not put a value in a callee-saved register, so a check
// written with them can pass while the switch drops all of them.
//
// x86-64: rbx r12-r15.  arm64: x19-x28 and d8-d15 (as bit patterns).
//

#if defined(__x86_64__)
static const int kRegCount = 5;
__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl gcu_test_regs_across\n"
  ".hidden gcu_test_regs_across\n"
  ".type gcu_test_regs_across, @function\n"
  "gcu_test_regs_across:\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n"
  "  pushq %r14\n  pushq %r15\n  pushq %rdx\n"
  "  movq %rdi, %rax\n"
  "  movq 0(%rsi), %rbx\n  movq 8(%rsi), %r12\n  movq 16(%rsi), %r13\n"
  "  movq 24(%rsi), %r14\n  movq 32(%rsi), %r15\n"
  "  call *%rax\n"
  "  movq (%rsp), %rdx\n"
  "  movq %rbx, 0(%rdx)\n  movq %r12, 8(%rdx)\n  movq %r13, 16(%rdx)\n"
  "  movq %r14, 24(%rdx)\n  movq %r15, 32(%rdx)\n"
  "  popq %rdx\n  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
  "  popq %rbx\n  popq %rbp\n  ret\n"
  ".size gcu_test_regs_across, .-gcu_test_regs_across\n"
);
#else
static const int kRegCount = 18;
__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl gcu_test_regs_across\n"
  ".hidden gcu_test_regs_across\n"
  ".type gcu_test_regs_across, %function\n"
  "gcu_test_regs_across:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  stp x19, x20, [sp, #-16]!\n  stp x21, x22, [sp, #-16]!\n"
  "  stp x23, x24, [sp, #-16]!\n  stp x25, x26, [sp, #-16]!\n"
  "  stp x27, x28, [sp, #-16]!\n"
  "  stp d8, d9, [sp, #-16]!\n  stp d10, d11, [sp, #-16]!\n"
  "  stp d12, d13, [sp, #-16]!\n  stp d14, d15, [sp, #-16]!\n"
  "  str x2, [sp, #-16]!\n"
  "  mov x9, x0\n  mov x10, x1\n"
  "  ldp x19, x20, [x10, #0]\n  ldp x21, x22, [x10, #16]\n"
  "  ldp x23, x24, [x10, #32]\n  ldp x25, x26, [x10, #48]\n"
  "  ldp x27, x28, [x10, #64]\n"
  "  ldp d8, d9, [x10, #80]\n  ldp d10, d11, [x10, #96]\n"
  "  ldp d12, d13, [x10, #112]\n  ldp d14, d15, [x10, #128]\n"
  "  blr x9\n"
  "  ldr x2, [sp]\n"
  "  stp x19, x20, [x2, #0]\n  stp x21, x22, [x2, #16]\n"
  "  stp x23, x24, [x2, #32]\n  stp x25, x26, [x2, #48]\n"
  "  stp x27, x28, [x2, #64]\n"
  "  stp d8, d9, [x2, #80]\n  stp d10, d11, [x2, #96]\n"
  "  stp d12, d13, [x2, #112]\n  stp d14, d15, [x2, #128]\n"
  "  add sp, sp, #16\n"
  "  ldp d14, d15, [sp], #16\n  ldp d12, d13, [sp], #16\n"
  "  ldp d10, d11, [sp], #16\n  ldp d8, d9, [sp], #16\n"
  "  ldp x27, x28, [sp], #16\n  ldp x25, x26, [sp], #16\n"
  "  ldp x23, x24, [sp], #16\n  ldp x21, x22, [sp], #16\n"
  "  ldp x19, x20, [sp], #16\n  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  ".size gcu_test_regs_across, .-gcu_test_regs_across\n"
);
#endif


void gcu_test_regs_across(void (*fn)(void), const uint64_t * in,
  uint64_t * out);

static void reg_pattern(uint64_t seed, uint64_t * v) {
  for (int i = 0; i < kRegCount; i++) {
    v[i] = seed * 0x0101010101010101ull + (uint64_t)(i + 1) * 0x1000193ull;
  }
}

static int regs_changed(const uint64_t * in, const uint64_t * out) {
  int bad = 0;
  for (int i = 0; i < kRegCount; i++) {
    if (in[i] != out[i]) bad++;
  }
  return bad;
}

static GCU_Fiber * regs_fiber;
static int regs_changed_in_fiber;

static void switch_thunk(void) { gcu_fiber_switch_to(regs_fiber); }
static void yield_thunk(void) { gcu_fiber_yield(); }

static void regs_entry(void * arg) {
  (void)arg;
  for (int i = 0; i < 100; i++) {
    uint64_t in[18], out[18];
    reg_pattern(0x70 + i, in);
    gcu_test_regs_across(yield_thunk, in, out);
    regs_changed_in_fiber += regs_changed(in, out);
  }
}

static void check_registers_survive(void) {
  int ok = gcu_fiber_create(&regs_fiber, regs_entry, NULL,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  int changed_in_caller = 0;
  for (int i = 0; ok && i < 100; i++) {
    uint64_t in[18], out[18];
    reg_pattern(0x20 + i, in);
    gcu_test_regs_across(switch_thunk, in, out);
    changed_in_caller += regs_changed(in, out);
  }
  ok = ok && gcu_fiber_switch_to(regs_fiber) == GCU_FIBER_OK;
  ok = ok && gcu_fiber_is_finished(regs_fiber);
  // Unconditionally, so a failed chain above does not leak the mapping.
  ok = (gcu_fiber_destroy(regs_fiber) == GCU_FIBER_OK) && ok;
  check(ok && changed_in_caller == 0 && regs_changed_in_fiber == 0,
    "callee-saved registers survive switches in both directions");
}

//
// Floating-point isolation.
//

typedef struct {
  int mode;
  int rounds;
  int wrong;
} Rounding;

static void rounding_entry(void * arg) {
  Rounding * r = (Rounding *)arg;
  fesetround(fe_value(r->mode));
  for (int i = 0; i < r->rounds; i++) {
    if (!mode_says(r->mode)) r->wrong++;
    gcu_fiber_yield();
  }
  if (!mode_says(r->mode)) r->wrong++;
}

static void check_rounding_isolation(void) {
  Rounding a = {kUp, 200, 0};
  Rounding b = {kDown, 200, 0};
  GCU_Fiber * fa = NULL;
  GCU_Fiber * fb = NULL;
  int ok = gcu_fiber_create(&fa, rounding_entry, &a,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  ok = ok && gcu_fiber_create(&fb, rounding_entry, &b,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  fesetround(FE_TOWARDZERO);
  ok = ok && mode_says(kZero);
  int caller_wrong = 0;
  for (int i = 0; ok && i < 200; i++) {
    ok = gcu_fiber_switch_to(fa) == GCU_FIBER_OK;
    if (!mode_says(kZero)) caller_wrong++;
    ok = ok && gcu_fiber_switch_to(fb) == GCU_FIBER_OK;
    if (!mode_says(kZero)) caller_wrong++;
  }
  ok = ok && gcu_fiber_switch_to(fa) == GCU_FIBER_OK;
  ok = ok && gcu_fiber_switch_to(fb) == GCU_FIBER_OK;
  int caller_end = mode_says(kZero);
  fesetround(FE_TONEAREST);
  check(ok && a.wrong == 0, "rounding: the upward fiber keeps its mode");
  check(ok && b.wrong == 0, "rounding: the downward fiber keeps its mode");
  check(ok && caller_wrong == 0 && caller_end,
    "rounding: the resumer's mode is untouched by either fiber");
  gcu_fiber_destroy(fa);
  gcu_fiber_destroy(fb);
}

static void new_fiber_mode_entry(void * arg) {
  *(int *)arg = mode_says(kNearest);
}

static void check_new_fiber_default_mode(void) {
  int nearest = 0;
  GCU_Fiber * f = NULL;
  fesetround(FE_UPWARD);
  int ok = gcu_fiber_create(&f, new_fiber_mode_entry, &nearest,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  ok = ok && gcu_fiber_switch_to(f) == GCU_FIBER_OK;
  int creator_kept = mode_says(kUp);
  fesetround(FE_TONEAREST);
  check(ok && nearest && creator_kept,
    "rounding: a new fiber starts in the default mode, not its creator's");
  gcu_fiber_destroy(f);
}

//
// Guard page.
//

static volatile long overflow_limit = 1L << 40;

__attribute__((noinline)) static long overflow_recurse(long depth) {
  volatile char pad[512];
  pad[0] = (char)depth;
  if (depth >= overflow_limit) {
    return pad[0];
  }
  return overflow_recurse(depth + 1) + pad[0];
}

static void overflow_entry(void * arg) {
  (void)arg;
  overflow_recurse(0);
}

static void check_guard_page(void) {
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(60);
    signal(SIGSEGV, SIG_DFL);
    GCU_Fiber * f = NULL;
    if (gcu_fiber_create(&f, overflow_entry, NULL,
          GCU_FIBER_MIN_STACK_SIZE * 2, NULL) != GCU_FIBER_OK) {
      _exit(70);
    }
    gcu_fiber_switch_to(f);
    _exit(71); // ran past the end of its stack
  }
  int status = 0;
  int ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
  check(ok, "guard page: overflowing a fiber stack dies on SIGSEGV");
}

//
// Thread pin.
//

typedef struct {
  GCU_Fiber * fiber;
  GCU_Fiber_Result resumed;
  GCU_Fiber_Result destroyed;
  GCU_Fiber * current;
} PinProbe;

static int pin_ticks;

static void pin_entry(void * arg) {
  (void)arg;
  pin_ticks++;
  gcu_fiber_yield();
  pin_ticks++;
}

static void * pin_thread(void * arg) {
  PinProbe * p = (PinProbe *)arg;
  p->resumed = gcu_fiber_switch_to(p->fiber);
  p->destroyed = gcu_fiber_destroy(p->fiber);
  p->current = gcu_fiber_current();
  return NULL;
}

static void check_thread_pin(void) {
  PinProbe p = {NULL, GCU_FIBER_OK, GCU_FIBER_OK, (GCU_Fiber *)1};
  int ok = gcu_fiber_create(&p.fiber, pin_entry, NULL,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  ok = ok && gcu_fiber_switch_to(p.fiber) == GCU_FIBER_OK && pin_ticks == 1;
  pthread_t t;
  ok = ok && pthread_create(&t, NULL, pin_thread, &p) == 0;
  ok = ok && pthread_join(t, NULL) == 0;
  check(ok && p.resumed == GCU_FIBER_ERR_THREAD &&
        p.destroyed == GCU_FIBER_ERR_THREAD && p.current == NULL &&
        pin_ticks == 1,
    "thread pin: resume and destroy from another thread are refused");
  // Untouched: it resumes here, where it was left, and finishes.
  ok = ok && gcu_fiber_switch_to(p.fiber) == GCU_FIBER_OK && pin_ticks == 2;
  ok = ok && gcu_fiber_is_finished(p.fiber);
  ok = ok && gcu_fiber_destroy(p.fiber) == GCU_FIBER_OK;
  check(ok, "thread pin: the refused fiber still runs on its own thread");
}

int main(void) {
  printf("fiber-check: %s, GCU_FIBER_SUPPORTED=%d\n",
#if defined(__aarch64__)
    "aarch64",
#elif defined(__x86_64__)
    "x86_64",
#else
    "other",
#endif
    GCU_FIBER_SUPPORTED);
  if (!GCU_FIBER_SUPPORTED) {
    printf("FAIL fibers are not supported here\n");
    return 1;
  }
  check_round_trip();
  check_registers_survive();
  check_rounding_isolation();
  check_new_fiber_default_mode();
  check_guard_page();
  check_thread_pin();
  printf("fiber-check: %d failed\n", failures);
  return failures;
}

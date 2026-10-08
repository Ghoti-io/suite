/*
 * What fibers do on an architecture with no switch routine: build, link, and
 * refuse.  Built by suite/tools/xarch/fiber.sh for every target in the matrix that
 * is not x86-64 or aarch64, and run under qemu-user.  Exit status 0 only if
 * every call reports GCU_FIBER_ERR_UNSUPPORTED rather than crashing or
 * pretending.
 */

#include <stdio.h>

#include <ghoti.io/cutil/fiber.h>

static void entry(void * arg) {
  (void)arg;
}

int main(void) {
  GCU_Fiber * f = (GCU_Fiber *)1;
  int bad = 0;
  if (GCU_FIBER_SUPPORTED != 0) {
    printf("FAIL GCU_FIBER_SUPPORTED is %d on a target with no switch\n",
      GCU_FIBER_SUPPORTED);
    bad++;
  }
  if (gcu_fiber_create(&f, entry, NULL, GCU_FIBER_DEFAULT_STACK_SIZE, NULL) !=
      GCU_FIBER_ERR_UNSUPPORTED || f != (GCU_Fiber *)1) {
    printf("FAIL create did not refuse, or wrote its output\n");
    bad++;
  }
  if (gcu_fiber_switch_to(f) != GCU_FIBER_ERR_UNSUPPORTED) {
    printf("FAIL switch_to did not refuse\n");
    bad++;
  }
  if (gcu_fiber_yield() != GCU_FIBER_ERR_UNSUPPORTED) {
    printf("FAIL yield did not refuse\n");
    bad++;
  }
  if (gcu_fiber_destroy(f) != GCU_FIBER_ERR_UNSUPPORTED) {
    printf("FAIL destroy did not refuse\n");
    bad++;
  }
  if (gcu_fiber_destroy(NULL) != GCU_FIBER_OK) {
    printf("FAIL destroy(NULL) is ignored, and returns OK, as in the real arms\n");
    bad++;
  }
  if (gcu_fiber_current() != NULL || !gcu_fiber_is_finished(f)) {
    printf("FAIL current/is_finished\n");
    bad++;
  }
  if (bad == 0) {
    printf("ok   every call refuses: %s\n",
      gcu_fiber_result_string(GCU_FIBER_ERR_UNSUPPORTED));
  }
  return bad;
}

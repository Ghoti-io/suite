/*
 * What the loop does where it has no implementation: build, link, and
 * refuse.  Built by suite/tools/xarch/loop.sh with the library compiled
 * `-DGCU_LOOP_FORCE_UNSUPPORTED`, which selects the arm kqueue targets get,
 * and run on the host, since that is the only way to reach the arm on a
 * machine that has epoll.  Exit status 0 only if every call reports
 * GCU_LOOP_ERR_UNSUPPORTED rather than crashing or pretending.
 */

#include <stdio.h>

#include <ghoti.io/cutil/loop.h>
#include <ghoti.io/cutil/socket.h>

int main(void) {
  int bad = 0;
  GCU_Loop * loop = (GCU_Loop *)1;
  GCU_Loop_Op op;
  gcu_loop_op_init(&op, NULL, NULL);
  if (GCU_LOOP_SUPPORTED != 0) {
    printf("FAIL GCU_LOOP_SUPPORTED is %d with the arm forced off\n",
      GCU_LOOP_SUPPORTED);
    bad++;
  }
  if (gcu_loop_create(&loop, NULL) != GCU_LOOP_ERR_UNSUPPORTED
      || loop != (GCU_Loop *)1) {
    printf("FAIL create did not refuse, or wrote its output\n");
    bad++;
  }
  GCU_Socket * s = NULL;
  if (gcu_socket_create(&s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, NULL)
      != GCU_SOCKET_OK) {
    printf("FAIL sockets do not depend on the loop and must still work\n");
    bad++;
  }
  GCU_Loop * fake = (GCU_Loop *)&op;   // never dereferenced by this arm
  char buf[4];
  if (gcu_loop_read(fake, &op, s, buf, 4) != GCU_LOOP_ERR_UNSUPPORTED
      || gcu_loop_timer_start(fake, &op, 1) != GCU_LOOP_ERR_UNSUPPORTED
      || gcu_loop_run_once(fake, 0) != GCU_LOOP_ERR_UNSUPPORTED
      || gcu_loop_post(fake, &op) != GCU_LOOP_ERR_UNSUPPORTED
      || gcu_loop_fiber_wait(&op) != GCU_LOOP_ERR_UNSUPPORTED) {
    printf("FAIL a call did not refuse\n");
    bad++;
  }
  if (gcu_loop_destroy(NULL) != GCU_LOOP_OK) {
    printf("FAIL destroy(NULL) is not ignored\n");
    bad++;
  }
  gcu_socket_close(s);
  if (!bad) {
    printf("ok   the unsupported arm builds, links and refuses\n");
  }
  return bad;
}

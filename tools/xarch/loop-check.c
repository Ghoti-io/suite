/*
 * The loop checks that run under qemu-user, in plain C.
 *
 * test-loop.cpp is the full suite, but it is C++ and GoogleTest, and the
 * ghoti-xarch image has only C cross compilers.  This is the part of it that
 * the architecture can change: the epoll arm's system calls under another
 * ABI, the ordering of timers, the wake from another thread, the cancel rule
 * and a fiber resumed by the loop.  suite/tools/xarch/loop.sh builds it for aarch64
 * and runs it under qemu-aarch64, and builds it for the host as the control.
 *
 * Exit status: 0 when every check passed, otherwise the number that failed.
 * Each check prints one line, `ok` or `FAIL`, so a run that did not execute a
 * check cannot be mistaken for one that passed it.  Nothing here waits
 * without a bound: a defect that loses a wakeup shows as a failed check, not
 * as a hang.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/cutil/loop.h>
#include <ghoti.io/cutil/socket.h>

static int failures = 0;

static void check(int ok, const char * name) {
  printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct Rec {
  GCU_Loop_Op op;
  int calls;
  GCU_Loop_Completion r;
  pthread_t tid;
  int order;
} Rec;

static int order_counter;

static void rec_callback(GCU_Loop_Op * op, void * user) {
  Rec * rec = (Rec *)user;
  rec->calls++;
  rec->r = op->result;
  rec->tid = pthread_self();
  rec->order = ++order_counter;
}

static void rec_init(Rec * rec) {
  memset(rec, 0, sizeof(*rec));
  gcu_loop_op_init(&rec->op, rec_callback, rec);
}

/** Run the loop until `*flag` (an int the callbacks bump) or the time is up. */
static int pump(GCU_Loop * loop, int (*done)(void *), void * arg, int ms) {
  int64_t end = now_ms() + ms;
  while (!done(arg)) {
    if (now_ms() > end) {
      return 0;
    }
    gcu_loop_run_once(loop, 10);
  }
  return 1;
}

static int calls_of(void * rec) { return ((Rec *)rec)->calls; }

typedef struct Pair {
  GCU_Socket * listener;
  GCU_Socket * client;
  GCU_Socket * server;
  GCU_Socket_Address address;
} Pair;

static int connect_pair(GCU_Loop * loop, Pair * p) {
  memset(p, 0, sizeof(*p));
  GCU_Socket_Address any;
  gcu_socket_address_loopback(&any, GCU_SOCKET_IPV4, 0);
  if (gcu_socket_create(&p->listener, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, NULL)
      || gcu_socket_bind(p->listener, &any)
      || gcu_socket_listen(p->listener, 8)
      || gcu_socket_local_address(p->listener, &p->address)
      || gcu_socket_create(&p->client, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, NULL)) {
    return 0;
  }
  Rec acc, con;
  rec_init(&acc);
  rec_init(&con);
  if (gcu_loop_accept(loop, &acc.op, p->listener)
      || gcu_loop_connect(loop, &con.op, p->client, &p->address)) {
    return 0;
  }
  int64_t end = now_ms() + 5000;
  while (!(acc.calls && con.calls) && now_ms() < end) {
    gcu_loop_run_once(loop, 10);
  }
  if (!(acc.calls && con.calls) || acc.r.status != GCU_LOOP_OK
      || con.r.status != GCU_LOOP_OK) {
    return 0;
  }
  p->server = acc.r.accepted;
  return p->server != NULL;
}

static void pair_close(Pair * p) {
  gcu_socket_close(p->client);
  gcu_socket_close(p->server);
  gcu_socket_close(p->listener);
}

static int all_done2(void * v) {
  Rec ** r = (Rec **)v;
  return r[0]->calls && r[1]->calls;
}

static void check_echo(GCU_Loop * loop) {
  Pair p;
  if (!connect_pair(loop, &p)) {
    check(0, "tcp: a loopback pair connects");
    return;
  }
  char in[16] = {0};
  Rec read1, write1;
  rec_init(&read1);
  rec_init(&write1);
  Rec * both[2] = {&read1, &write1};
  int started = gcu_loop_read(loop, &read1.op, p.server, in, sizeof(in)) == 0
    && gcu_loop_write(loop, &write1.op, p.client, "ping", 4) == 0;
  int done = started && pump(loop, all_done2, both, 5000);
  pthread_t self = pthread_self();
  check(done && read1.r.status == GCU_LOOP_OK && read1.r.bytes == 4
      && memcmp(in, "ping", 4) == 0 && write1.r.bytes == 4
      && read1.calls == 1 && write1.calls == 1
      && pthread_equal(read1.tid, self) && pthread_equal(write1.tid, self),
    "tcp: write and read complete once, on the loop thread");
  pair_close(&p);
}

static void check_timers(GCU_Loop * loop) {
  Rec t[3];
  unsigned long delays[3] = {30, 10, 20};
  int64_t started[3], fired[3] = {0, 0, 0};
  order_counter = 0;
  for (int i = 0; i < 3; ++i) {
    rec_init(&t[i]);
  }
  for (int i = 0; i < 3; ++i) {
    started[i] = now_ms();
    gcu_loop_timer_start(loop, &t[i].op, delays[i]);
  }
  int64_t end = now_ms() + 3000;
  while (!(t[0].calls && t[1].calls && t[2].calls) && now_ms() < end) {
    gcu_loop_run_once(loop, 10);
    for (int i = 0; i < 3; ++i) {
      if (t[i].calls && !fired[i]) {
        fired[i] = now_ms();
      }
    }
  }
  check(t[0].calls && t[1].calls && t[2].calls && t[1].order == 1
      && t[2].order == 2 && t[0].order == 3,
    "timers of 30, 10 and 20 ms fire in the order 10, 20, 30");
  int early = 0;
  for (int i = 0; i < 3; ++i) {
    // Millisecond clock on both sides: a timer fires no earlier than its delay
    // less the one tick the two readings can disagree by.
    if (fired[i] - started[i] < (int64_t)delays[i] - 1) {
      early = 1;
    }
  }
  check(!early, "timers do not fire before their time");
}

static void check_large_write(GCU_Loop * loop) {
  Pair p;
  if (!connect_pair(loop, &p)) {
    check(0, "large write: a loopback pair connects");
    return;
  }
  gcu_socket_set_option(p.client, GCU_SOCKET_OPT_SEND_BUFFER, 16384);
  gcu_socket_set_option(p.server, GCU_SOCKET_OPT_RECV_BUFFER, 16384);
  size_t total = 512 * 1024;
  unsigned char * payload = malloc(total);
  unsigned char * got = calloc(1, total);
  for (size_t i = 0; i < total; ++i) {
    payload[i] = (unsigned char)(i * 7 + (i >> 9));
  }
  Rec write, read;
  rec_init(&write);
  rec_init(&read);
  size_t have = 0;
  int reads = 0;
  gcu_loop_write(loop, &write.op, p.client, payload, total);
  gcu_loop_read(loop, &read.op, p.server, got, total);
  int64_t end = now_ms() + 20000;
  while (!(write.calls && have == total) && now_ms() < end) {
    gcu_loop_run_once(loop, 10);
    if (read.calls) {
      have += read.r.bytes;
      ++reads;
      read.calls = 0;
      if (read.r.status != GCU_LOOP_OK || read.r.bytes == 0) {
        break;
      }
      if (have < total) {
        gcu_loop_read(loop, &read.op, p.server, got + have, total - have);
      }
    }
  }
  check(write.calls == 1 && write.r.status == GCU_LOOP_OK
      && write.r.bytes == total && have == total && reads > 1
      && memcmp(payload, got, total) == 0,
    "a write larger than the socket buffer completes after all its bytes");
  pair_close(&p);
  free(payload);
  free(got);
}

static void check_cancel(GCU_Loop * loop) {
  Pair p;
  if (!connect_pair(loop, &p)) {
    check(0, "cancel: a loopback pair connects");
    return;
  }
  unsigned char buf[64];
  memset(buf, 0xAB, sizeof(buf));
  Rec rd;
  rec_init(&rd);
  int started = gcu_loop_read(loop, &rd.op, p.server, buf, sizeof(buf)) == 0;
  int cancelled = started && gcu_loop_cancel(loop, &rd.op) == GCU_LOOP_OK;
  check(cancelled && rd.calls == 0,
    "cancel: the completion does not arrive before cancel returns");
  int64_t end = now_ms() + 2000;
  while (!rd.calls && now_ms() < end) {
    gcu_loop_run_once(loop, 10);
  }
  check(rd.calls == 1 && rd.r.status == GCU_LOOP_CANCELLED,
    "cancel: one completion, CANCELLED");

  // The peer writes only now.
  Rec w;
  rec_init(&w);
  gcu_loop_write(loop, &w.op, p.client, "late-data", 9);
  end = now_ms() + 300;
  while (now_ms() < end) {
    gcu_loop_run_once(loop, 5);
  }
  int untouched = 1;
  for (size_t i = 0; i < sizeof(buf); ++i) {
    if (buf[i] != 0xAB) {
      untouched = 0;
    }
  }
  check(untouched && rd.calls == 1,
    "cancel: the buffer is untouched when the peer writes afterwards");
  pair_close(&p);
}

typedef struct PostArg {
  GCU_Loop * loop;
  Rec * rec;
  int result;
} PostArg;

static void * poster(void * v) {
  PostArg * a = (PostArg *)v;
  struct timespec pause = {0, 60 * 1000 * 1000};
  nanosleep(&pause, NULL);
  a->result = gcu_loop_post(a->loop, &a->rec->op);
  return NULL;
}

static void check_post(GCU_Loop * loop) {
  Rec posted;
  rec_init(&posted);
  PostArg arg = {loop, &posted, -1};
  pthread_t t;
  int64_t t0 = now_ms();
  pthread_create(&t, NULL, poster, &arg);
  // A bounded wait, so a post that never wakes the loop is a failed check.
  gcu_loop_run_once(loop, 5000);
  int64_t elapsed = now_ms() - t0;
  pthread_join(t, NULL);
  check(arg.result == GCU_LOOP_OK && posted.calls == 1
      && pthread_equal(posted.tid, pthread_self()) && elapsed < 2000,
    "a post from another thread wakes a waiting loop");
  // Drain a late delivery in the planted build so teardown stays clean.
  gcu_loop_run_once(loop, 0);
}

typedef struct FiberArg {
  GCU_Loop * loop;
  GCU_Socket * socket;
  Rec rd;
  char buf[16];
  pthread_t fiber_thread;
  int wait_result;
  int done;
} FiberArg;

static void fiber_entry(void * v) {
  FiberArg * f = (FiberArg *)v;
  gcu_loop_read(f->loop, &f->rd.op, f->socket, f->buf, sizeof(f->buf));
  f->wait_result = gcu_loop_fiber_wait(&f->rd.op);
  f->fiber_thread = pthread_self();
  f->done = 1;
}

static int fiber_done(void * v) { return ((FiberArg *)v)->done; }

static void check_fiber(GCU_Loop * loop) {
  if (!GCU_FIBER_SUPPORTED) {
    printf("skip fibers are not supported on this target\n");
    return;
  }
  Pair p;
  if (!connect_pair(loop, &p)) {
    check(0, "fiber: a loopback pair connects");
    return;
  }
  static FiberArg fa;
  memset(&fa, 0, sizeof(fa));
  rec_init(&fa.rd);
  fa.loop = loop;
  fa.socket = p.server;
  GCU_Fiber * fiber = NULL;
  int made = gcu_fiber_create(&fiber, fiber_entry, &fa,
    GCU_FIBER_DEFAULT_STACK_SIZE, NULL) == GCU_FIBER_OK;
  int parked = made && gcu_fiber_switch_to(fiber) == GCU_FIBER_OK && !fa.done;
  Rec w;
  rec_init(&w);
  gcu_loop_write(loop, &w.op, p.client, "fiber", 5);
  int resumed = parked && pump(loop, fiber_done, &fa, 5000);
  check(resumed && fa.wait_result == GCU_LOOP_OK && fa.rd.r.bytes == 5
      && memcmp(fa.buf, "fiber", 5) == 0
      && pthread_equal(fa.fiber_thread, pthread_self()),
    "a fiber waiting on a read resumes on its own thread with the result");
  if (fiber) {
    gcu_fiber_destroy(fiber);
  }
  pair_close(&p);
}

static void check_datagram(GCU_Loop * loop) {
  GCU_Socket * a = NULL;
  GCU_Socket * b = NULL;
  gcu_socket_create(&a, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, NULL);
  gcu_socket_create(&b, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, NULL);
  GCU_Socket_Address addr_a, addr_b;
  gcu_socket_address_loopback(&addr_a, GCU_SOCKET_IPV4, 0);
  gcu_socket_address_loopback(&addr_b, GCU_SOCKET_IPV4, 0);
  gcu_socket_bind(a, &addr_a);
  gcu_socket_bind(b, &addr_b);
  gcu_socket_local_address(a, &addr_a);
  gcu_socket_local_address(b, &addr_b);
  char buf[16] = {0};
  Rec recv, send;
  rec_init(&recv);
  rec_init(&send);
  gcu_loop_recvfrom(loop, &recv.op, a, buf, sizeof(buf));
  gcu_loop_sendto(loop, &send.op, b, "hello", 5, &addr_a);
  Rec * both[2] = {&recv, &send};
  int done = pump(loop, all_done2, both, 5000);
  check(done && recv.r.status == GCU_LOOP_OK && recv.r.bytes == 5
      && memcmp(buf, "hello", 5) == 0 && recv.r.peer.port == addr_b.port,
    "a datagram arrives with its sender's address");
  gcu_socket_close(a);
  gcu_socket_close(b);
}

static void check_refused(GCU_Loop * loop) {
  GCU_Socket * probe = NULL;
  gcu_socket_create(&probe, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, NULL);
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 0);
  gcu_socket_bind(probe, &a);
  gcu_socket_local_address(probe, &a);
  gcu_socket_close(probe);
  GCU_Socket * s = NULL;
  gcu_socket_create(&s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, NULL);
  Rec con;
  rec_init(&con);
  gcu_loop_connect(loop, &con.op, s, &a);
  pump(loop, calls_of, &con, 5000);
  check(con.calls == 1 && con.r.status == GCU_LOOP_ERR_OS
      && gcu_socket_error_kind(con.r.os_error) == GCU_SOCKET_ERROR_REFUSED,
    "connecting to a closed port completes with a refusal");
  gcu_socket_close(s);
}

static void check_destroy(void) {
  GCU_Loop * loop = NULL;
  if (gcu_loop_create(&loop, NULL) != GCU_LOOP_OK) {
    check(0, "destroy: a second loop is created");
    return;
  }
  Rec timer, posted;
  rec_init(&timer);
  rec_init(&posted);
  gcu_loop_timer_start(loop, &timer.op, 60000);
  gcu_loop_post(loop, &posted.op);
  int destroyed = gcu_loop_destroy(loop) == GCU_LOOP_OK;
  check(destroyed && timer.calls == 1 && timer.r.status == GCU_LOOP_CANCELLED
      && posted.calls == 1 && posted.r.status == GCU_LOOP_CANCELLED,
    "destroying a loop cancels what is outstanding, then returns");
}

int main(void) {
  GCU_Loop * loop = NULL;
  if (gcu_loop_create(&loop, NULL) != GCU_LOOP_OK) {
    printf("FAIL the loop could not be created\n");
    return 1;
  }
  check_echo(loop);
  check_timers(loop);
  check_large_write(loop);
  check_cancel(loop);
  check_post(loop);
  check_fiber(loop);
  check_datagram(loop);
  check_refused(loop);
  check_destroy();
  gcu_loop_destroy(loop);
  return failures;
}

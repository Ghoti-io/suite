/* Win64 probe for the runtime stack's Windows-only branches (TODO(windows)).
 * Built by m1-probe.sh against the *installed* DLLs and import libraries, so
 * it exercises the dllexport/dllimport switching as a consumer sees it.
 *
 *   A  runtime-core page provider: VirtualAlloc / VirtualProtect / VirtualFree,
 *      and the write-xor-execute flip a JIT depends on (checked with
 *      VirtualQuery, by running machine code, and by a write to the read-execute
 *      page that must fault).
 *   B  threads on winpthreads: create/join, a mutex and condition, a
 *      _Thread_local counter per thread; the clock a condition accepts
 *      (CLOCK_REALTIME only: CLOCK_MONOTONIC is EINVAL, which runtime-core used
 *      to read as out of memory); and runtime-core making a request port and
 *      post to it, which is where that mattered.
 *   C  runtime-jit: grjit_backend_available() is true, and a function is
 *      compiled, run and destroyed, with its unwind table seen registered by
 *      RtlLookupFunctionEntry and gone after the destroy.
 *   D  runtime-debug: grdbg_transport_create_fd refuses on Windows.
 *
 * Exit 0 only if every CHECK passes.  -DPLANT_SKIP_PROTECT leaves the page
 * read-write instead of flipping it; the probe must then FAIL (the control). */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-jit/runtime-jit.h>
#include <ghoti.io/runtime-debug/runtime-debug.h>

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; \
  printf("  FAIL  line %d: %s\n", __LINE__, #c); } else printf("  ok    %s\n", #c); } while (0)

static volatile LONG g_faulted;
static LONG CALLBACK on_fault(EXCEPTION_POINTERS * e) {
  if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    g_faulted = 1;
    e->ContextRecord->Rip = (DWORD64)(uintptr_t)ExitThread;
    e->ContextRecord->Rcx = 0;
    e->ContextRecord->Rsp = (e->ContextRecord->Rsp & ~(DWORD64)15) - 8 - 32;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static DWORD WINAPI write_thread(void * p) { *(volatile unsigned char *)p = 0x90; return 0; }

static DWORD query_protect(void * p) {
  MEMORY_BASIC_INFORMATION m;
  return VirtualQuery(p, &m, sizeof m) ? m.Protect : 0;
}

static void part_a(void) {
  printf("A  runtime-core page provider (VirtualAlloc/VirtualProtect/VirtualFree)\n");
  const GRCORE_PageProvider * pp = grcore_page_provider_default();
  SYSTEM_INFO si; GetSystemInfo(&si);
  CHECK(pp != NULL && pp->page_size == si.dwPageSize);
  size_t ps = pp->page_size;
  CHECK(pp->map(pp->ctx, 0) == NULL);
  CHECK(pp->map(pp->ctx, ps + 1) == NULL);
  unsigned char * p = pp->map(pp->ctx, ps);
  CHECK(p != NULL && ((uintptr_t)p % ps) == 0);
  if (!p) return;
  CHECK(query_protect(p) == PAGE_READWRITE);
  bool zero = true; for (size_t i = 0; i < ps; i++) zero &= p[i] == 0;
  CHECK(zero);
  /* mov eax, 42 ; ret */
  static const unsigned char code[] = {0xB8, 0x2A, 0, 0, 0, 0xC3};
  memcpy(p, code, sizeof code);
  CHECK(grcore_page_protect(pp, p + 1, ps, GRCORE_PAGE_READ_EXECUTE) == GRCORE_ERR_INVALID);
  CHECK(grcore_page_protect(pp, p, ps - 1, GRCORE_PAGE_READ_EXECUTE) == GRCORE_ERR_INVALID);
#ifndef PLANT_SKIP_PROTECT
  CHECK(grcore_page_protect(pp, p, ps, GRCORE_PAGE_READ_EXECUTE) == GRCORE_OK);
#endif
  DWORD prot = query_protect(p);
  printf("  VirtualQuery protection = 0x%08lx\n", (unsigned long)prot);
  /* VirtualQuery may include PAGE_* modifier bits in addition to the base
   * protection value; test the page's access mode and ignore those flags. */
  CHECK((prot & 0xffu) == PAGE_EXECUTE_READ);
  if ((prot & 0xffu) == PAGE_EXECUTE_READ) {
    FlushInstructionCache(GetCurrentProcess(), p, sizeof code);
    int (*fn)(void); memcpy(&fn, &p, sizeof fn);
    CHECK(fn() == 42);
    /* W^X: a write to the read-execute page must fault. */
    PVOID h = AddVectoredExceptionHandler(1, on_fault);
    g_faulted = 0;
    HANDLE t = CreateThread(NULL, 0, write_thread, p, 0, NULL);
    WaitForSingleObject(t, 10000); CloseHandle(t);
    RemoveVectoredExceptionHandler(h);
    CHECK(g_faulted == 1);
    CHECK(grcore_page_protect(pp, p, ps, GRCORE_PAGE_READ_WRITE) == GRCORE_OK);
    CHECK(query_protect(p) == PAGE_READWRITE);
  }
  pp->unmap(pp->ctx, p, ps);
  MEMORY_BASIC_INFORMATION m;
  VirtualQuery(p, &m, sizeof m);
  CHECK(m.State != MEM_COMMIT);   /* VirtualFree(MEM_RELEASE) took it back */
}

static _Thread_local unsigned tl = 0;
static void * thr(void * a) { tl += 5; *(unsigned *)a = tl; return a; }

static void part_b(void) {
  printf("B  winpthreads\n");
  pthread_t t; unsigned seen = 0; tl = 100;
  CHECK(pthread_create(&t, NULL, thr, &seen) == 0);
  void * r = NULL; CHECK(pthread_join(t, &r) == 0 && r == &seen);
  CHECK(seen == 5 && tl == 100);       /* _Thread_local is per thread */
  pthread_mutex_t m; pthread_cond_t c; pthread_condattr_t a;
  CHECK(pthread_mutex_init(&m, NULL) == 0);
  CHECK(pthread_condattr_init(&a) == 0);
  /* winpthreads accepts only CLOCK_REALTIME for a condition. This is the fact
   * src/b/cond_clock_internal.h is built on; if a newer winpthreads accepts
   * CLOCK_MONOTONIC the library is still right (it uses the system clock
   * there) and this line changes but nothing fails. */
  int mono = pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  printf("  pthread_condattr_setclock(CLOCK_MONOTONIC) = %d (EINVAL=%d)\n", mono, EINVAL);
  CHECK(pthread_condattr_setclock(&a, CLOCK_REALTIME) == 0);
  CHECK(pthread_cond_init(&c, &a) == 0);
  struct timespec d; clock_gettime(CLOCK_REALTIME, &d); d.tv_nsec += 50000000;
  if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
  pthread_mutex_lock(&m);
  CHECK(pthread_cond_timedwait(&c, &m, &d) == ETIMEDOUT);
  pthread_mutex_unlock(&m);
  pthread_cond_destroy(&c); pthread_mutex_destroy(&m); pthread_condattr_destroy(&a);

  /* The call that used to fail: a context's request port is made with a
   * condition variable, and a post to it takes the port's mutex. */
  GRCORE_Group * g = NULL; GRCORE_Context * ctx = NULL; GRCORE_Port * port = NULL;
  CHECK(grcore_group_create(NULL, NULL, &g) == GRCORE_OK);
  CHECK(grcore_context_create(g, NULL, &ctx) == GRCORE_OK);
  CHECK(grcore_context_port(ctx, &port) == GRCORE_OK && port != NULL);
  if (port) {
    CHECK(grcore_port_post(port, GRCORE_REQUEST_TIME) == GRCORE_OK);
    CHECK(grcore_context_request_pending(ctx, GRCORE_REQUEST_TIME));
    grcore_port_release(port);
  }
  grcore_context_destroy(ctx);
  grcore_group_destroy(g);
}

static uint32_t probe_deopt(void * context, uint64_t cause) {
  (void)context;
  (void)cause;
  return 0;
}

static void part_c(void) {
  printf("C  runtime-jit (compiled code through the installed DLLs)\n");
  CHECK(grjit_backend_available() == true);   /* Windows x86-64 has a backend */
  GRJIT_Builder * b = NULL;
  GRJIT_VReg x, y, environment, spare;
  GRJIT_BlockId blk;
  GRJIT_Function * f = NULL;
  CHECK(grjit_builder_create("probe", 0, NULL, NULL, &b) == GRJIT_OK);
  if (!b) return;
  GRJIT_CallHooks hooks = {0};
  hooks.deopt = probe_deopt;
  CHECK(grjit_builder_set_callable(b, &hooks, 2) == GRJIT_OK);
  /* Twelve arguments cross the Win64 register boundary; the callable also
   * returns its invocation environment through the second-result buffer. */
  CHECK(grjit_builder_param(b, GRJIT_TYPE_I64, &x) == GRJIT_OK);
  for (size_t i = 1; i < 12; i++)
    CHECK(grjit_builder_param(b, GRJIT_TYPE_I64, &spare) == GRJIT_OK);
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_I64, &y) == GRJIT_OK);
  CHECK(grjit_builder_vreg(b, GRJIT_TYPE_PTR, &environment) == GRJIT_OK);
  CHECK(grjit_builder_block(b, &blk) == GRJIT_OK);
  CHECK(grjit_builder_set_block(b, blk) == GRJIT_OK);
  CHECK(grjit_builder_binary(b, GRJIT_OP_MUL, y, grjit_operand_vreg(x), grjit_operand_imm(2)) == GRJIT_OK);
  CHECK(grjit_builder_binary(b, GRJIT_OP_ADD, y, grjit_operand_vreg(y), grjit_operand_imm(1)) == GRJIT_OK);
  CHECK(grjit_builder_instance(b, environment) == GRJIT_OK);
  const GRJIT_Operand results[2] = {grjit_operand_vreg(y), grjit_operand_vreg(environment)};
  CHECK(grjit_builder_ret_multi(b, results, 2) == GRJIT_OK);
  CHECK(grjit_builder_finish(b, &f) == GRJIT_OK);
  if (!f) return;
  GRJIT_CompileOptions o;
  memset(&o, 0, sizeof o);
  o.pages = grcore_page_provider_default();
  GRJIT_Code * code = NULL;
  CHECK(grjit_compile(&o, f, &code) == GRJIT_OK);
  if (code) {
    GRCORE_Group * group = NULL;
    GRCORE_Context * context = NULL;
    int instance = 0;
    uint64_t args[12] = {20};
    size_t out_words = grjit_code_out_words(code);
    CHECK(out_words >= 2);
    uint64_t * out = calloc(out_words, sizeof *out);
    CHECK(out != NULL);
    CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
    if (group) CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
    if (context && out) {
      CHECK(grjit_code_call(code, context, &instance, args, out) == GRJIT_EXIT_RETURNED &&
          out[0] == 41 && out[1] == (uint64_t)(uintptr_t)&instance);
    }
    if (context) grcore_context_destroy(context);
    if (group) grcore_group_destroy(group);
    free(out);
    /* The code's unwind table is registered with the system: the lookup of an
     * address inside it finds the function, with the mapping as its base. */
    DWORD64 code_base = (DWORD64)(uintptr_t)grjit_code_address(code), image = 0;
    DWORD64 internal = (DWORD64)grjit_code_internal_entry(code);
    DWORD body_begin = internal >= code_base ? (DWORD)(internal - code_base) : 0;
    DWORD64 at = code_base + 1;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(at, &image, NULL);
    CHECK(rf != NULL && image == code_base);
    /* Callable code registers separate unwind ranges for the entry adapter and
     * compiled body. Alignment may leave an unregistered gap between them. */
    CHECK(body_begin > 0 && body_begin < grjit_code_size(code));
    CHECK(rf != NULL && rf->BeginAddress <= 1 && rf->EndAddress > 1 &&
        rf->EndAddress <= body_begin);
    DWORD64 body_image = 0;
    PRUNTIME_FUNCTION body = RtlLookupFunctionEntry(internal, &body_image, NULL);
    CHECK(body != NULL && body_image == code_base);
    CHECK(body != NULL && body->BeginAddress == body_begin && body->EndAddress == grjit_code_size(code));
    grjit_code_destroy(code);
    /* ...and not once the code is gone. */
    CHECK(RtlLookupFunctionEntry(at, &image, NULL) == NULL);
    CHECK(RtlLookupFunctionEntry(internal, &body_image, NULL) == NULL);
  }
  grjit_function_destroy(f);
}

static void part_d(void) {
  printf("D  runtime-debug fd transport\n");
  GRDBG_Transport * t = (GRDBG_Transport *)1;
  GRDBG_Result r = grdbg_transport_create_fd(0, 1, NULL, &t);
  CHECK(r == GRDBG_ERR_UNSUPPORTED);
}

int main(void) {
  part_a(); part_b(); part_c(); part_d();
  printf("%d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

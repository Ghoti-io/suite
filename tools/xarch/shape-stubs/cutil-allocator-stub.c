/* The definitions behind shape-stubs/ghoti.io/cutil/allocator.h: malloc, for
 * the cross gates' probes.  Not a copy of cutil's allocator. */
#include <ghoti.io/cutil/allocator.h>

#include <stdlib.h>

static void * m(void * ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void * c(void * ctx, size_t k, size_t n) { (void)ctx; return calloc(k ? k : 1, n ? n : 1); }
static void * r(void * ctx, void * p, size_t n) { (void)ctx; return realloc(p, n ? n : 1); }
static void f(void * ctx, void * p) { (void)ctx; free(p); }

static const GCU_Allocator stub = {0, m, c, r, f};

const GCU_Allocator * gcu_allocator_default(void) { return &stub; }
void * gcu_allocator_malloc(const GCU_Allocator * a, size_t n) { if (!a) a = &stub; return a->malloc_fn(a->ctx, n); }
void * gcu_allocator_calloc(const GCU_Allocator * a, size_t k, size_t n) { if (!a) a = &stub; return a->calloc_fn(a->ctx, k, n); }
void * gcu_allocator_realloc(const GCU_Allocator * a, void * p, size_t n) { if (!a) a = &stub; return a->realloc_fn(a->ctx, p, n); }
void gcu_allocator_free(const GCU_Allocator * a, void * p) { if (!a) a = &stub; a->free_fn(a->ctx, p); }

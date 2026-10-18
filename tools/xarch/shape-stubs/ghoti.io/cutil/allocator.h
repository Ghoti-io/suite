/*
 * A stand-in for cutil's <ghoti.io/cutil/allocator.h>, for the cross gates only.
 *
 * The cross gates compile the shape libraries' sources for targets that have
 * no cutil (and cutil's real header needs generated headers).  A library that
 * allocates takes its allocator type and its five allocator functions from
 * cutil; this declares exactly those, unrenamed, and cutil-allocator-stub.c
 * defines them over malloc.  shape.sh section 6 fails if the declarations
 * here differ from cutil's, so the stand-in cannot drift.
 */
#ifndef SHAPE_STUB_CUTIL_ALLOCATOR_H
#define SHAPE_STUB_CUTIL_ALLOCATOR_H

#include <stddef.h>

typedef struct GCU_Allocator {
  void * ctx;
  void * (*malloc_fn)(void * ctx, size_t size);
  void * (*calloc_fn)(void * ctx, size_t nitems, size_t size);
  void * (*realloc_fn)(void * ctx, void * ptr, size_t size);
  void (*free_fn)(void * ctx, void * ptr);
} GCU_Allocator;

const GCU_Allocator * gcu_allocator_default(void);
void * gcu_allocator_malloc(const GCU_Allocator * allocator, size_t size);
void * gcu_allocator_calloc(
  const GCU_Allocator * allocator, size_t nitems, size_t size);
void * gcu_allocator_realloc(
  const GCU_Allocator * allocator, void * ptr, size_t size);
void gcu_allocator_free(const GCU_Allocator * allocator, void * ptr);

#endif

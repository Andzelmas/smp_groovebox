// Copyright 2011-2021 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "allocator.h"

#include "attributes.h"

#include <stdlib.h>
#include <unistd.h>

// POSIX.1-2001: posix_memalign()
#  ifndef HAVE_POSIX_MEMALIGN
#    if defined(_POSIX_VERSION) && _POSIX_VERSION >= 200112L
#      define HAVE_POSIX_MEMALIGN 1
#    else
#      define HAVE_POSIX_MEMALIGN 0
#    endif
#  endif

#if HAVE_POSIX_MEMALIGN
#  define USE_POSIX_MEMALIGN 1
#else
#  define USE_POSIX_MEMALIGN 0
#endif

ZIX_MALLOC_FUNC
static void*
zix_default_malloc(ZixAllocator* const allocator, const size_t size)
{
  (void)allocator;
  return malloc(size);
}

ZIX_MALLOC_FUNC
static void*
zix_default_calloc(ZixAllocator* const allocator,
                   const size_t        nmemb,
                   const size_t        size)
{
  (void)allocator;
  return calloc(nmemb, size);
}

static void*
zix_default_realloc(ZixAllocator* const allocator,
                    void* const         ptr,
                    const size_t        size)
{
  (void)allocator;
  return realloc(ptr, size);
}

static void
zix_default_free(ZixAllocator* const allocator, void* const ptr)
{
  (void)allocator;
  free(ptr);
}

ZIX_MALLOC_FUNC
static void*
zix_default_aligned_alloc(ZixAllocator* const allocator,
                          const size_t        alignment,
                          const size_t        size)
{
  (void)allocator;

#if defined USE_POSIX_MEMALIGN
  void*     ptr = NULL;
  const int ret = posix_memalign(&ptr, alignment, size);
  return ret ? NULL : ptr;
#else
  return NULL;
#endif
}

static void
zix_default_aligned_free(ZixAllocator* const allocator, void* const ptr)
{
  (void)allocator;

  free(ptr);
}

ZixAllocator*
zix_default_allocator(void)
{
  static ZixAllocator default_allocator = {
    zix_default_malloc,
    zix_default_calloc,
    zix_default_realloc,
    zix_default_free,
    zix_default_aligned_alloc,
    zix_default_aligned_free,
  };

  return &default_allocator;
}

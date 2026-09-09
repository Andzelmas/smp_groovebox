#pragma once
#include <stdint.h>

// The identity of a navigable context. Minted by the data layer (app_data.c
// composes it from a namespace + a module-local uid) and used unchanged by
// every layer above it - the context layer and the ui layer speak the same
// type, no per-layer renaming.
//
// This header has no dependency on the rest of the program on purpose; it is
// part of the portable contract, like data_object.h.

typedef uint64_t ContextId;

// "no id". A real id is never 0.
#define CONTEXT_ID_NULL ((ContextId)0)

// The namespace lives in the top 8 bits (see MAKE_ID in app_data.c). Every real
// id carries a non-zero namespace, which is what tells a composed id apart from
// a bare integer or garbage.
#define CTXID_NS_SHIFT 56
#define CTXID_NS(id)   ((unsigned)((ContextId)(id) >> CTXID_NS_SHIFT))

#ifndef NDEBUG
#include <assert.h>
#define ASSERT_CTXID(id) assert((id) != CONTEXT_ID_NULL && CTXID_NS(id) != 0)
#else
#define ASSERT_CTXID(id) ((void)0)
#endif

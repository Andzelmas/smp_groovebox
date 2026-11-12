#pragma once
#include <stdbool.h>
#include <stddef.h>

// Which items of a list are shown, and the slice of them that fits a UI's
// layout. Knows only counts and indices, not what an item is.
// Optional helper for ease of use: a UI can ignore it and call ui_layer directly.

// what a slice is built over: count raw items, of which shown() says which are
// drawn. shown NULL means every item is drawn
typedef struct _ui_windowing_source{
    size_t count;
    bool (*shown)(void *user, size_t idx);
    void *user;
} UI_WINDOWING_SOURCE;

// the part of a source's shown items that fits in capacity. items are raw
// indices into the source, in order - only shown ones, so an item that is not
// drawn never costs a slot or sets more_before / more_after
typedef struct _ui_windowing_slice{
    size_t *items;      // caller-owned, holds capacity items
    size_t capacity;
    size_t count;
    size_t cursor_slot; // index into items, count when the cursor is not in them
    bool more_before;   // shown items exist before items[0]
    bool more_after;    // ... and after items[count - 1]
} UI_WINDOWING_SLICE;

bool ui_windowing_is_shown(const UI_WINDOWING_SOURCE *source, size_t idx);

// move steps shown items away from from_idx. With wrap it goes on past the
// ends, without it stops on the last shown item in that direction.
// source->count when that leaves it on no shown item
size_t ui_windowing_step(const UI_WINDOWING_SOURCE *source, size_t from_idx, size_t steps,
                         bool forward, bool wrap);

// from_idx itself when it is shown, else the next shown item after it, wrapping
// at the end. source->count when nothing is shown
size_t ui_windowing_nearest_shown(const UI_WINDOWING_SOURCE *source, size_t from_idx);

// the last shown item before idx, else the first shown one at or after it.
// source->count when nothing is shown
size_t ui_windowing_last_shown_before(const UI_WINDOWING_SOURCE *source, size_t idx);

// collect up to capacity shown items walking away from from_idx in one
// direction (no wrapping), nearest first, and return how many. Sets *out_more
// when at least one more shown item was left behind. A NULL out_items only counts
size_t ui_windowing_collect_shown(const UI_WINDOWING_SOURCE *source, size_t from_idx, bool forward,
                                  size_t capacity, size_t *out_items, bool *out_more);

// fill slice with source's shown items around cursor_idx, up to slice->capacity.
// No capacity, or a cursor that is out of range or on an item not shown yields
// an empty slice
void ui_windowing_fill_around_cursor(const UI_WINDOWING_SOURCE *source, size_t cursor_idx,
                                     UI_WINDOWING_SLICE *slice);

// fill slice forward from the first shown item at or after first_idx, up to
// slice->capacity. Never pulls in earlier items, so a short last page keeps the
// page boundaries fixed. No capacity yields an empty slice, so does no shown
// item at or after first_idx, but then more_before says if any lie before it.
// To page, step items[0] (first_idx when empty) by capacity without wrap while
// more_after / more_before
void ui_windowing_fill_from_first(const UI_WINDOWING_SOURCE *source, size_t first_idx,
                                  size_t cursor_idx, UI_WINDOWING_SLICE *slice);

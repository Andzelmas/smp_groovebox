#include "ui_windowing.h"

bool ui_windowing_is_shown(const UI_WINDOWING_SOURCE *source, size_t idx){
    if(idx >= source->count)
        return false;
    return !source->shown || source->shown(source->user, idx);
}

size_t ui_windowing_step(const UI_WINDOWING_SOURCE *source, size_t from_idx, size_t steps,
                         bool forward, bool wrap){
    size_t count = source->count;
    size_t landed = ui_windowing_is_shown(source, from_idx) ? from_idx : count;
    size_t idx = from_idx < count ? from_idx : count;
    size_t shown_seen = 0;

    for(size_t moves = 0; steps > 0; moves++){
        if(wrap && moves == count){
            // a full lap went by, each further lap lands where it started
            if(shown_seen == 0)
                return count;
            steps %= shown_seen;
            if(steps == 0)
                break;
        }

        if(forward ? idx + 1 >= count : idx == 0){
            if(!wrap)
                break;
            idx = forward ? 0 : count - 1;
        }
        else
            idx = forward ? idx + 1 : idx - 1;

        if(ui_windowing_is_shown(source, idx)){
            landed = idx;
            shown_seen++;
            steps--;
        }
    }

    return landed;
}

size_t ui_windowing_nearest_shown(const UI_WINDOWING_SOURCE *source, size_t from_idx){
    if(ui_windowing_is_shown(source, from_idx))
        return from_idx;
    return ui_windowing_step(source, from_idx, 1, true, true);
}

size_t ui_windowing_last_shown_before(const UI_WINDOWING_SOURCE *source, size_t idx){
    for(size_t i = idx < source->count ? idx : source->count; i > 0; i--){
        if(ui_windowing_is_shown(source, i - 1))
            return i - 1;
    }
    return ui_windowing_nearest_shown(source, idx);
}

size_t ui_windowing_collect_shown(const UI_WINDOWING_SOURCE *source, size_t from_idx, bool forward,
                                  size_t capacity, size_t *out_items, bool *out_more){
    size_t count = 0;
    *out_more = false;

    size_t idx = from_idx;
    while(forward ? (idx + 1 < source->count) : (idx > 0)){
        idx = forward ? idx + 1 : idx - 1;
        if(!ui_windowing_is_shown(source, idx))
            continue;
        if(count == capacity){
            *out_more = true;
            break;
        }
        if(out_items)
            out_items[count] = idx;
        count++;
    }

    return count;
}

static size_t min_size(size_t a, size_t b){
    return a < b ? a : b;
}

// anchor_idx and the shown items around it, up to before_anchor of them before
// it. keep_full gives the slots the end of the list leaves unused to more
// items before the anchor
static void fill_slice(const UI_WINDOWING_SOURCE *source, size_t anchor_idx, size_t before_anchor,
                       bool keep_full, size_t cursor_idx, UI_WINDOWING_SLICE *slice){
    slice->count = 0;
    slice->cursor_slot = 0;
    slice->more_before = false;
    slice->more_after = false;

    if(slice->capacity == 0 || !ui_windowing_is_shown(source, anchor_idx))
        return;

    // count as much as either side could need, then split the other slots
    size_t other_slots = slice->capacity - 1;
    size_t before_count = ui_windowing_collect_shown(source, anchor_idx, false,
                                                     keep_full ? other_slots : before_anchor,
                                                     NULL, &slice->more_before);
    size_t after_count = ui_windowing_collect_shown(source, anchor_idx, true, other_slots,
                                                    NULL, &slice->more_after);
    size_t take_before = min_size(before_count, before_anchor);
    size_t take_after = min_size(after_count, other_slots - take_before);
    if(keep_full)
        take_before = min_size(before_count, other_slots - take_after);

    // capped at the split, so these also set more_* for the items it dropped
    slice->count = ui_windowing_collect_shown(source, anchor_idx, false, take_before,
                                              slice->items, &slice->more_before);
    // the before items came back nearest-first
    for(size_t low = 0, high = slice->count; low + 1 < high; low++, high--){
        size_t item = slice->items[low];
        slice->items[low] = slice->items[high - 1];
        slice->items[high - 1] = item;
    }
    slice->items[slice->count++] = anchor_idx;
    slice->count += ui_windowing_collect_shown(source, anchor_idx, true, take_after,
                                               slice->items + slice->count,
                                               &slice->more_after);

    while(slice->cursor_slot < slice->count && slice->items[slice->cursor_slot] != cursor_idx)
        slice->cursor_slot++;
}

void ui_windowing_fill_around_cursor(const UI_WINDOWING_SOURCE *source, size_t cursor_idx,
                                     UI_WINDOWING_SLICE *slice){
    size_t before_cursor = slice->capacity > 0 ? (slice->capacity - 1) / 2 : 0;
    fill_slice(source, cursor_idx, before_cursor, true, cursor_idx, slice);
}

void ui_windowing_fill_from_first(const UI_WINDOWING_SOURCE *source, size_t first_idx,
                                  size_t cursor_idx, UI_WINDOWING_SLICE *slice){
    size_t anchor_idx = ui_windowing_is_shown(source, first_idx)
                            ? first_idx
                            : ui_windowing_step(source, first_idx, 1, true, false);
    fill_slice(source, anchor_idx, 0, false, cursor_idx, slice);

    // nothing shown at or after first_idx, so any shown item lies before it
    if(slice->capacity > 0 && slice->count == 0)
        slice->more_before = ui_windowing_last_shown_before(source, first_idx) < source->count;
}

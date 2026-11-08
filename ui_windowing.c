#include "ui_windowing.h"

bool ui_windowing_is_shown(const UI_WINDOWING_SOURCE *source, size_t idx){
    if(idx >= source->count)
        return false;
    return !source->shown || source->shown(source->user, idx);
}

size_t ui_windowing_step(const UI_WINDOWING_SOURCE *source, size_t from_idx, bool forward){
    if(source->count == 0)
        return 0;

    size_t idx = from_idx;
    for(size_t steps = 0; steps < source->count; steps++){
        if(forward)
            idx = (idx + 1 >= source->count) ? 0 : idx + 1;
        else
            idx = (idx == 0 || idx > source->count) ? source->count - 1 : idx - 1;
        if(ui_windowing_is_shown(source, idx))
            return idx;
    }

    return source->count;
}

size_t ui_windowing_nearest_shown(const UI_WINDOWING_SOURCE *source, size_t from_idx){
    if(ui_windowing_is_shown(source, from_idx))
        return from_idx;
    return ui_windowing_step(source, from_idx, true);
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

void ui_windowing_fill_around_cursor(const UI_WINDOWING_SOURCE *source, size_t cursor_idx,
                                     UI_WINDOWING_SLICE *slice){
    slice->count = 0;
    slice->cursor_slot = 0;
    slice->more_before = false;
    slice->more_after = false;

    if(slice->capacity == 0 || !ui_windowing_is_shown(source, cursor_idx))
        return;

    // count as much as either side could ever need, then split the
    // non-cursor slots between them
    size_t non_cursor_slots = slice->capacity - 1;
    size_t before_count = ui_windowing_collect_shown(source, cursor_idx, false, non_cursor_slots,
                                                     NULL, &slice->more_before);
    size_t after_count = ui_windowing_collect_shown(source, cursor_idx, true, non_cursor_slots,
                                                    NULL, &slice->more_after);

    // centre the cursor, then hand whatever the short side left unused to the
    // other one, so the slice stays full near either end of the list
    size_t selected_dist = non_cursor_slots / 2;
    size_t take_before = before_count < selected_dist ? before_count : selected_dist;
    size_t take_after = after_count < selected_dist ? after_count : selected_dist;
    size_t spare = non_cursor_slots - take_before - take_after;

    size_t grow = after_count - take_after;
    if(grow > spare)
        grow = spare;
    take_after += grow;
    spare -= grow;

    grow = before_count - take_before;
    if(grow > spare)
        grow = spare;
    take_before += grow;

    // capped at the split, so these also set more_* for the items it dropped
    slice->count = ui_windowing_collect_shown(source, cursor_idx, false, take_before,
                                              slice->items, &slice->more_before);
    // the before items came back nearest-first
    for(size_t low = 0, high = slice->count; low + 1 < high; low++, high--){
        size_t item = slice->items[low];
        slice->items[low] = slice->items[high - 1];
        slice->items[high - 1] = item;
    }
    slice->cursor_slot = slice->count;
    slice->items[slice->count++] = cursor_idx;
    slice->count += ui_windowing_collect_shown(source, cursor_idx, true, take_after,
                                               slice->items + slice->count,
                                               &slice->more_after);
}

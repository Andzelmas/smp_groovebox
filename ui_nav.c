#include "ui_nav.h"

// NULL when context is not valid or has no name
static const char *ui_nav_valid_context_name(UI_LAYER *ui_layer, ContextId context){
    if (!ui_layer_context_valid(ui_layer, context))
        return NULL;
    return ui_layer_context_name_return(ui_layer, context);
}

bool ui_nav_context_info_get(UI_LAYER *ui_layer, ContextId context, UI_NAV_CONTEXT_INFO *info)
{
    if (!ui_layer)
        return false;

    if (!info)
        return false;

    const char *cx_name = ui_nav_valid_context_name(ui_layer, context);
    if (!cx_name)
        return false;

    info->name = cx_name;
    info->child_count = ui_layer_context_children_count(ui_layer, context);
    info->value = ui_layer_context_value_as_string(ui_layer, context);
    info->is_hidden = ui_layer_context_is_hidden(ui_layer, context);
    info->latency = ui_layer_context_latency(ui_layer, context);

    return true;
}

bool ui_nav_child_visible_at(UI_LAYER *ui_layer, ContextId parent, size_t idx,
                             ContextId *out_child){
    ContextId child = ui_layer_context_child_at(ui_layer, parent, idx);
    if(!ui_nav_valid_context_name(ui_layer, child))
        return false;
    if(ui_layer_context_is_hidden(ui_layer, child))
        return false;
    if(out_child)
        *out_child = child;
    return true;
}

static bool ui_nav_child_shown(void *user, size_t idx){
    UI_NAV_CHILDREN *children = user;
    return ui_nav_child_visible_at(children->ui_layer, children->parent, idx, NULL);
}

UI_WINDOWING_SOURCE ui_nav_children_source(UI_LAYER *ui_layer, ContextId parent,
                                           UI_NAV_CHILDREN *children){
    children->ui_layer = ui_layer;
    children->parent = parent;
    return (UI_WINDOWING_SOURCE){ui_layer_context_children_count(ui_layer, parent),
                                 ui_nav_child_shown, children};
}

ContextId ui_nav_purpose_context(UI_STATE *state, ContextId source, UiPurpose purpose){
    UI_TARGET_LIST *targets = ui_layer_nav_target_list_begin(state, source, purpose);
    if(!targets)
        return CONTEXT_ID_INVALID;
    ContextId context = ui_layer_nav_target_list_get(targets, 0);
    ui_layer_nav_target_list_end(state);
    return context;
}

bool ui_nav_purpose_context_set(UI_LAYER *ui_layer, UI_STATE *state, ContextId source,
                                UiPurpose purpose, ContextId context){
    if(!ui_layer_context_valid(ui_layer, context))
        return false;
    UI_TARGET_LIST *targets = ui_layer_nav_target_list_begin(state, source, purpose);
    if(!targets)
        return false;
    ui_layer_nav_target_list_clear(targets);
    bool added = ui_layer_nav_target_list_add(targets, context) == UI_TARGET_LIST_ADD_SUCCESS;
    ui_layer_nav_target_list_end(state);
    return added;
}

size_t ui_nav_purpose_context_ensure_visible(UI_LAYER *ui_layer, UI_STATE *state,
                                             ContextId parent, UiPurpose purpose,
                                             UiStalePolicy stale_policy){
    size_t child_count = ui_layer_context_children_count(ui_layer, parent);
    if(child_count == 0 || !ui_nav_valid_context_name(ui_layer, parent))
        return child_count;

    UI_TARGET_LIST *targets = ui_layer_nav_target_list_begin(state, parent, purpose);
    // begin does not borrow when it finds nothing, so entry_set is safe here
    if(!targets && ui_layer_state_entry_set(state, parent, purpose, 1)){
        targets = ui_layer_nav_target_list_begin(state, parent, purpose);
        ui_layer_target_list_set_stale_policy(targets, stale_policy);
    }
    if(!targets)
        return child_count;
    ContextId saved = ui_layer_nav_target_list_get(targets, 0);
    ui_layer_nav_target_list_end(state);

    // child_count when saved is not a child of parent (anymore)
    size_t saved_idx = child_count;
    for(size_t i = 0; i < child_count; i++){
        if(ui_layer_context_child_at(ui_layer, parent, i) == saved){
            saved_idx = i;
            break;
        }
    }

    UI_NAV_CHILDREN children;
    UI_WINDOWING_SOURCE source = ui_nav_children_source(ui_layer, parent, &children);
    size_t visible_idx = ui_windowing_nearest_shown(&source, saved_idx);
    if(visible_idx < child_count && visible_idx != saved_idx)
        ui_nav_purpose_context_set(ui_layer, state, parent, purpose,
                                   ui_layer_context_child_at(ui_layer, parent, visible_idx));
    return visible_idx;
}

size_t ui_nav_purpose_step(UI_LAYER *ui_layer, UI_STATE *state, ContextId parent,
                           UiPurpose purpose, size_t from_idx, size_t steps, bool forward,
                           bool wrap){
    if(!ui_nav_valid_context_name(ui_layer, parent))
        return from_idx;

    UI_NAV_CHILDREN children;
    UI_WINDOWING_SOURCE source = ui_nav_children_source(ui_layer, parent, &children);
    size_t to_idx = ui_windowing_step(&source, from_idx, steps, forward, wrap);
    if(to_idx >= source.count)
        return from_idx;
    if(!ui_nav_purpose_context_set(ui_layer, state, parent, purpose,
                                   ui_layer_context_child_at(ui_layer, parent, to_idx)))
        return from_idx;
    return to_idx;
}

bool ui_nav_context_enter(UI_LAYER *ui_layer, UI_STATE *state, ContextId current_source,
                          UiPurpose current_purpose, UiPurpose hovered_purpose){
    ContextId current = ui_nav_purpose_context(state, current_source, current_purpose);
    ContextId hovered = ui_nav_purpose_context(state, current, hovered_purpose);
    if(ui_layer_context_children_count(ui_layer, hovered) == 0)
        return false;
    return ui_nav_purpose_context_set(ui_layer, state, current_source, current_purpose, hovered);
}

bool ui_nav_context_exit(UI_LAYER *ui_layer, UI_STATE *state, ContextId current_source,
                         UiPurpose current_purpose){
    ContextId current = ui_nav_purpose_context(state, current_source, current_purpose);
    return ui_nav_purpose_context_set(ui_layer, state, current_source, current_purpose,
                                      ui_layer_context_parent_return(ui_layer, current));
}

bool ui_nav_shown_store(UI_LAYER *ui_layer, UI_STATE *state, ContextId source,
                        UiPurpose purpose, ContextId parent,
                        const UI_WINDOWING_SLICE *slice){
    if(!slice)
        return false;
    // a target list holds at least one
    size_t capacity = slice->capacity > 0 ? slice->capacity : 1;
    if(!ui_layer_state_entry_set(state, source, purpose, capacity))
        return false;
    UI_TARGET_LIST *targets = ui_layer_nav_target_list_begin(state, source, purpose);
    if(!targets)
        return false;

    ui_layer_nav_target_list_clear(targets);
    bool stored = ui_layer_nav_target_list_capacity(targets) == capacity ||
                  ui_layer_nav_target_list_resize(targets, capacity);
    for(size_t row = 0; row < slice->count; row++){
        ContextId child = ui_layer_context_child_at(ui_layer, parent, slice->items[row]);
        if(ui_layer_nav_target_list_add(targets, child) != UI_TARGET_LIST_ADD_SUCCESS)
            stored = false;
    }
    ui_layer_nav_target_list_end(state);
    return stored;
}

size_t ui_nav_actions_gather(UI_LAYER *ui_layer, ContextId context, UI_NAV_ACTION *out,
                             size_t capacity){
    if(!out || !ui_layer_context_valid(ui_layer, context))
        return 0;
    DataAction actions[UI_NAV_CONTEXT_ACTIONS_MAX];
    size_t count = ui_layer_context_actions(
        ui_layer, context, actions,
        capacity < UI_NAV_CONTEXT_ACTIONS_MAX ? capacity : UI_NAV_CONTEXT_ACTIONS_MAX);
    for(size_t i = 0; i < count; i++)
        out[i] = (UI_NAV_ACTION){context, actions[i]};
    return count;
}

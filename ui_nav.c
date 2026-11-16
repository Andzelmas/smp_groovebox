#include "ui_nav.h"
#include <stdio.h>

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

bool ui_nav_context_action_find(UI_LAYER *ui_layer, ContextId context,
                                DataActionType type, UI_NAV_ACTION *out){
    if(!out)
        return false;
    UI_NAV_ACTION actions[UI_NAV_CONTEXT_ACTIONS_MAX];
    size_t count = ui_nav_actions_gather(ui_layer, context, actions, UI_NAV_CONTEXT_ACTIONS_MAX);
    for(size_t i = 0; i < count; i++){
        if(actions[i].action.type == type){
            *out = actions[i];
            return true;
        }
    }
    return false;
}

UiNavInput ui_nav_action_input(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                               DataArgSpec specs[UI_NAV_ACTION_ARGS_MAX]){
    if(!action || !specs)
        return UI_NAV_INPUT_UNSUPPORTED;
    DataActionType type = action->action.type;
    size_t arg_count = ui_layer_context_action_args(ui_layer, action->context, type,
                                                    action->action.id, specs,
                                                    UI_NAV_ACTION_ARGS_MAX);
    if(type == DATA_ACTION_CONNECT)
        return arg_count < 2 ? UI_NAV_INPUT_MISCONFIGURED : UI_NAV_INPUT_LIST_CONNECT;
    if(type == DATA_ACTION_ADJUST_VALUE)
        return arg_count < 1 ? UI_NAV_INPUT_MISCONFIGURED : UI_NAV_INPUT_STEPS;
    if(type == DATA_ACTION_SET_VALUE)
        return arg_count < 1 ? UI_NAV_INPUT_MISCONFIGURED : UI_NAV_INPUT_NUMBER;
    if(arg_count == 0)
        return UI_NAV_INPUT_NONE;
    if(arg_count == 1 && (specs[0].kind == DATA_ARG_PATH || specs[0].kind == DATA_ARG_STRING))
        return UI_NAV_INPUT_TEXT;
    if(arg_count == 1 && specs[0].kind == DATA_ARG_CHOICE)
        return UI_NAV_INPUT_LIST_CHOICE;
    return UI_NAV_INPUT_UNSUPPORTED;
}

// req carries the input, the action's type and id are filled here
static DataActionResult ui_nav_action_req_run(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                              DataActionReq *req, ContextId *out_new){
    if(!action){
        if(out_new)
            *out_new = CONTEXT_ID_INVALID;
        return DATA_ACTION_ERR_INVALID;
    }
    req->type = action->action.type;
    req->id = action->action.id;
    return ui_layer_context_action_do(ui_layer, action->context, req, out_new);
}

DataActionResult ui_nav_action_run(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                   ContextId *out_new){
    DataActionReq req = {0};
    return ui_nav_action_req_run(ui_layer, action, &req, out_new);
}

DataActionResult ui_nav_action_run_adjust(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                          int steps, ContextId *out_new){
    DataActionReq req = {.adjust_value.step = steps};
    return ui_nav_action_req_run(ui_layer, action, &req, out_new);
}

DataActionResult ui_nav_action_run_set_value(UI_LAYER *ui_layer,
                                             const UI_NAV_ACTION *action, double value,
                                             ContextId *out_new){
    DataActionReq req = {.set_value.value = value};
    return ui_nav_action_req_run(ui_layer, action, &req, out_new);
}

DataActionResult ui_nav_action_run_text(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                        const char *text, ContextId *out_new){
    DataActionReq req = {0};
    if(action && action->action.type == DATA_ACTION_SAVE)
        req.save.name = text;
    else
        req.add_file_path.path = text;
    return ui_nav_action_req_run(ui_layer, action, &req, out_new);
}

bool ui_nav_list_row_at(const UI_NAV_LIST_ROWS *rows, size_t idx, DataChoice *out_row){
    return ui_layer_context_list_at(rows->ui_layer, rows->context, rows->list, rows->partial,
                                    rows->branch, idx, out_row);
}

size_t ui_nav_list_count(const UI_NAV_LIST_ROWS *rows){
    size_t count = ui_layer_context_list_count(rows->ui_layer, rows->context, rows->list,
                                               rows->partial, rows->branch);
    if(count > 0)
        return count;
    DataChoice row;
    while(ui_nav_list_row_at(rows, count, &row))
        count++;
    return count;
}

static bool ui_nav_list_row_shown(void *user, size_t idx){
    const UI_NAV_LIST_ROWS *rows = user;
    DataChoice row;
    if(!ui_nav_list_row_at(rows, idx, &row))
        return false;
    if(rows->filter->group != 0 && row.group_key != rows->filter->group)
        return false;
    if(rows->filter->linked_only && !(row.flags & DATA_CHOICE_LINKED))
        return false;
    return true;
}

UI_WINDOWING_SOURCE ui_nav_list_source(UI_NAV_LIST_ROWS *rows){
    UI_WINDOWING_SOURCE source = {ui_nav_list_count(rows), NULL, rows};
    // unfiltered, every row list_at returns is shown
    const UI_NAV_LIST_FILTER *filter = rows->filter;
    if(filter && (filter->group != 0 || filter->linked_only))
        source.shown = ui_nav_list_row_shown;
    return source;
}

uint64_t ui_nav_list_value_at(const UI_NAV_LIST_ROWS *rows, size_t idx){
    DataChoice row;
    if(!ui_nav_list_row_at(rows, idx, &row))
        return 0;
    return row.value;
}

size_t ui_nav_list_find(const UI_NAV_LIST_ROWS *rows, uint64_t value, size_t hint_idx){
    if(value == 0)
        return SIZE_MAX;
    if(ui_nav_list_value_at(rows, hint_idx) == value)
        return hint_idx;
    DataChoice row;
    for(size_t i = 0; ui_nav_list_row_at(rows, i, &row); i++){
        if(row.value == value)
            return i;
    }
    return SIZE_MAX;
}

// idx must be shown
static void ui_nav_list_cursor_land(const UI_NAV_LIST_ROWS *rows, const UI_WINDOWING_SOURCE *source,
                                    UI_NAV_LIST_CURSOR *cursor, size_t idx){
    size_t near;
    bool more;
    cursor->idx = idx;
    cursor->value = ui_nav_list_value_at(rows, idx);
    cursor->prev = ui_windowing_collect_shown(source, idx, false, 1, &near, &more)
                       ? ui_nav_list_value_at(rows, near)
                       : 0;
    cursor->next = ui_windowing_collect_shown(source, idx, true, 1, &near, &more)
                       ? ui_nav_list_value_at(rows, near)
                       : 0;
}

bool ui_nav_list_cursor_set(UI_NAV_LIST_ROWS *rows, UI_NAV_LIST_CURSOR *cursor, size_t idx){
    UI_WINDOWING_SOURCE source = ui_nav_list_source(rows);
    if(!ui_windowing_is_shown(&source, idx))
        return false;
    ui_nav_list_cursor_land(rows, &source, cursor, idx);
    return true;
}

bool ui_nav_list_cursor_follow_row(UI_NAV_LIST_ROWS *rows, UI_NAV_LIST_CURSOR *cursor){
    UI_WINDOWING_SOURCE source = ui_nav_list_source(rows);
    size_t idx;
    size_t found = ui_nav_list_find(rows, cursor->value, cursor->idx);
    if(cursor->value == 0){
        idx = ui_windowing_nearest_shown(&source, 0);
    } else if(found < source.count){
        idx = ui_windowing_nearest_shown(&source, found);
    } else {
        // where the gone row would sit now, going by the rows it had around it
        size_t prev = ui_nav_list_find(rows, cursor->prev, cursor->idx - 1);
        size_t next = ui_nav_list_find(rows, cursor->next, cursor->idx);
        size_t pos = cursor->idx < source.count ? cursor->idx : source.count;
        if(prev < source.count)
            pos = prev + 1;
        else if(next < source.count)
            pos = next;
        idx = ui_windowing_last_shown_before(&source, pos);
    }
    if(idx >= source.count)
        return false;
    ui_nav_list_cursor_land(rows, &source, cursor, idx);
    return true;
}

bool ui_nav_list_cursor_step(UI_NAV_LIST_ROWS *rows, UI_NAV_LIST_CURSOR *cursor, size_t steps,
                             bool forward, bool wrap){
    UI_WINDOWING_SOURCE source = ui_nav_list_source(rows);
    size_t idx = ui_windowing_step(&source, cursor->idx, steps, forward, wrap);
    if(idx >= source.count)
        return false;
    ui_nav_list_cursor_land(rows, &source, cursor, idx);
    return true;
}

size_t ui_nav_list_group_first_row(const UI_NAV_LIST_ROWS *rows, uint64_t group){
    DataChoice row;
    for(size_t i = 0; ui_nav_list_row_at(rows, i, &row); i++){
        if(row.group_key == group)
            return i;
    }
    return SIZE_MAX;
}

uint64_t ui_nav_list_group_next(const UI_NAV_LIST_ROWS *rows, uint64_t current){
    bool passed = current == 0;
    DataChoice row;
    for(size_t i = 0; ui_nav_list_row_at(rows, i, &row); i++){
        uint64_t group = row.group_key;
        if(group == 0 || ui_nav_list_group_first_row(rows, group) != i)
            continue;
        if(passed)
            return group;
        if(group == current)
            passed = true;
    }
    return 0;
}

size_t ui_nav_list_group_count(const UI_NAV_LIST_ROWS *rows, size_t limit){
    size_t count = 0;
    DataChoice row;
    for(size_t i = 0; count < limit && ui_nav_list_row_at(rows, i, &row); i++){
        if(row.group_key != 0 && ui_nav_list_group_first_row(rows, row.group_key) == i)
            count++;
    }
    return count;
}

uint64_t ui_nav_list_group_of_context(const UI_NAV_LIST_ROWS *rows, ContextId context){
    for(ContextId cx = context; ui_layer_context_valid(rows->ui_layer, cx);
        cx = ui_layer_context_parent_return(rows->ui_layer, cx)){
        uint64_t group = ui_layer_context_group_key(rows->ui_layer, cx);
        if(group != 0 && ui_nav_list_group_first_row(rows, group) != SIZE_MAX)
            return group;
    }
    return 0;
}

bool ui_nav_list_session_start(UI_NAV_LIST_SESSION *session, const UI_NAV_ACTION *action,
                               UiNavInput input, const DataArgSpec *specs){
    size_t spec_count = input == UI_NAV_INPUT_LIST_CONNECT ? 2
                        : input == UI_NAV_INPUT_LIST_CHOICE ? 1
                                                            : 0;
    if(!session || !action || !specs || spec_count == 0)
        return false;
    *session = (UI_NAV_LIST_SESSION){
        .action = *action,
        .input = input,
        .connect_pick = UI_NAV_CONNECT_PICK_SOURCE,
        .partial = {.type = action->action.type, .id = action->action.id}};
    for(size_t i = 0; i < spec_count; i++)
        session->specs[i] = specs[i];
    return true;
}

static UI_NAV_LIST_ROWS ui_nav_list_session_rows_of(UI_LAYER *ui_layer,
                                                    const UI_NAV_LIST_SESSION *session,
                                                    size_t spec_idx){
    return (UI_NAV_LIST_ROWS){ui_layer,
                              session->action.context,
                              session->specs[spec_idx].list,
                              &session->partial,
                              ui_nav_list_session_branch(session, spec_idx),
                              &session->filters[spec_idx]};
}

void ui_nav_list_session_narrow_to(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session,
                                   ContextId context){
    UI_NAV_LIST_ROWS rows = ui_nav_list_session_rows_of(ui_layer, session, 0);
    session->filters[0].group = ui_nav_list_group_of_context(&rows, context);
}

uint64_t ui_nav_list_session_branch(const UI_NAV_LIST_SESSION *session, size_t spec_idx){
    if(spec_idx >= UI_NAV_LIST_SESSION_SPECS)
        return 0;
    size_t depth = session->depth[spec_idx];
    return depth ? session->levels[spec_idx][depth - 1].branch : 0;
}

bool ui_nav_list_session_open(UI_NAV_LIST_SESSION *session, size_t spec_idx,
                              const DataChoice *row){
    if(spec_idx >= UI_NAV_LIST_SESSION_SPECS ||
       session->depth[spec_idx] == UI_NAV_LIST_DEPTH_MAX)
        return false;
    UI_NAV_LIST_LEVEL *level = &session->levels[spec_idx][session->depth[spec_idx]++];
    level->branch = row->value;
    level->parent_cursor = session->cursors[spec_idx];
    level->has_label = row->label != NULL;
    snprintf(level->label, sizeof(level->label), "%s", row->label ? row->label : "");
    session->cursors[spec_idx] = (UI_NAV_LIST_CURSOR){0};
    return true;
}

bool ui_nav_list_session_close(UI_NAV_LIST_SESSION *session, size_t spec_idx){
    if(spec_idx >= UI_NAV_LIST_SESSION_SPECS || session->depth[spec_idx] == 0)
        return false;
    session->depth[spec_idx]--;
    session->cursors[spec_idx] = session->levels[spec_idx][session->depth[spec_idx]].parent_cursor;
    return true;
}

static bool ui_nav_list_session_picking_targets(const UI_NAV_LIST_SESSION *session){
    return session->input == UI_NAV_INPUT_LIST_CONNECT &&
           session->connect_pick == UI_NAV_CONNECT_PICK_TARGETS;
}

size_t ui_nav_list_session_shown_spec(const UI_NAV_LIST_SESSION *session){
    return ui_nav_list_session_picking_targets(session) ? 1 : 0;
}

UI_NAV_LIST_ROWS ui_nav_list_session_rows(UI_LAYER *ui_layer,
                                          const UI_NAV_LIST_SESSION *session){
    return ui_nav_list_session_rows_of(ui_layer, session,
                                       ui_nav_list_session_shown_spec(session));
}

bool ui_nav_list_session_sync(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session){
    if(!ui_layer_context_valid(ui_layer, session->action.context))
        return false;
    size_t spec_idx = ui_nav_list_session_shown_spec(session);
    UI_NAV_LIST_ROWS rows = ui_nav_list_session_rows_of(ui_layer, session, spec_idx);
    UI_NAV_LIST_FILTER *filter = &session->filters[spec_idx];
    if(filter->group != 0 && ui_nav_list_group_first_row(&rows, filter->group) == SIZE_MAX)
        filter->group = 0;
    ui_nav_list_cursor_follow_row(&rows, &session->cursors[spec_idx]);
    return true;
}

UiNavListPick ui_nav_list_session_pick(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session,
                                       size_t idx, DataActionResult *out_result,
                                       ContextId *out_new){
    if(out_new)
        *out_new = CONTEXT_ID_INVALID;
    size_t spec_idx = ui_nav_list_session_shown_spec(session);
    UI_NAV_LIST_ROWS rows = ui_nav_list_session_rows_of(ui_layer, session, spec_idx);
    DataChoice row;
    if(!ui_nav_list_row_at(&rows, idx, &row) ||
       !ui_nav_list_cursor_set(&rows, &session->cursors[spec_idx], idx))
        return UI_NAV_LIST_PICK_NOTHING;

    if(row.flags & DATA_CHOICE_BRANCH)
        return ui_nav_list_session_open(session, spec_idx, &row) ? UI_NAV_LIST_PICK_OPENED_LEVEL
                                                                 : UI_NAV_LIST_PICK_TOO_DEEP;

    bool connect = session->input == UI_NAV_INPUT_LIST_CONNECT;
    if(connect && spec_idx == 0){
        session->partial.connect.source = row.value;
        session->connect_pick = UI_NAV_CONNECT_PICK_TARGETS;
        // a new source has its own target list
        session->cursors[1] = (UI_NAV_LIST_CURSOR){0};
        session->depth[1] = 0;
        return UI_NAV_LIST_PICK_TO_TARGETS;
    }

    DataActionReq req = {0};
    if(connect){
        req.connect.source = session->partial.connect.source;
        req.connect.targets = &row.value;
        req.connect.target_count = 1;
    } else {
        req.add_choice.choice_value = row.value;
    }
    DataActionResult result = ui_nav_action_req_run(ui_layer, &session->action, &req, out_new);
    if(out_result)
        *out_result = result;
    return UI_NAV_LIST_PICK_RAN;
}

UiNavListBack ui_nav_list_session_back(UI_NAV_LIST_SESSION *session){
    if(ui_nav_list_session_close(session, ui_nav_list_session_shown_spec(session)))
        return UI_NAV_LIST_BACK_CLOSED_LEVEL;
    if(ui_nav_list_session_picking_targets(session)){
        session->connect_pick = UI_NAV_CONNECT_PICK_SOURCE;
        session->partial.connect.source = 0;
        return UI_NAV_LIST_BACK_TO_SOURCE;
    }
    return UI_NAV_LIST_BACK_OVER;
}

bool ui_nav_list_session_next_group(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session){
    size_t spec_idx = ui_nav_list_session_shown_spec(session);
    UI_NAV_LIST_ROWS rows = ui_nav_list_session_rows_of(ui_layer, session, spec_idx);
    UI_NAV_LIST_FILTER *filter = &session->filters[spec_idx];
    uint64_t group = ui_nav_list_group_count(&rows, 2) >= 2
                         ? ui_nav_list_group_next(&rows, filter->group)
                         : 0;
    if(group == filter->group)
        return false;
    filter->group = group;
    return true;
}

bool ui_nav_list_session_toggle_linked(UI_NAV_LIST_SESSION *session){
    if(!ui_nav_list_session_picking_targets(session))
        return false;
    session->filters[1].linked_only = !session->filters[1].linked_only;
    return true;
}

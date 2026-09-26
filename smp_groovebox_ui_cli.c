#include "data_actions.h"
#include "data_object.h"
#include "ui_layer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// limit the display to 10 rows before and after the hovered context
#define SELECTED_DIST 10 // further away contexts from cx_selected will not be displayed
#define VIEW_ROWS (SELECTED_DIST * 2 + 1) // hovered row + SELECTED_DIST either side

#define ACTION_LIST_COUNT 5 // maximum possible actions for a context when returning DataAction
#define ACTION_ARG_COUNT 4 // maximum possible DataArgSpec for a single DataAction
#define STATES_COUNT 2 // how many states on this program
#define ACTION_SOURCES_COUNT 2 // contexts whose actions are gathered: current + hovered
#define MAX_ACTION_CANDIDATES (ACTION_LIST_COUNT * ACTION_SOURCES_COUNT)

// convenient struct to hold info about a retrieved ContextId
typedef struct _context_intrf_info{
    // BORROWED STRING ADDRESS
    const char *name;
    size_t child_count;
    bool is_hidden; //is this context hidden (as informed by the data layer)
    const char* value; //value of a context with a value, BORROWED
} CONTEXT_INTRF_INFO;

// the slice of a parent's visible children that fits on screen, built around
// the hovered one. Everything here is in *visible child* terms, so hidden
// children never consume a row or trigger a truncation marker.
typedef struct _child_view{
    ContextId rows[VIEW_ROWS];
    size_t row_count;
    size_t hovered_row; // index into rows, row_count when nothing is hovered
    bool more_before;   // visible children exist above rows[0]
    bool more_after;    // ... and below rows[row_count - 1]
} CHILD_VIEW;

// one (context, action) pair still reachable this drill round, plus where in
// its own label the next letter search should resume from if it survives
// another narrow.
typedef struct _action_candidate{
    ContextId actions_context;
    DataAction action;
    size_t label_pos;   // resume position for the NEXT round's letter search
    size_t letter_pos;  // where in the label THIS round's letter was found
    char letter;         // this round's activation key, '\0' = unreachable
                         // (disabled action, or its label ran out of letters)
} ACTION_CANDIDATE;

struct termios orig_termios;
struct termios raw_termios;

// reserved letters for navigation. Only enforced while picking round-0
// letters (see helper_action_candidates_assign_letters) 
static char reserved_letters[] = {'J','K','j','k','h','l','q', '\0'};

static void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
static void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    raw_termios = orig_termios;
    raw_termios.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios);
}
// toggle to cooked/echo mode for an action's argument prompts, then back to
// raw for the normal nav loop. Unlike enableRawMode these do not touch
// atexit - they are called once per resolved action, not once per program.
static void enterCookedMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
static void enterRawModeAgain() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios);
}

enum UiPurpose{
    UI_PURPOSE_HOVERED = 1,
    UI_PURPOSE_SELECTED = 2,
    UI_PURPOSE_CURRENT = 3
};

// navigation modes
// standard - where standard hjkl navigation works
// action - where the user started selecting an action for a context
// list - an action's argument is being picked from a live list
enum Ui_Modes{
    UI_MODE_STANDARD = 1,
    UI_MODE_ACTION = 2,
    UI_MODE_LIST = 3
};

enum { LIST_SESSION_CHOICE, LIST_SESSION_CONNECT };
enum { CONNECT_PICK_SOURCE, CONNECT_PICK_TARGETS };

// where a list's cursor is, and the row it sits on, so it can follow that row
// when the list changes under it
typedef struct _list_cursor{
    size_t idx;
    uint64_t value; // 0 until the cursor has landed on a row
} LIST_CURSOR;

// an action whose argument is picked from a live list. The main loop drives it
// one frame at a time, so the list resyncs between keypresses.
// CHOICE browses specs[0] and runs the action per pick, staying open.
// CONNECT picks a source from specs[0], then toggles links to rows of
// specs[1]; h/ESC there steps back to the source list
typedef struct _list_session{
    int kind;
    ContextId context;
    DataActionType type;
    const char *label; // the action's label, static in the data layer
    DataArgSpec specs[2];
    int connect_pick;
    LIST_CURSOR cursors[2]; // one per spec
    uint64_t source_value;  // CONNECT: the source picked from specs[0]
} LIST_SESSION;

static bool helper_context_info_get( UI_LAYER *ui_layer, ContextId context, CONTEXT_INTRF_INFO *info)
{
    if (!ui_layer)
        return false;

    if (!info)
        return false;

    if (!ui_layer_context_valid(ui_layer, context))
        return false;

    const char *cx_name = ui_layer_context_name_return(ui_layer, context);
    if (!cx_name)
        return false;

    info->name = cx_name;
    info->child_count = ui_layer_context_children_count(ui_layer, context);
    info->value = ui_layer_context_value_as_string(ui_layer, context);
    info->is_hidden = ui_layer_context_is_hidden(ui_layer, context);

    return true;
}

// Add add_id, if the array is full reset the array and start from beginning
static bool helper_target_list_add_reset_on_full(UI_LAYER* ui_layer, UI_TARGET_LIST* target_list, ContextId add_id){
    if (!target_list)
        return false;
    if(!ui_layer)
        return false;
    if (!ui_layer_context_valid(ui_layer, add_id))
        return false;

    UiTargetListAddResult add_result =
        ui_layer_nav_target_list_add(target_list, add_id);
    if (add_result == UI_TARGET_LIST_ADD_ERROR)
        return false;
    if (add_result == UI_TARGET_LIST_ADD_FULL) {
        if(!ui_layer_nav_target_list_clear(target_list))
            return false;

        add_result = ui_layer_nav_target_list_add(target_list, add_id);
        if(add_result != UI_TARGET_LIST_ADD_SUCCESS)
            return false;
    }

    return true;
}
// Helper function to add a add_id, if the array is full, double its size
// TODO not used at his time, since no UI_TARGET_LISTS with growing arrays
/*
static bool helper_target_list_add_resize(UI_LAYER* ui_layer, UI_TARGET_LIST* target_list, ContextId add_id){
    if(!ui_layer)
        return false;
    if (!target_list)
        return false;
    if (!ui_layer_context_valid(ui_layer, add_id))
        return false;

    UiTargetListAddResult add_result =
        ui_layer_nav_target_list_add(target_list, add_id);
    if (add_result == UI_TARGET_LIST_ADD_ERROR)
        return false;
    if (add_result == UI_TARGET_LIST_ADD_FULL) {
        size_t capacity = ui_layer_nav_target_list_capacity(target_list);
        if (capacity > SIZE_MAX / 2)
            return false;
        size_t new_capacity = capacity * 2;
        if(!ui_layer_nav_target_list_resize(target_list, new_capacity))
            return false;

        add_result = ui_layer_nav_target_list_add(target_list, add_id);
        if(add_result != UI_TARGET_LIST_ADD_SUCCESS)
            return false;
    }

    return true;
}
*/

// ---------------------------------------------------------------------------
// visible children. Navigation and rendering must agree on which children
// exist, otherwise the cursor lands on rows that are never drawn - so every
// index that travels between them goes through the helpers below.

// a child is visible when it can be read and the data layer does not hide it.
// Deliberately the same info_get the renderer uses, so "visible" always means
// "will be drawn"
static bool helper_child_visible_at(UI_LAYER* ui_layer, ContextId parent, size_t idx, ContextId* out_child){
    ContextId child = ui_layer_context_child_at(ui_layer, parent, idx);
    CONTEXT_INTRF_INFO info;
    if(!helper_context_info_get(ui_layer, child, &info))
        return false;
    if(info.is_hidden)
        return false;
    if(out_child)
        *out_child = child;
    return true;
}

// step one visible child away from `from`, wrapping at the ends.
// returns child_count when the parent has no visible child at all
static size_t helper_child_visible_step(UI_LAYER* ui_layer, ContextId parent, size_t child_count, size_t from, bool next){
    if(child_count == 0)
        return 0;

    size_t idx = from;
    for(size_t steps = 0; steps < child_count; steps++){
        if(next)
            idx = (idx + 1 >= child_count) ? 0 : idx + 1;
        else
            idx = (idx == 0 || idx > child_count) ? child_count - 1 : idx - 1;
        if(helper_child_visible_at(ui_layer, parent, idx, NULL))
            return idx;
    }

    return child_count;
}

// `from` itself when it is visible, else the next visible child after it.
// returns child_count when the parent has no visible child at all
static size_t helper_child_visible_nearest(UI_LAYER* ui_layer, ContextId parent, size_t child_count, size_t from){
    if(from < child_count && helper_child_visible_at(ui_layer, parent, from, NULL))
        return from;
    return helper_child_visible_step(ui_layer, parent, child_count, from, true);
}

// collect up to cap visible children walking away from `from` in one
// direction (no wrapping), nearest first. Sets *more when at least one more
// visible child was left behind
static size_t helper_child_visible_collect(UI_LAYER* ui_layer, ContextId parent, size_t child_count,
                                           size_t from, bool next, size_t cap, ContextId* out, bool* more){
    size_t count = 0;
    *more = false;

    size_t idx = from;
    while(next ? (idx + 1 < child_count) : (idx > 0)){
        idx = next ? idx + 1 : idx - 1;
        ContextId child;
        if(!helper_child_visible_at(ui_layer, parent, idx, &child))
            continue;
        if(count == cap){
            *more = true;
            break;
        }
        out[count++] = child;
    }

    return count;
}

// build the on-screen slice of parent's visible children around hovered_idx
// (an index into the parent child array, as returned by
// helper_nav_context_single_purpose_set). An out of range or hidden
// hovered_idx yields an empty view
static void helper_child_view_build(UI_LAYER* ui_layer, ContextId parent, size_t child_count,
                                    size_t hovered_idx, CHILD_VIEW* view){
    view->row_count = 0;
    view->hovered_row = 0;
    view->more_before = false;
    view->more_after = false;

    ContextId hovered_child;
    if(hovered_idx >= child_count)
        return;
    if(!helper_child_visible_at(ui_layer, parent, hovered_idx, &hovered_child))
        return;

    // gather as much as either side could ever need, then split the
    // VIEW_ROWS - 1 non-hovered rows between them
    ContextId before[VIEW_ROWS - 1];
    ContextId after[VIEW_ROWS - 1];
    size_t before_count = helper_child_visible_collect(ui_layer, parent, child_count, hovered_idx,
                                                       false, VIEW_ROWS - 1, before, &view->more_before);
    size_t after_count = helper_child_visible_collect(ui_layer, parent, child_count, hovered_idx,
                                                      true, VIEW_ROWS - 1, after, &view->more_after);

    // centre the hovered row, then hand whatever the short side left unused
    // to the other one, so the screen stays full near either end of the list
    size_t take_before = before_count < SELECTED_DIST ? before_count : SELECTED_DIST;
    size_t take_after = after_count < SELECTED_DIST ? after_count : SELECTED_DIST;
    size_t spare = (VIEW_ROWS - 1) - take_before - take_after;

    size_t grow = after_count - take_after;
    if(grow > spare)
        grow = spare;
    take_after += grow;
    spare -= grow;

    grow = before_count - take_before;
    if(grow > spare)
        grow = spare;
    take_before += grow;

    // before[] came back nearest-first, so it unwinds into the view backwards
    for(size_t i = take_before; i > 0; i--)
        view->rows[view->row_count++] = before[i - 1];
    view->hovered_row = view->row_count;
    view->rows[view->row_count++] = hovered_child;
    for(size_t i = 0; i < take_after; i++)
        view->rows[view->row_count++] = after[i];

    // rows dropped by the split are truncation just as much as the ones
    // collect left behind
    if(take_before < before_count)
        view->more_before = true;
    if(take_after < after_count)
        view->more_after = true;
}

// select previous child of parent (or next if next true), skipping hidden
// children so the cursor only ever rests on a drawn row.
// cur_idx - address of the current index in the parent children array
static void helper_nav_context_scroll(UI_LAYER* ui_layer, UI_STATE* state, ContextId parent, UiPurpose purpose, size_t* cur_idx, bool next){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!cur_idx)
        return;

    CONTEXT_INTRF_INFO cx_info;
    if(!helper_context_info_get(ui_layer, parent, &cx_info))
        return;

    size_t new_idx = helper_child_visible_step(ui_layer, parent, cx_info.child_count, *cur_idx, next);
    if(new_idx >= cx_info.child_count)
        return;

    ContextId new_context = ui_layer_context_child_at(ui_layer, parent, new_idx);
    if(!ui_layer_context_valid(ui_layer, new_context))
        return;

    UI_TARGET_LIST* target_list = ui_layer_nav_target_list_begin(state, parent, purpose);
    if(!target_list)
        return;

    *cur_idx = new_idx;
    helper_target_list_add_reset_on_full(ui_layer, target_list, new_context);

    ui_layer_nav_target_list_end(state);
}

// find the parent + purpose on the state (creating the entry with capacity 1
// when it does not exist) and point it at a *visible* child: the stored one
// while it is still there and not hidden, otherwise the nearest visible one.
// Returns that child's index in the parent child array, or child_count when
// the parent has no visible child to point at - so callers can tell "nothing
// to point at" apart from "points at index 0"
static size_t
helper_nav_context_single_purpose_set(UI_LAYER *ui_layer, UI_STATE *state,
                                      ContextId parent, UiPurpose purpose,
                                      UiStalePolicy stale_policy) {
    if(!ui_layer)
        return 0;
    if(!state)
        return 0;
    if(!ui_layer_context_valid(ui_layer, parent))
        return 0;

    CONTEXT_INTRF_INFO parent_info;
    if(!helper_context_info_get(ui_layer, parent, &parent_info))
        return 0;
    if(parent_info.child_count == 0)
        return 0;

    UI_TARGET_LIST* targets = ui_layer_nav_target_list_begin(state, parent, purpose);
    if(!targets){
        // begin does not borrow when it finds nothing, so entry_set is safe here
        ui_layer_state_entry_set(state, parent, purpose, 1);
        targets = ui_layer_nav_target_list_begin(state, parent, purpose);
        if(!targets)
            return 0;
        ui_layer_target_list_set_stale_policy(targets, stale_policy);
    }

    // where the stored target sits now - child_count when it is not a child
    // of parent (anymore), which also covers a freshly created entry
    size_t stored_idx = parent_info.child_count;
    ContextId stored = ui_layer_nav_target_list_get(targets, 0);
    if(ui_layer_context_valid(ui_layer, stored)){
        for(size_t i = 0; i < parent_info.child_count; i++){
            if(ui_layer_context_child_at(ui_layer, parent, i) == stored){
                stored_idx = i;
                break;
            }
        }
    }

    // a hidden row is never drawn, so the purpose must not rest on one
    size_t visible_idx = helper_child_visible_nearest(ui_layer, parent, parent_info.child_count, stored_idx);
    if(visible_idx < parent_info.child_count && visible_idx != stored_idx){
        ContextId visible_child = ui_layer_context_child_at(ui_layer, parent, visible_idx);
        if(ui_layer_context_valid(ui_layer, visible_child)){
            ui_layer_nav_target_list_clear(targets);
            helper_target_list_add_reset_on_full(ui_layer, targets, visible_child);
        }
    }

    ui_layer_nav_target_list_end(state);

    return visible_idx;
}

// return the (single) ContextId saved for parent+purpose on state, or
// CONTEXT_ID_INVALID if there is none. Factors the begin/get/end idiom used
// in several places below.
static ContextId helper_purpose_get(UI_LAYER* ui_layer, UI_STATE* state, ContextId parent, UiPurpose purpose){
    if(!ui_layer)
        return CONTEXT_ID_INVALID;
    if(!state)
        return CONTEXT_ID_INVALID;

    UI_TARGET_LIST* target_list = ui_layer_nav_target_list_begin(state, parent, purpose);
    if(!target_list)
        return CONTEXT_ID_INVALID;

    ContextId id = ui_layer_nav_target_list_get(target_list, 0);
    ui_layer_nav_target_list_end(state);
    return id;
}

// change the parent to the ContextId saved in the purpose on the *parent
static void helper_nav_context_enter(UI_LAYER* ui_layer, UI_STATE* state, ContextId* parent, UiPurpose purpose){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!parent)
        return;
    if(!ui_layer_context_valid(ui_layer, *parent))
        return;

    ContextId cx_curr = helper_purpose_get(ui_layer, state, *parent, purpose);
    if(ui_layer_context_valid(ui_layer, cx_curr) && ui_layer_context_children_count(ui_layer, cx_curr) > 0){
        *parent = cx_curr;
    }
}

// change the parent to the parents parent
static void helper_nav_context_exit(UI_LAYER* ui_layer, UI_STATE* state, ContextId* parent){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!parent)
        return;
    if(!ui_layer_context_valid(ui_layer, *parent))
        return;

    ContextId cx_parent = ui_layer_context_parent_return(ui_layer, *parent);
    if(ui_layer_context_valid(ui_layer, cx_parent)){
        *parent = cx_parent;
    }

}

// append ctx's actions (if any) to candidates[*count..), each starting with
// label_pos/letter_pos 0 and letter '\0' - unassigned until
// helper_action_candidates_assign_letters runs over the whole gathered set.
static void helper_action_candidates_gather(UI_LAYER *ui_layer, ContextId ctx,
                                            ACTION_CANDIDATE *candidates,
                                            size_t cap, size_t *count) {
    if (!ui_layer || !candidates || !count)
        return;
    if (!ui_layer_context_valid(ui_layer, ctx))
        return;
    if (*count >= cap)
        return;

    DataAction actions[ACTION_LIST_COUNT];
    size_t action_count =
        ui_layer_context_actions(ui_layer, ctx, actions, ACTION_LIST_COUNT);
    for (size_t i = 0; i < action_count && *count < cap; i++) {
        ACTION_CANDIDATE *cand = &candidates[*count];
        cand->actions_context = ctx;
        cand->action = actions[i];
        cand->label_pos = 0;
        cand->letter_pos = 0;
        cand->letter = '\0';
        (*count)++;
    }
}

// pure: for each candidate, pick the round's activation letter by scanning
// its label from label_pos onward for the first usable character. Disabled
// actions, and a label with nothing left to try from label_pos, get letter
// '\0' (never reachable by a keypress). Several candidates CAN land on the
// same letter in one round by design - a matching keypress narrows to that
// group and this function runs again for just them, continuing each one's
// scan from just past the letter that matched (see
// helper_action_candidates_filter_matching).
static void helper_action_candidates_assign_letters(ACTION_CANDIDATE *candidates,
                                                     size_t count,
                                                     bool respect_nav_keys) {
    if (!candidates)
        return;
    for (size_t i = 0; i < count; i++) {
        ACTION_CANDIDATE *cand = &candidates[i];
        cand->letter = '\0';
        if (!cand->action.enabled)
            continue;
        const char *label = cand->action.label;
        if (!label)
            continue;
        size_t len = strlen(label);
        for (size_t j = cand->label_pos; j < len; j++) {
            char cur_char = label[j];
            if (respect_nav_keys && strchr(reserved_letters, cur_char))
                continue;
            cand->letter = cur_char;
            cand->letter_pos = j;
            break;
        }
    }
}

// keep only the candidates whose current letter == key, compacting the
// array in place; advances each survivor's label_pos to just past the
// letter that matched, ready for the next assign_letters call. Returns how
// many survived. On 0 matches, candidates/*count are left untouched - there
// is nothing to narrow, the caller decides what an unmatched key means.
static size_t helper_action_candidates_filter_matching(ACTION_CANDIDATE *candidates,
                                                        size_t *count, char key) {
    if (!candidates || !count)
        return 0;

    size_t matched = 0;
    for (size_t i = 0; i < *count; i++) {
        if (candidates[i].letter != '\0' && candidates[i].letter == key)
            matched++;
    }
    if (matched == 0)
        return 0;

    size_t insert_idx = 0;
    for (size_t i = 0; i < *count; i++) {
        if (candidates[i].letter == '\0' || candidates[i].letter != key)
            continue;
        ACTION_CANDIDATE survivor = candidates[i];
        survivor.label_pos = survivor.letter_pos + 1;
        candidates[insert_idx] = survivor;
        insert_idx++;
    }
    *count = insert_idx;
    return insert_idx;
}

// drop any candidate whose context vanished since it was gathered (a
// structural change landed between drill rounds - unlikely in this
// single-user cli, but cheap to guard against). Returns the surviving count.
static size_t helper_action_candidates_revalidate(UI_LAYER *ui_layer,
                                                   ACTION_CANDIDATE *candidates,
                                                   size_t count) {
    if (!candidates)
        return 0;
    size_t insert_idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (!ui_layer_context_valid(ui_layer, candidates[i].actions_context))
            continue;
        if (insert_idx != i)
            candidates[insert_idx] = candidates[i];
        insert_idx++;
    }
    return insert_idx;
}

// is the context among the canditates
// in other words does the context have any actions (will return true if has disabled actions)
static bool helper_action_canditates_has_source(ACTION_CANDIDATE* canditates, size_t count, ContextId context){
    if(!canditates || count == 0)
        return false;

    for(size_t i = 0; i < count; i ++){
        ACTION_CANDIDATE cur_canditate = canditates[i];
        if(cur_canditate.actions_context == context){
            return true;
        }
    }

    return false;
}

static void helper_string_print_underline(const char* string, char char_underline){
    if(!string)return;

    for(size_t i = 0; i < strlen(string); i++){
        char cur_char = string[i];
        if(cur_char == char_underline){
            printf("\033[4m%c\033[24m", cur_char);
        }
        else{
            printf("%c", cur_char);
        }
    }
}

// list_count may answer 0 for "unknown" rather than "empty" - page with list_at
// then
static size_t helper_list_count(UI_LAYER *ui_layer, ContextId context,
                                DataListId list, const DataActionReq *partial) {
    size_t count = ui_layer_context_list_count(ui_layer, context, list, partial);
    if (count > 0)
        return count;
    DataChoice row;
    while (ui_layer_context_list_at(ui_layer, context, list, partial, count,
                                    &row))
        count++;
    return count;
}

// put the cursor back on the row it was on, wherever the list has moved it;
// the first row when that row is gone
static void helper_list_cursor_anchor(UI_LAYER *ui_layer, ContextId context,
                                      DataListId list,
                                      const DataActionReq *partial,
                                      LIST_CURSOR *cur) {
    DataChoice row;
    if (cur->value != 0) {
        if (ui_layer_context_list_at(ui_layer, context, list, partial, cur->idx,
                                     &row) &&
            row.value == cur->value)
            return;
        for (size_t i = 0;
             ui_layer_context_list_at(ui_layer, context, list, partial, i, &row);
             i++) {
            if (row.value == cur->value) {
                cur->idx = i;
                return;
            }
        }
    }
    cur->idx = 0;
    cur->value =
        ui_layer_context_list_at(ui_layer, context, list, partial, 0, &row)
            ? row.value
            : 0;
}

static void helper_list_cursor_move(UI_LAYER *ui_layer, ContextId context,
                                    DataListId list,
                                    const DataActionReq *partial,
                                    LIST_CURSOR *cur, bool next) {
    size_t count = helper_list_count(ui_layer, context, list, partial);
    if (count == 0)
        return;
    if (next)
        cur->idx = (cur->idx + 1 >= count) ? 0 : cur->idx + 1;
    else
        cur->idx = (cur->idx == 0 || cur->idx >= count) ? count - 1
                                                         : cur->idx - 1;
    DataChoice row;
    cur->value = ui_layer_context_list_at(ui_layer, context, list, partial,
                                          cur->idx, &row)
                     ? row.value
                     : 0;
}

static void helper_list_render(UI_LAYER *ui_layer, ContextId context,
                               DataListId list, const DataActionReq *partial,
                               const char *title, const char *status,
                               size_t cursor_idx) {
    printf("-- %s (j/k move, l select, h/ESC back) --\n\n",
           title ? title : "");

    DataChoice row;
    bool any = false;
    for (size_t i = 0;
         ui_layer_context_list_at(ui_layer, context, list, partial, i, &row);
         i++) {
        any = true;
        printf("%s%s%s\n", i == cursor_idx ? ">" : "",
               row.label ? row.label : "?",
               (row.flags & DATA_CHOICE_LINKED) ? " [connected]" : "");
    }
    if (!any)
        printf("(no options)\n");

    if (status && status[0])
        printf("\n%s\n\n", status);
}

static void helper_action_result_msg(DataActionResult result, const char *label,
                                     char *msg, size_t msg_cap) {
    switch (result) {
    case DATA_ACTION_OK:
        snprintf(msg, msg_cap, "%s: done.", label);
        break;
    case DATA_ACTION_ERR_INVALID:
        snprintf(msg, msg_cap, "%s: invalid request.", label);
        break;
    case DATA_ACTION_ERR_NOT_ALLOWED:
        snprintf(msg, msg_cap, "%s: not allowed right now.", label);
        break;
    case DATA_ACTION_ERR_STALE:
        snprintf(msg, msg_cap, "%s: choice no longer valid, try again.", label);
        break;
    case DATA_ACTION_ERR_DATA:
        snprintf(msg, msg_cap, "%s: rejected (could not load or apply).", label);
        break;
    }
}

// a 0-arg action (REMOVE): nothing to collect, just execute.
static ContextId helper_action_do_direct(UI_LAYER *ui_layer, ContextId context,
                                         DataActionType type, const char *label,
                                         char *msg, size_t msg_cap) {
    DataActionReq req = {.type = type};
    ContextId out_new = CONTEXT_ID_INVALID;
    DataActionResult result = ui_layer_context_action_do(ui_layer, context, &req, &out_new);
    helper_action_result_msg(result, label, msg, msg_cap);
    return result == DATA_ACTION_OK ? out_new : CONTEXT_ID_INVALID;
}

// single PATH arg (ADD_FILE_PATH): cooked-mode text entry. A future UI that
// wants a real file browser here only needs to replace this function - the
// dispatch in helper_action_resolve and every other action stay untouched.
static ContextId helper_action_do_path(UI_LAYER *ui_layer, ContextId context,
                                       DataActionType type, const DataArgSpec *spec,
                                       const char *label, char *msg, size_t msg_cap) {
    char path_buf[512];
    enterCookedMode();
    printf("%s: ", spec->label ? spec->label : spec->name);
    fflush(stdout);
    bool ok = fgets(path_buf, (int)sizeof(path_buf), stdin) != NULL;
    enterRawModeAgain();
    if (!ok) {
        snprintf(msg, msg_cap, "%s: cancelled.", label);
        return CONTEXT_ID_INVALID;
    }
    size_t len = strlen(path_buf);
    if (len > 0 && path_buf[len - 1] == '\n')
        path_buf[len - 1] = '\0';

    DataActionReq req = {.type = type, .add_file_path.path = path_buf};
    ContextId out_new = CONTEXT_ID_INVALID;
    DataActionResult result = ui_layer_context_action_do(ui_layer, context, &req, &out_new);
    helper_action_result_msg(result, label, msg, msg_cap);
    return result == DATA_ACTION_OK ? out_new : CONTEXT_ID_INVALID;
}

static void helper_list_session_start(LIST_SESSION *session, int kind,
                                      ContextId context, DataActionType type,
                                      const char *label,
                                      const DataArgSpec *specs,
                                      size_t spec_count) {
    *session = (LIST_SESSION){0};
    session->kind = kind;
    session->context = context;
    session->type = type;
    session->label = label;
    for (size_t i = 0; i < spec_count && i < 2; i++)
        session->specs[i] = specs[i];
    session->connect_pick = CONNECT_PICK_SOURCE;
}

// one frame of a list session: re-anchor, draw, read a key, act. Returns false
// once the session is over, with its closing message in msg
static bool helper_list_session_frame(UI_LAYER *ui_layer, LIST_SESSION *session,
                                      char *msg, size_t msg_cap) {
    if (!ui_layer_context_valid(ui_layer, session->context)) {
        snprintf(msg, msg_cap, "%s: no longer available.", session->label);
        return false;
    }

    bool picking_targets = session->kind == LIST_SESSION_CONNECT &&
                           session->connect_pick == CONNECT_PICK_TARGETS;
    const DataArgSpec *spec = &session->specs[picking_targets ? 1 : 0];
    LIST_CURSOR *cur = &session->cursors[picking_targets ? 1 : 0];
    DataActionReq partial = {.type = session->type};
    if (picking_targets)
        partial.connect.source = session->source_value;

    helper_list_cursor_anchor(ui_layer, session->context, spec->list, &partial,
                              cur);
    helper_list_render(ui_layer, session->context, spec->list, &partial,
                       spec->label ? spec->label : spec->name, msg, cur->idx);

    int input = getchar();
    // a result is shown for exactly one frame
    msg[0] = '\0';

    if (input == '\e' || input == 'h') {
        if (picking_targets) {
            session->connect_pick = CONNECT_PICK_SOURCE;
            return true;
        }
        snprintf(msg, msg_cap, "%s: cancelled.", session->label);
        return false;
    }
    if (input == 'j' || input == 'k') {
        helper_list_cursor_move(ui_layer, session->context, spec->list,
                                &partial, cur, input == 'j');
        return true;
    }
    if (input != 'l')
        return true;

    DataChoice picked;
    if (!ui_layer_context_list_at(ui_layer, session->context, spec->list,
                                  &partial, cur->idx, &picked))
        return true;

    if (session->kind == LIST_SESSION_CONNECT && !picking_targets) {
        session->source_value = picked.value;
        session->connect_pick = CONNECT_PICK_TARGETS;
        session->cursors[1] = (LIST_CURSOR){0};
        return true;
    }

    DataActionReq req = {.type = session->type};
    uint64_t target_value = picked.value;
    if (session->kind == LIST_SESSION_CONNECT) {
        req.connect.source = session->source_value;
        req.connect.targets = &target_value;
        req.connect.target_count = 1;
    } else {
        req.add_choice.choice_value = picked.value;
    }
    ContextId out_new = CONTEXT_ID_INVALID;
    DataActionResult result =
        ui_layer_context_action_do(ui_layer, session->context, &req, &out_new);
    helper_action_result_msg(result, session->label, msg, msg_cap);
    return true;
}

// dispatch to the function that knows how to collect that action's arguments.
// Returns true when that is a list: the session is set up in *session and the
// main loop drives it from then on
static bool helper_action_resolve(UI_LAYER *ui_layer, ACTION_CANDIDATE *chosen,
                                  LIST_SESSION *session, char *msg,
                                  size_t msg_cap) {
    if (!msg || msg_cap == 0)
        return false;
    msg[0] = '\0';
    if (!ui_layer || !chosen || !session)
        return false;

    ContextId context = chosen->actions_context;
    DataActionType type = chosen->action.type;
    const char *label = chosen->action.label ? chosen->action.label : "action";

    DataArgSpec specs[ACTION_ARG_COUNT];
    size_t arg_count = ui_layer_context_action_args(ui_layer, context, type,
                                                     specs, ACTION_ARG_COUNT);

    if (type == DATA_ACTION_CONNECT) {
        if (arg_count < 2) {
            snprintf(msg, msg_cap, "%s: misconfigured.", label);
            return false;
        }
        helper_list_session_start(session, LIST_SESSION_CONNECT, context, type,
                                  label, specs, 2);
        return true;
    }
    if (arg_count == 0) {
        helper_action_do_direct(ui_layer, context, type, label, msg, msg_cap);
        return false;
    }
    if (arg_count == 1 && specs[0].kind == DATA_ARG_PATH) {
        printf("\n-- %s --\n", label);
        helper_action_do_path(ui_layer, context, type, &specs[0], label, msg,
                              msg_cap);
        return false;
    }
    if (arg_count == 1 && specs[0].kind == DATA_ARG_CHOICE) {
        helper_list_session_start(session, LIST_SESSION_CHOICE, context, type,
                                  label, specs, 1);
        return true;
    }

    snprintf(msg, msg_cap, "%s: unsupported arguments.", label);
    return false;
}

static void helper_program_destroy(UI_LAYER* ui_layer, UI_STATE** states, size_t states_capacity){
    ui_layer_destroy(ui_layer, states, states_capacity);

    printf("\nCleaned everything, closing the up\n");
    disableRawMode();
}

int main() {
    enableRawMode();
    UI_LAYER* ui_layer = ui_layer_init();

    // if ui_layer failed to initialize analyze the error write it and exit
    if (!ui_layer) {
        printf("\nCould not start the ui_layer\n");
        exit(1);
    }

    // create the various states
    // this state will always stay on the root context
    UI_STATE* state_root = ui_layer_state_init(ui_layer);
    ContextId id_root = ui_layer_state_root_return(ui_layer);

    // main ui state, for general program navigation
    UI_STATE* state_main = ui_layer_state_init(ui_layer);
    // current ContextId of the state_main;
    ContextId state_main_context_current = id_root;
    // the current context parent, useful when current is stale/deleted
    ContextId state_main_context_current_parent = id_root;
    // which idx in the state_main_context_current children array is the UI_PURPOSE_HOVERED
    size_t state_main_hovered_idx = 0;

    // this array will contain all of the states
    UI_STATE* states_all[STATES_COUNT] = {state_main, state_root};

    // currently focused state
    UI_STATE* state_current = state_main;
    ContextId* state_current_context_current = &state_main_context_current;
    size_t *state_current_hovered_idx = &state_main_hovered_idx;

    // navigation mode
    size_t ui_nav_mode = UI_MODE_STANDARD;
    // action candidates for the current drill round. Only meaningful while
    // ui_nav_mode == UI_MODE_ACTION; UI_MODE_STANDARD rebuilds this fresh
    ACTION_CANDIDATE candidates[MAX_ACTION_CANDIDATES] = {0};
    size_t candidate_count = 0;
    // result of the last resolved action, shown once at the top of the frame
    char action_status_msg[128] = {0};
    // the list being browsed while ui_nav_mode == UI_MODE_LIST
    LIST_SESSION list_session = {0};

    while (1) {
        // erase the terminal
        printf("\033[2J\033[H");

        // update the ui_layer - it updates the data underneath
        ui_layer_update_cycle(ui_layer);

        // let each view react to contexts that appeared / were removed
        for (size_t si = 0; si < STATES_COUNT; si++) {
            UiReconcileResult rr =
                ui_layer_state_reconcile(ui_layer, states_all[si]);
            // the view's cursor fell behind, so per-event anchoring was lost -
            // if the ContextIds this UI uses are not valid anymore, fallback to
            // the root ContextId
            if (rr == UI_RECONCILE_REBUILD && states_all[si] == state_main &&
                !ui_layer_context_valid(ui_layer, state_main_context_current)) {
                state_main_context_current = id_root;
            }
        }

        // show the state_main info
        // first update the contexts if they are deleted
        if (!ui_layer_context_valid(ui_layer, state_main_context_current)) {
            if (ui_layer_context_valid(ui_layer,
                                       state_main_context_current_parent)) {
                state_main_context_current = state_main_context_current_parent;
            } else {
                state_main_context_current = id_root;
            }
        }

        state_main_hovered_idx = helper_nav_context_single_purpose_set(
            ui_layer, state_main, state_main_context_current, UI_PURPOSE_HOVERED,
            (UiStalePolicy){.mode = UI_STALE_PREV_SIBLING});

        // update the parent
        ContextId parent = ui_layer_context_parent_return(ui_layer, state_main_context_current);
        if(ui_layer_context_valid(ui_layer, parent)){
            state_main_context_current_parent = parent;
        }
        else{
            state_main_context_current_parent = id_root;
        }


        printf("\e[22m");
        // a list session owns the whole frame; it runs after the update cycle
        // above, so it always draws the list as it is now
        if (ui_nav_mode == UI_MODE_LIST) {
            if (!helper_list_session_frame(ui_layer, &list_session,
                                           action_status_msg,
                                           sizeof(action_status_msg)))
                ui_nav_mode = UI_MODE_STANDARD;
            continue;
        }
        if(ui_nav_mode == UI_MODE_ACTION)
            printf("\e[2m");

        CONTEXT_INTRF_INFO state_main_current_info;
        if (helper_context_info_get(ui_layer, state_main_context_current,
                                    &state_main_current_info)) {
            printf("----| %s |----\n\n", state_main_current_info.name);

            // the window of children that fits on screen around the hovered
            // one. The builder owns the hidden/limit geometry, so all that is
            // left here is drawing rows
            CHILD_VIEW view;
            helper_child_view_build(ui_layer, state_main_context_current,
                                    state_main_current_info.child_count,
                                    state_main_hovered_idx, &view);

            if (view.more_before)
                printf("-...-\n");
            for (size_t row = 0; row < view.row_count; row++) {
                CONTEXT_INTRF_INFO child_info;
                if (!helper_context_info_get(ui_layer, view.rows[row],
                                             &child_info))
                    continue;
                if (row == view.hovered_row && state_current == state_main)
                    printf(">%s", child_info.name);
                else
                    printf("%s", child_info.name);
                child_info.value ? printf(" --- %s\n", child_info.value)
                                 : printf("\n");
            }
            if (view.more_after)
                printf("-...-\n");
        }

        // --------------------------------------------------
        // Actions for the state_current

        // action candidates: either keep drilling the previous round
        // (revalidating first, in case something acted on disappeared
        // between rounds) or, in standard mode, rebuild round 0 fresh from
        // the live current + hovered contexts every frame.
        ContextId state_current_context_hovered = helper_purpose_get(
            ui_layer, state_current, *state_current_context_current,
            UI_PURPOSE_HOVERED);
        bool need_gather = true;
        if (ui_nav_mode == UI_MODE_ACTION) {
            candidate_count = helper_action_candidates_revalidate(
                ui_layer, candidates, candidate_count);
            if (candidate_count > 0)
                need_gather = false;
            else
                ui_nav_mode = UI_MODE_STANDARD;
        }
        if (need_gather) {
            candidate_count = 0;
            helper_action_candidates_gather(
                ui_layer, *state_current_context_current, candidates,
                MAX_ACTION_CANDIDATES, &candidate_count);
            helper_action_candidates_gather(
                ui_layer, state_current_context_hovered, candidates,
                MAX_ACTION_CANDIDATES, &candidate_count);
            helper_action_candidates_assign_letters(candidates, candidate_count,
                                                    true);
        }
        // show possible actions for the current drill round
        printf("\n-------------------------------------------------------------"
               "---------------------------------------\n");

        ContextId action_sources[ACTION_SOURCES_COUNT] = {*state_current_context_current,
                                                           state_current_context_hovered};
        for (size_t s = 0; s < ACTION_SOURCES_COUNT; s++) {
            ContextId src = action_sources[s];
            CONTEXT_INTRF_INFO src_info;
            if (!helper_context_info_get(ui_layer, src, &src_info))
                continue;
            if (helper_action_canditates_has_source(candidates, candidate_count,
                                                    src))
                printf("%s: ", src_info.name);
            for (size_t i = 0; i < candidate_count; i++) {
                if (candidates[i].actions_context != src)
                    continue;
                if (ui_nav_mode == UI_MODE_ACTION)
                    printf("\e[22m");
                helper_string_print_underline(candidates[i].action.label,
                                              candidates[i].letter);
                if (!candidates[i].action.enabled)
                    printf(" (disabled)");
                printf(" ");
                if (ui_nav_mode == UI_MODE_ACTION)
                    printf("\e[2m");
            }
        }
        printf("\n");
        printf("-------------------------------------------------------------"
               "---------------------------------------\n");

        // show the state_root info
        printf("\n");
        CONTEXT_INTRF_INFO cx_root_info;
        if(helper_context_info_get(ui_layer, id_root, &cx_root_info)){
            for(size_t i = 0; i < cx_root_info.child_count; i++){
                ContextId root_child =
                    ui_layer_context_child_at(ui_layer, id_root, i);
                if (ui_layer_context_valid(ui_layer, root_child)) {
                    CONTEXT_INTRF_INFO cx_root_child_info;
                    if(helper_context_info_get(ui_layer, root_child, &cx_root_child_info)){
                        printf("| %lu_%s |", i, cx_root_child_info.name);
                    }
                }
                if(i == cx_root_info.child_count - 1)
                    printf("\n");
            }
        }

        // system messages
        printf("\nMessages:\n");
        if (action_status_msg[0])
            printf("%s\n", action_status_msg);

        // get user inputs
        int input = getchar();
        unsigned int exit = 0;

        if (input == '\e') {
            // cancel any drill in progress, back to standard navigation
            ui_nav_mode = UI_MODE_STANDARD;
            candidate_count = 0;
            action_status_msg[0] = '\0';
        } else {
            size_t matches = helper_action_candidates_filter_matching(
                candidates, &candidate_count, (char)input);
            if (matches == 1) {
                bool listing = helper_action_resolve(
                    ui_layer, &candidates[0], &list_session, action_status_msg,
                    sizeof(action_status_msg));
                candidate_count = 0;
                ui_nav_mode = listing ? UI_MODE_LIST : UI_MODE_STANDARD;
            } else if (matches > 1) {
                ui_nav_mode = UI_MODE_ACTION;
                helper_action_candidates_assign_letters(candidates,
                                                        candidate_count, false);
            } else if (ui_nav_mode == UI_MODE_STANDARD) {
                // not an action letter this round - ordinary navigation.
                // nothing matched, so candidates/candidate_count are
                // untouched (still this frame's round-0 set).
                switch (input) {
                case 'J':
                    break;
                case 'K':
                    break;
                case 'j':
                    helper_nav_context_scroll(
                        ui_layer, state_current, *state_current_context_current,
                        UI_PURPOSE_HOVERED, state_current_hovered_idx, true);
                    break;
                case 'k':
                    helper_nav_context_scroll(
                        ui_layer, state_current, *state_current_context_current,
                        UI_PURPOSE_HOVERED, state_current_hovered_idx, false);
                    break;
                case 'l':
                    helper_nav_context_enter(ui_layer, state_current,
                                             state_current_context_current,
                                             UI_PURPOSE_HOVERED);
                    break;
                case 'h':
                    helper_nav_context_exit(ui_layer, state_current,
                                            state_current_context_current);
                    break;
                case 'q':
                    exit = 1;
                    break;
                }
            }
            // else: ui_nav_mode == UI_MODE_ACTION and the key matched
            // nothing - navigation is disabled here, so it is simply
            // ignored; candidates/letters stay exactly as they were.
        }

        if (exit == 1)
            break;
    }

    helper_program_destroy(ui_layer, states_all, STATES_COUNT);

    return 0;
}

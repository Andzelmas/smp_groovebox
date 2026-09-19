#include "data_actions.h"
#include "ui_layer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define SELECTED_DIST 10 // further away contexts from cx_selected will not be displayed
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
enum Ui_Modes{
    UI_MODE_STANDARD = 1,
    UI_MODE_ACTION = 2
};

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

// select previous child of parent (or next if next true).
// cur_idx - address of the current index in the parent children array
static void helper_nav_context_scroll(UI_LAYER* ui_layer, UI_STATE* state, ContextId parent, UiPurpose purpose, size_t* cur_idx, bool next){
    if(!ui_layer)
        return;
    if(!state)
        return;
    if(!cur_idx)
        return;

    UI_TARGET_LIST* target_list = ui_layer_nav_target_list_begin(state, parent, purpose);
    if(!target_list)
        return;

    int old_idx = (int)*cur_idx;
    int new_idx = 0;
    if (!next)
        new_idx = old_idx - 1;
    else
        new_idx = old_idx + 1;

    CONTEXT_INTRF_INFO cx_info;
    if(helper_context_info_get(ui_layer, parent, &cx_info)){
        if(new_idx < 0){
            new_idx = (int)cx_info.child_count - 1;
        }
        if(new_idx >= (int)cx_info.child_count){
            new_idx = 0;
        }

        ContextId new_context = ui_layer_context_child_at(ui_layer, parent, (size_t)new_idx);

        if(ui_layer_context_valid(ui_layer, new_context)){
            *cur_idx = (size_t)new_idx;
            helper_target_list_add_reset_on_full(ui_layer, target_list, new_context);
        }
    }

    ui_layer_nav_target_list_end(state);
}

// find the parent + purpose on the state and return the target ContextId index
// in the parent child array if the parent + purpose does not exist create it
// (capacity 1) and add the first child as the target ContextId
static size_t
helper_nav_context_single_purpose_set(UI_LAYER *ui_layer, UI_STATE *state,
                                      ContextId parent, UiPurpose purpose,
                                      UiStalePolicy stale_policy) {
    if(!ui_layer)
        return 0;
    if(!ui_layer_context_valid(ui_layer, parent))
        return 0;
    CONTEXT_INTRF_INFO state_main_current_info;
    if(helper_context_info_get(ui_layer, parent, &state_main_current_info)){
        if(state_main_current_info.child_count > 0){
            UI_TARGET_LIST* selected_target = ui_layer_nav_target_list_begin(state, parent, purpose);
            if (selected_target) {

                size_t return_idx = 0;
                ContextId cur_purpose = ui_layer_nav_target_list_get(selected_target, 0);
                for(size_t i = 0; i < state_main_current_info.child_count; i ++){
                    ContextId cur_child = ui_layer_context_child_at(ui_layer, parent, i);
                    if(cur_child == cur_purpose){
                       return_idx = i;
                       break;
                    }
                }
                ui_layer_nav_target_list_end(state);
                return return_idx;
            }
            else{
                ui_layer_state_entry_set(state, parent, purpose, 1);
                UI_TARGET_LIST* targets = ui_layer_nav_target_list_begin(state, parent, purpose);

                if(targets){
                    ui_layer_target_list_set_stale_policy(targets,
                                                          stale_policy);
                    ContextId new_purpose =
                        ui_layer_context_child_at(ui_layer, parent, 0);
                    if (ui_layer_context_valid(ui_layer, new_purpose)) {
                        helper_target_list_add_reset_on_full(ui_layer, targets,
                                                             new_purpose);
                    }
                    ui_layer_nav_target_list_end(state);
                    return 0;
                }
            }
        }
    }

    return 0;
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
    if(ui_layer_context_valid(ui_layer, cx_curr)){
        *parent = cx_curr;
    }
}

// change the parent to the parents parent
static void helper_nav_context_exit(UI_LAYER* ui_layer, UI_STATE* state, ContextId* parent, UiPurpose purpose){
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

typedef enum {
    LIST_STEP_CONTINUE,   // cursor moved or just redrawn, keep looping
    LIST_STEP_SELECTED,   // l on a valid row - *out filled
    LIST_STEP_CANCELLED,  // h or ESC
} ListStepResult;

// one render + one keypress against a live DataChoice list: j/k move
// *cursor (probing the neighbouring index and wrapping on list_at's false -
// list_at's own false return is the only "no more rows" signal used here,
// list_count is never needed), l selects the highlighted row into *out,
// h/ESC cancel. *cursor is caller-owned, so it survives across calls -
// including across a mode switch that later resumes the same list (see
// helper_action_do_connect). This is the one primitive every list-based
// action interaction below is built from.
//
// override_value/override_flags let a caller override one row's flags for
// this render only (override_value == 0, never a real DataChoice.value,
// means "no override"). This exists because a list's flags can come from a
// live external source whose own change-notification lags behind an action
// this same caller just performed (e.g. JACK: app_jack_ports_connected reads
// a best-effort local cache, not the server, so a just-toggled row can still
// read as its pre-toggle state for a frame)
static ListStepResult helper_choice_list_step(UI_LAYER *ui_layer, ContextId context,
                                              DataListId list,
                                              const DataActionReq *partial,
                                              const char *title, const char *status,
                                              size_t *cursor, DataChoice *out,
                                              uint64_t override_value,
                                              uint32_t override_flags) {
    printf("\033[2J\033[H-- %s (j/k move, l select, h/ESC back) --\n\n",
          title ? title : "");

    DataChoice row;
    bool any = false;
    for (size_t i = 0;
         ui_layer_context_list_at(ui_layer, context, list, partial, i, &row);
         i++) {
        any = true;
        uint32_t flags = (override_value != 0 && row.value == override_value)
                            ? override_flags : row.flags;
        printf("%s%s%s\n", i == *cursor ? ">" : "", row.label ? row.label : "?",
              (flags & DATA_CHOICE_LINKED) ? " [connected]" : "");
    }
    if (!any)
        printf("(no options)\n");

    if (status && status[0])
        printf("\n%s\n\n", status);

    int input = getchar();
    if (input == '\e' || input == 'h')
        return LIST_STEP_CANCELLED;
    if (input == 'j') {
        DataChoice probe;
        size_t next = *cursor + 1;
        *cursor = ui_layer_context_list_at(ui_layer, context, list, partial,
                                           next, &probe)
                    ? next : 0;
    } else if (input == 'k') {
        if (*cursor == 0) {
            DataChoice probe;
            while (ui_layer_context_list_at(ui_layer, context, list, partial,
                                            *cursor + 1, &probe))
                (*cursor)++;
        } else {
            (*cursor)--;
        }
    } else if (input == 'l') {
        if (ui_layer_context_list_at(ui_layer, context, list, partial,
                                     *cursor, out))
            return LIST_STEP_SELECTED;
    }
    return LIST_STEP_CONTINUE;
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

// single CHOICE arg (ADD_CHOICE): pick, execute, keep browsing - the result
// shows inline via helper_choice_list_step's status line, so one session can
// add several items (e.g. plugins) without leaving the picker. Ends on h/ESC.
static ContextId helper_action_do_choice_repeat(UI_LAYER *ui_layer, ContextId context,
                                                DataActionType type, const DataArgSpec *spec,
                                                const char *label, char *msg, size_t msg_cap) {
    size_t cursor = 0;
    ContextId last_new = CONTEXT_ID_INVALID;
    msg[0] = '\0';
    DataActionReq partial = {.type = type};
    while (1) {
        DataChoice picked;
        ListStepResult step = helper_choice_list_step(
            ui_layer, context, spec->list, &partial,
            spec->label ? spec->label : spec->name, msg, &cursor, &picked, 0, 0);
        msg[0] = '\0';
        if (step == LIST_STEP_CANCELLED)
            break;
        if (step != LIST_STEP_SELECTED)
            continue;

        DataActionReq req = {.type = type, .add_choice.choice_value = picked.value};
        ContextId out_new = CONTEXT_ID_INVALID;
        DataActionResult result = ui_layer_context_action_do(ui_layer, context, &req, &out_new);
        helper_action_result_msg(result, label, msg, msg_cap);
        if (result == DATA_ACTION_OK)
            last_new = out_new;
    }
    if (msg[0] == '\0')
        snprintf(msg, msg_cap, "%s: cancelled.", label);
    return last_new;
}

// CONNECT: a dedicated two-mode navigator. SOURCE mode picks specs[0]'s list once and
// switches to TARGETS mode; TARGETS mode browses specs[1]'s list scoped to
// the chosen source (each row's [connected] marker comes from
// DATA_CHOICE_LINKED) and toggles connect/disconnect immediately per row,
// staying open. h/ESC in TARGETS steps back to SOURCE (cursor preserved);
// h/ESC in SOURCE leaves the whole picker.
static ContextId helper_action_do_connect(UI_LAYER *ui_layer, ContextId context,
                                          const DataArgSpec *specs, size_t spec_count,
                                          const char *label, char *msg, size_t msg_cap) {
    msg[0] = '\0';
    if (spec_count < 2) {
        snprintf(msg, msg_cap, "%s: misconfigured.", label);
        return CONTEXT_ID_INVALID;
    }
    const DataArgSpec *source_spec = &specs[0];
    const DataArgSpec *targets_spec = &specs[1];

    enum { CONNECT_MODE_SOURCE, CONNECT_MODE_TARGETS } mode = CONNECT_MODE_SOURCE;
    size_t source_cursor = 0;
    size_t target_cursor = 0;
    DataChoice source_choice = {0};
    ContextId last_new = CONTEXT_ID_INVALID;
    // the row we just toggled, and what we know its flags must now be - see
    // helper_choice_list_step's override_value doc. Reset to "none" (0) once
    // shown
    uint64_t override_value = 0;
    uint32_t override_flags = 0;

    while (1) {
        if (mode == CONNECT_MODE_SOURCE) {
            DataActionReq partial = {.type = DATA_ACTION_CONNECT};
            DataChoice picked;
            ListStepResult step = helper_choice_list_step(
                ui_layer, context, source_spec->list, &partial,
                source_spec->label ? source_spec->label : source_spec->name,
                msg, &source_cursor, &picked, 0, 0);
            if (step == LIST_STEP_CANCELLED)
                break;
            if (step == LIST_STEP_SELECTED) {
                source_choice = picked;
                target_cursor = 0;
                msg[0] = '\0';
                mode = CONNECT_MODE_TARGETS;
            }
            continue;
        }

        DataActionReq partial = {.type = DATA_ACTION_CONNECT,
                                 .connect.source = source_choice.value};
        DataChoice picked;
        ListStepResult step = helper_choice_list_step(
            ui_layer, context, targets_spec->list, &partial,
            targets_spec->label ? targets_spec->label : targets_spec->name,
            msg, &target_cursor, &picked, override_value, override_flags);
        msg[0] = '\0';
        override_value = 0;
        if (step == LIST_STEP_CANCELLED) {
            mode = CONNECT_MODE_SOURCE;
            continue;
        }
        if (step != LIST_STEP_SELECTED)
            continue;

        uint64_t target_value = picked.value;
        DataActionReq req = {.type = DATA_ACTION_CONNECT,
                             .connect.source = source_choice.value,
                             .connect.targets = &target_value,
                             .connect.target_count = 1};
        ContextId out_new = CONTEXT_ID_INVALID;
        DataActionResult result = ui_layer_context_action_do(ui_layer, context, &req, &out_new);
        helper_action_result_msg(result, label, msg, msg_cap);
        if (result == DATA_ACTION_OK) {
            last_new = out_new;
            // trk_action_do toggles: already-linked -> disconnect, else
            // connect. picked.flags is this row's LINKED state as read just
            // before the toggle, so the new state is its exact negation
            override_value = picked.value;
            override_flags = picked.flags ^ DATA_CHOICE_LINKED;
        }
    }
    if (msg[0] == '\0')
        snprintf(msg, msg_cap, "%s: cancelled.", label);
    return last_new;
}

// dispatch to the function that knows how to collect that action's arguments
// and execute it Extending or replacing how an action behaves (a real file
// browser for ADD_FILE_PATH, a different CONNECT UI) means adding/swapping one
// helper_action_do_* function and one dispatch line here - every other action's
// function is untouched.
static ContextId helper_action_resolve(UI_LAYER *ui_layer, ACTION_CANDIDATE *chosen,
                                       char *msg, size_t msg_cap) {
    if (!msg || msg_cap == 0)
        return CONTEXT_ID_INVALID;
    msg[0] = '\0';
    if (!ui_layer || !chosen)
        return CONTEXT_ID_INVALID;

    ContextId context = chosen->actions_context;
    DataActionType type = chosen->action.type;
    const char *label = chosen->action.label ? chosen->action.label : "action";

    DataArgSpec specs[ACTION_ARG_COUNT];
    size_t arg_count = ui_layer_context_action_args(ui_layer, context, type,
                                                     specs, ACTION_ARG_COUNT);
    if (arg_count > 0)
        printf("\n-- %s --\n", label);

    if (type == DATA_ACTION_CONNECT)
        return helper_action_do_connect(ui_layer, context, specs, arg_count,
                                        label, msg, msg_cap);
    if (arg_count == 0)
        return helper_action_do_direct(ui_layer, context, type, label, msg, msg_cap);
    if (arg_count == 1 && specs[0].kind == DATA_ARG_PATH)
        return helper_action_do_path(ui_layer, context, type, &specs[0], label,
                                     msg, msg_cap);
    if (arg_count == 1 && specs[0].kind == DATA_ARG_CHOICE)
        return helper_action_do_choice_repeat(ui_layer, context, type, &specs[0],
                                              label, msg, msg_cap);

    snprintf(msg, msg_cap, "%s: unsupported arguments.", label);
    return CONTEXT_ID_INVALID;
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

    while (1) {
        // erase the terminal
        printf("\033[2J\033[H");
        // update the interface, of course should be in a loop
        ui_layer_update_cycle(ui_layer);

        // let each view react to contexts that appeared / were removed
        for (size_t si = 0; si < STATES_COUNT; si++) {
            UiReconcileResult rr =
                ui_layer_state_reconcile(ui_layer, states_all[si]);
            if (rr == UI_RECONCILE_REBUILD && states_all[si] == state_main) {
                // the view's cursor fell behind - fall back to the root
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
        ContextId state_main_id_hovered = helper_purpose_get(
            ui_layer, state_main, state_main_context_current, UI_PURPOSE_HOVERED);

        // update the parent
        ContextId parent = ui_layer_context_parent_return(ui_layer, state_main_context_current);
        if(ui_layer_context_valid(ui_layer, parent)){
            state_main_context_current_parent = parent;
        }
        else{
            state_main_context_current_parent = id_root;
        }


        printf("\e[22m");
        if(ui_nav_mode == UI_MODE_ACTION)
            printf("\e[2m");

        CONTEXT_INTRF_INFO state_main_current_info;
        if(helper_context_info_get(ui_layer, state_main_context_current, &state_main_current_info)){
            printf("----| %s |----\n\n", state_main_current_info.name);
            for(size_t i = 0; i < state_main_current_info.child_count; i++){
                CONTEXT_INTRF_INFO state_main_current_child_info;
                ContextId cur_child = ui_layer_context_child_at(ui_layer, state_main_context_current, i);
                if(helper_context_info_get(ui_layer, cur_child, &state_main_current_child_info)){
                    if(state_main_current_child_info.is_hidden)
                        continue;
                    if(cur_child == state_main_id_hovered && state_current == state_main){
                        printf(">%s", state_main_current_child_info.name);
                    }
                    else{
                        printf("%s", state_main_current_child_info.name);
                    }
                    state_main_current_child_info.value
                        ? printf(" --- %s\n", state_main_current_child_info.value)
                        : printf("\n");
                }
            }
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
            helper_action_candidates_gather(ui_layer, *state_current_context_current,
                                            candidates, MAX_ACTION_CANDIDATES,
                                            &candidate_count);
            helper_action_candidates_gather(ui_layer, state_current_context_hovered,
                                            candidates, MAX_ACTION_CANDIDATES,
                                            &candidate_count);
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
                helper_action_resolve(ui_layer, &candidates[0],
                                      action_status_msg,
                                      sizeof(action_status_msg));
                candidate_count = 0;
                ui_nav_mode = UI_MODE_STANDARD;
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
                                            state_current_context_current,
                                            UI_PURPOSE_HOVERED);
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

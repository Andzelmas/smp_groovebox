#include "data_actions.h"
#include "data_object.h"
#include "ui_layer.h"
#include "ui_nav.h"
#include "ui_windowing.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// limit the display to 10 rows before and after the hovered context
#define VIEW_ROWS 21

#define STATES_COUNT 2 // how many states on this program
#define ACTION_SOURCES_COUNT 10 // contexts whose actions are gathered: hovered + ContexIds up to root Context
#define MAX_ACTION_CANDIDATES (UI_NAV_CONTEXT_ACTIONS_MAX * ACTION_SOURCES_COUNT)
// while waiting for a key, pull fresh data from the ui_layer this often (ms)
#define INPUT_WAIT_TICK_MS 30
// helper_input_wait result when the screen has to be drawn again - not a key
// (and not EOF, which is -1)
#define INPUT_WAIT_REDRAW (-2)

// the views helper_input_wait keeps in step with the data while no key is
// pressed
typedef struct _views_sync{
    UI_LAYER *ui_layer;
    UI_STATE **states;
    size_t states_count;
    // ui_layer_wake_fd(ui_layer, INPUT_WAIT_TICK_MS), requested once at
    // startup; -1 if unavailable - helper_input_wait falls back to ticking
    int wake_fd;
} VIEWS_SYNC;

// an action still reachable this drill round, plus where in its own label the
// next letter search should resume from if it survives another narrow.
typedef struct _action_candidate{
    UI_NAV_ACTION entry;
    size_t label_pos;   // resume position for the NEXT round's letter search
    size_t letter_pos;  // where in the label THIS round's letter was found
    char letter;         // this round's activation key, '\0' = unreachable
                         // (disabled action, or its label ran out of letters)
} ACTION_CANDIDATE;

struct termios orig_termios;
struct termios raw_termios;

// reserved letters for navigation. Only enforced while picking round-0
// letters (see helper_action_candidates_assign_letters) 
static char reserved_letters[] = {'-','+','j','k','h','l','q', '\0'};

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
    UI_PURPOSE_CURRENT = 3,
    // the rows drawn this frame, so a change to any of them - not only the
    // hovered one - makes reconcile report the view as changed
    UI_PURPOSE_SHOWN = 4
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
// when the list changes under it. prev/next are the shown rows around it, to
// find where it was if its row goes away
typedef struct _list_cursor{
    size_t idx;
    uint64_t value; // 0 until the cursor has landed on a row
    uint64_t prev;  // 0 when there is none
    uint64_t next;  // 0 when there is none
} LIST_CURSOR;

// what narrows a list. The two parts compose
typedef struct _list_filter{
    uint64_t group;   // DataChoice.group_key to keep, 0 = every group
    bool linked_only; // keep only DATA_CHOICE_LINKED rows
} LIST_FILTER;

// how many categories deep a list can be opened
#define LIST_DEPTH_MAX 16
// how many list args one session browses (CONNECT: source, targets)
#define LIST_MAX_ARG_SPECS 2

// one opened category of a list (a DATA_CHOICE_BRANCH row): its value is the
// branch listed, parent_cursor is the level above's, given back on close
typedef struct _list_level{
    uint64_t branch;
    LIST_CURSOR parent_cursor;
    char label[64]; // copied - a row's label is borrowed
} LIST_LEVEL;

// an action whose argument is picked from a live list. The main loop drives it
// one frame at a time, so the list resyncs between keypresses.
// CHOICE browses specs[0] and runs the action per pick, staying open.
// CONNECT picks a source from specs[0], then toggles links to rows of
// specs[1]; h/ESC there steps back to the source list.
// l on a category row opens it, h/ESC closes it again first
typedef struct _list_session{
    int kind;
    ContextId context;
    DataActionType type;
    uint32_t id; // the action's DataAction.id
    const char *label; // the action's label, static in the data layer
    DataArgSpec specs[LIST_MAX_ARG_SPECS];
    int connect_pick;
    // one per spec, of its innermost opened level
    LIST_CURSOR cursors[LIST_MAX_ARG_SPECS];
    LIST_FILTER filters[LIST_MAX_ARG_SPECS]; // one per spec
    uint64_t source_value; // CONNECT: the source picked from specs[0]
    // opened categories, one stack per spec
    LIST_LEVEL levels[LIST_MAX_ARG_SPECS][LIST_DEPTH_MAX];
    size_t depth[LIST_MAX_ARG_SPECS];
} LIST_SESSION;

// one list of a session as a UI_WINDOWING_SOURCE: shown means the row passes
// filter
typedef struct _list_rows{
    UI_LAYER *ui_layer;
    ContextId context;
    DataListId list;
    const DataActionReq *partial;
    uint64_t branch; // level of the list shown, 0 = top
    const LIST_FILTER *filter;
} LIST_ROWS;

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
        if (!cand->entry.action.enabled)
            continue;
        const char *label = cand->entry.action.label;
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
        if (!ui_layer_context_valid(ui_layer, candidates[i].entry.context))
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
        if(cur_canditate.entry.context == context){
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
static size_t helper_list_count(const LIST_ROWS *rows) {
    size_t count = ui_layer_context_list_count(rows->ui_layer, rows->context,
                                               rows->list, rows->partial,
                                               rows->branch);
    if (count > 0)
        return count;
    DataChoice row;
    while (ui_layer_context_list_at(rows->ui_layer, rows->context, rows->list,
                                    rows->partial, rows->branch, count, &row))
        count++;
    return count;
}

static bool helper_list_row_shown(void *user, size_t idx) {
    const LIST_ROWS *rows = user;
    DataChoice row;
    if (!ui_layer_context_list_at(rows->ui_layer, rows->context, rows->list,
                                  rows->partial, rows->branch, idx, &row))
        return false;
    if (rows->filter->group != 0 && row.group_key != rows->filter->group)
        return false;
    if (rows->filter->linked_only && !(row.flags & DATA_CHOICE_LINKED))
        return false;
    return true;
}

// an unfiltered list skips the predicate - every row list_at returns is shown
static UI_WINDOWING_SOURCE helper_list_source(LIST_ROWS *rows) {
    UI_WINDOWING_SOURCE src = {helper_list_count(rows), NULL, rows};
    if (rows->filter->group != 0 || rows->filter->linked_only)
        src.shown = helper_list_row_shown;
    return src;
}

static uint64_t helper_list_value_at(const LIST_ROWS *rows, size_t idx) {
    DataChoice row;
    if (!ui_layer_context_list_at(rows->ui_layer, rows->context, rows->list,
                                  rows->partial, rows->branch, idx, &row))
        return 0;
    return row.value;
}

// index of the row holding value, trying hint first. SIZE_MAX when no row does
static size_t helper_list_find(const LIST_ROWS *rows, uint64_t value,
                               size_t hint) {
    if (value == 0)
        return SIZE_MAX;
    if (helper_list_value_at(rows, hint) == value)
        return hint;
    DataChoice row;
    for (size_t i = 0;
         ui_layer_context_list_at(rows->ui_layer, rows->context, rows->list,
                                  rows->partial, rows->branch, i, &row);
         i++) {
        if (row.value == value)
            return i;
    }
    return SIZE_MAX;
}

// idx must be shown
static void helper_list_cursor_settle(const LIST_ROWS *rows,
                                      const UI_WINDOWING_SOURCE *src,
                                      LIST_CURSOR *cur, size_t idx) {
    size_t near;
    bool more;
    cur->idx = idx;
    cur->value = helper_list_value_at(rows, idx);
    cur->prev = ui_windowing_collect_shown(src, idx, false, 1, &near, &more)
                    ? helper_list_value_at(rows, near)
                    : 0;
    cur->next = ui_windowing_collect_shown(src, idx, true, 1, &near, &more)
                    ? helper_list_value_at(rows, near)
                    : 0;
}

// put the cursor back on the row it was on, wherever the list has moved it.
// A row the filter now hides passes the cursor to the next shown row after it.
// A row that is gone passes it to the row before where it was, as
// UI_STALE_PREV_SIBLING does. With nothing shown the cursor is left as it is
static void helper_list_cursor_anchor(LIST_ROWS *rows, LIST_CURSOR *cur) {
    UI_WINDOWING_SOURCE src = helper_list_source(rows);
    size_t idx;
    size_t found = helper_list_find(rows, cur->value, cur->idx);
    if (cur->value == 0) {
        idx = ui_windowing_nearest_shown(&src, 0);
    } else if (found < src.count) {
        idx = ui_windowing_nearest_shown(&src, found);
    } else {
        // where the gone row would sit now, going by the rows it had around it
        size_t prev = helper_list_find(rows, cur->prev, cur->idx - 1);
        size_t next = helper_list_find(rows, cur->next, cur->idx);
        size_t pos = cur->idx < src.count ? cur->idx : src.count;
        if (prev < src.count)
            pos = prev + 1;
        else if (next < src.count)
            pos = next;
        idx = ui_windowing_last_shown_before(&src, pos);
    }
    if (idx < src.count)
        helper_list_cursor_settle(rows, &src, cur, idx);
}

static void helper_list_cursor_move(LIST_ROWS *rows, LIST_CURSOR *cur,
                                    bool next) {
    UI_WINDOWING_SOURCE src = helper_list_source(rows);
    size_t idx = ui_windowing_step(&src, cur->idx, 1, next, true);
    if (idx < src.count)
        helper_list_cursor_settle(rows, &src, cur, idx);
}

// the label of the first row in `group`, NULL when no row is in it
static const char *helper_list_group_label(const LIST_ROWS *rows,
                                           uint64_t group) {
    DataChoice row;
    for (size_t i = 0; ui_layer_context_list_at(rows->ui_layer, rows->context,
                                                rows->list, rows->partial,
                                                rows->branch, i, &row);
         i++) {
        if (row.group_key == group)
            return row.group_label ? row.group_label : "?";
    }
    return NULL;
}

static bool helper_list_group_starts_at(const LIST_ROWS *rows, uint64_t group,
                                        size_t idx) {
    DataChoice row;
    for (size_t i = 0; i < idx && ui_layer_context_list_at(
                                      rows->ui_layer, rows->context, rows->list,
                                      rows->partial, rows->branch, i, &row);
         i++) {
        if (row.group_key == group)
            return false;
    }
    return true;
}

// the group after `current` in the order the groups first appear; 0 (every
// group) after the last one, and when current is not in the list at all
static uint64_t helper_list_group_next(const LIST_ROWS *rows,
                                       uint64_t current) {
    bool passed = current == 0;
    DataChoice row;
    for (size_t i = 0; ui_layer_context_list_at(rows->ui_layer, rows->context,
                                                rows->list, rows->partial,
                                                rows->branch, i, &row);
         i++) {
        uint64_t group = row.group_key;
        if (group == 0 || !helper_list_group_starts_at(rows, group, i))
            continue;
        if (passed)
            return group;
        if (group == current)
            passed = true;
    }
    return 0;
}

// how many groups the list has, counting no further than limit
static size_t helper_list_group_count(const LIST_ROWS *rows, size_t limit) {
    size_t count = 0;
    DataChoice row;
    for (size_t i = 0; count < limit &&
                       ui_layer_context_list_at(rows->ui_layer, rows->context,
                                                rows->list, rows->partial,
                                                rows->branch, i, &row);
         i++) {
        if (row.group_key != 0 &&
            helper_list_group_starts_at(rows, row.group_key, i))
            count++;
    }
    return count;
}

// the group of the nearest context, from `from` up through its parents, that
// declares one appearing in the list; 0 when none does
static uint64_t helper_list_scope(const LIST_ROWS *rows, ContextId from) {
    for (ContextId cx = from; ui_layer_context_valid(rows->ui_layer, cx);
         cx = ui_layer_context_parent_return(rows->ui_layer, cx)) {
        uint64_t group = ui_layer_context_group_key(rows->ui_layer, cx);
        if (group != 0 && helper_list_group_label(rows, group))
            return group;
    }
    return 0;
}

// the list's name, then whatever narrows it: "Connect to (system, connected)"
static void helper_list_title(const LIST_ROWS *rows, const char *name,
                              char *out, size_t cap) {
    const char *group = rows->filter->group
                            ? helper_list_group_label(rows, rows->filter->group)
                            : NULL;
    bool linked = rows->filter->linked_only;
    if (!group && !linked) {
        snprintf(out, cap, "%s", name);
        return;
    }
    snprintf(out, cap, "%s (%s%s%s)", name, group ? group : "",
             group && linked ? ", " : "", linked ? "connected" : "");
}

static void helper_list_render(LIST_ROWS *rows, const char *header,
                               const char *status, size_t cursor_idx) {
    printf("-- %s --\n\n", header);

    UI_WINDOWING_SOURCE source = helper_list_source(rows);
    size_t slice_items[VIEW_ROWS];
    UI_WINDOWING_SLICE slice = {.items = slice_items, .capacity = VIEW_ROWS};
    ui_windowing_fill_around_cursor(&source, cursor_idx, &slice);
    if (slice.count == 0)
        printf("(no options)\n");

    if (slice.more_before)
        printf("-...-\n");
    DataChoice row;
    for (size_t r = 0; r < slice.count; r++) {
        if (!ui_layer_context_list_at(rows->ui_layer, rows->context, rows->list,
                                      rows->partial, rows->branch, slice.items[r], &row))
            continue;
        // the group tells same-named rows apart until one group is shown
        bool grouped = rows->filter->group == 0 && row.group_key != 0;
        printf("%s%s%s%s%s%s\n", r == slice.cursor_slot ? ">" : "",
               grouped ? (row.group_label ? row.group_label : "?") : "",
               grouped ? ": " : "", row.label ? row.label : "?",
               (row.flags & DATA_CHOICE_BRANCH) ? "/" : "",
               (row.flags & DATA_CHOICE_LINKED) ? " [connected]" : "");
    }
    if (slice.more_after)
        printf("-...-\n");

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

static const char *helper_action_label(const DataAction *action) {
    return action->label ? action->label : "action";
}

// read the line typed for spec in cooked mode, without its newline. The whole
// line is always read, so none of it is left over as keys. false with msg set
// on EOF or when the line does not fit in line
static bool helper_prompt_line(const DataArgSpec *spec, const char *label, char *line,
                               size_t line_cap, char *msg, size_t msg_cap) {
    enterCookedMode();
    printf("%s: ", spec->label ? spec->label : spec->name);
    fflush(stdout);
    bool got_line = fgets(line, (int)line_cap, stdin) != NULL;
    size_t len = got_line ? strlen(line) : 0;
    bool whole = len > 0 && line[len - 1] == '\n';
    // a full buffer without the newline: drain the rest. Nothing left means
    // the line fit exactly, or was the last one before EOF
    bool too_long = false;
    if (got_line && !whole) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            too_long = true;
    }
    // a Ctrl-D ends the prompt, not the program. Input that really ended
    // reads EOF again in the main loop
    clearerr(stdin);
    enterRawModeAgain();
    if (!got_line) {
        snprintf(msg, msg_cap, "%s: cancelled.", label);
        return false;
    }
    if (too_long) {
        snprintf(msg, msg_cap, "%s: too long.", label);
        return false;
    }
    if (whole)
        line[len - 1] = '\0';
    return true;
}

// read one number typed for spec, spaces around it allowed. as_int: it has to
// fit an int. false with msg set when cancelled (also a blank line) or the
// line is not one number
static bool helper_prompt_number(const DataArgSpec *spec, const char *label, bool as_int,
                                 double *out, char *msg, size_t msg_cap) {
    char line[128];
    if (!helper_prompt_line(spec, label, line, sizeof(line), msg, msg_cap))
        return false;
    const char *start = line;
    while (isspace((unsigned char)*start))
        start++;
    if (*start == '\0') {
        snprintf(msg, msg_cap, "%s: cancelled.", label);
        return false;
    }
    char *end;
    bool in_range;
    errno = 0;
    if (as_int) {
        long value = strtol(start, &end, 10);
        in_range = errno != ERANGE && value >= INT_MIN && value <= INT_MAX;
        *out = (double)value;
    } else {
        *out = strtod(start, &end);
        in_range = !(errno == ERANGE && isinf(*out));
    }
    while (isspace((unsigned char)*end))
        end++;
    if (end == start || *end != '\0' || !in_range) {
        snprintf(msg, msg_cap, "%s: not a number.", label);
        return false;
    }
    return true;
}

// prompt for a step count, then adjust the value (of a parameter for example)
static void helper_action_do_adjust_value(UI_LAYER *ui_layer, const UI_NAV_ACTION *chosen,
                                          const DataArgSpec *spec, const char *label,
                                          char *msg, size_t msg_cap) {
    double steps;
    if (!helper_prompt_number(spec, label, true, &steps, msg, msg_cap))
        return;
    helper_action_result_msg(ui_nav_action_run_adjust(ui_layer, chosen, (int)steps, NULL),
                             label, msg, msg_cap);
}
// adjust value (of a parameter for example), directly. Not through action
// drilling. msg stays as it was when context has no adjust action
static void helper_action_do_adjust_value_direct(UI_LAYER *ui_layer, ContextId context,
                                                 int adjust_value, char *msg,
                                                 size_t msg_cap) {
    UI_NAV_ACTION adjust;
    if (!ui_nav_context_action_find(ui_layer, context, DATA_ACTION_ADJUST_VALUE, &adjust))
        return;
    helper_action_result_msg(ui_nav_action_run_adjust(ui_layer, &adjust, adjust_value, NULL),
                             helper_action_label(&adjust.action), msg, msg_cap);
}

// set value (of a parameter for example) using user intput
static void helper_action_do_set_value(UI_LAYER *ui_layer, const UI_NAV_ACTION *chosen,
                                       const DataArgSpec *spec, const char *label,
                                       char *msg, size_t msg_cap) {
    double value;
    if (!helper_prompt_number(spec, label, false, &value, msg, msg_cap))
        return;
    helper_action_result_msg(ui_nav_action_run_set_value(ui_layer, chosen, value, NULL),
                             label, msg, msg_cap);
}

// single PATH (ADD_FILE_PATH) or STRING (SAVE) arg: cooked-mode text entry.
static void helper_action_do_text(UI_LAYER *ui_layer, const UI_NAV_ACTION *chosen,
                                  const DataArgSpec *spec, const char *label, char *msg,
                                  size_t msg_cap) {
    char text_buf[512];
    if (!helper_prompt_line(spec, label, text_buf, sizeof(text_buf), msg, msg_cap))
        return;
    helper_action_result_msg(ui_nav_action_run_text(ui_layer, chosen, text_buf, NULL),
                             label, msg, msg_cap);
}

static void helper_list_session_start(LIST_SESSION *session, int kind,
                                      ContextId context,
                                      const DataAction *action,
                                      const char *label,
                                      const DataArgSpec *specs,
                                      size_t spec_count) {
    *session = (LIST_SESSION){0};
    session->kind = kind;
    session->context = context;
    session->type = action->type;
    session->id = action->id;
    session->label = label;
    for (size_t i = 0; i < spec_count && i < LIST_MAX_ARG_SPECS; i++)
        session->specs[i] = specs[i];
    session->connect_pick = CONNECT_PICK_SOURCE;
}

// a session opens on its first list narrowed to the context the user is at,
// when that context declares a group the list has
static void helper_list_session_scope(UI_LAYER *ui_layer, LIST_SESSION *session,
                                      ContextId from) {
    DataActionReq partial = {.type = session->type, .id = session->id};
    LIST_FILTER none = {0};
    LIST_ROWS rows = {ui_layer, session->context, session->specs[0].list,
                      &partial, 0, &none};
    session->filters[0].group = helper_list_scope(&rows, from);
}

// the branch a spec's list shows, 0 = its top level
static uint64_t helper_list_branch(const LIST_SESSION *session, size_t which) {
    size_t depth = session->depth[which];
    return depth ? session->levels[which][depth - 1].branch : 0;
}

// open a category row of a spec's list. false when already LIST_DEPTH_MAX deep
static bool helper_list_open(LIST_SESSION *session, size_t which,
                             const DataChoice *row) {
    if (session->depth[which] == LIST_DEPTH_MAX)
        return false;
    LIST_LEVEL *level = &session->levels[which][session->depth[which]++];
    level->branch = row->value;
    level->parent_cursor = session->cursors[which];
    snprintf(level->label, sizeof(level->label), "%s",
             row->label ? row->label : "?");
    session->cursors[which] = (LIST_CURSOR){0};
    return true;
}

// close the innermost opened category, the cursor goes back onto its row.
// false at the top level
static bool helper_list_close(LIST_SESSION *session, size_t which) {
    if (session->depth[which] == 0)
        return false;
    session->depth[which]--;
    session->cursors[which] =
        session->levels[which][session->depth[which]].parent_cursor;
    return true;
}

// the list's name, then every category opened in it: "Preset / Bank"
static void helper_list_path(const LIST_SESSION *session, size_t which,
                             char *out, size_t cap) {
    const DataArgSpec *spec = &session->specs[which];
    size_t len = (size_t)snprintf(out, cap, "%s",
                                  spec->label ? spec->label : spec->name);
    for (size_t i = 0; i < session->depth[which] && len < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, " / %s",
                                session->levels[which][i].label);
}

// let each view react to the context events since the last call. Returns true
// when a view was touched and has to be drawn again
static bool helper_views_reconcile(const VIEWS_SYNC *sync) {
    bool redraw = false;
    for (size_t si = 0; si < sync->states_count; si++) {
        UiReconcileResult rr =
            ui_layer_state_reconcile(sync->ui_layer, sync->states[si]);
        if (rr != UI_RECONCILE_OK)
            redraw = true;
    }
    return redraw;
}

// wait for a key, pulling fresh data meanwhile - woken by sync->wake_fd when
// it is available, else by polling every INPUT_WAIT_TICK_MS.
// Returns the key, EOF once stdin has ended, or INPUT_WAIT_REDRAW as soon as a
// view shows a change
static int helper_input_wait(const VIEWS_SYNC *sync) {
    struct pollfd fds[2] = {{.fd = STDIN_FILENO, .events = POLLIN},
                            {.fd = sync->wake_fd, .events = POLLIN}};
    int nfds = sync->wake_fd >= 0 ? 2 : 1;
    // the wake fd's own armed timer covers the periodic tick now - no need
    // for poll() to also time out
    int timeout = sync->wake_fd >= 0 ? -1 : INPUT_WAIT_TICK_MS;
    while (1) {
        int ready = poll(fds, nfds, timeout);
        // a pipe whose writer is gone reports POLLHUP without POLLIN -
        // getchar then returns EOF instead of blocking
        if (ready > 0 &&
            (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            return getchar();
        // poll itself failed - fall back to simply blocking for the key
        if (ready < 0 && errno != EINTR)
            return getchar();
        ui_layer_update_cycle(sync->ui_layer);
        if (helper_views_reconcile(sync))
            return INPUT_WAIT_REDRAW;
    }
}

// one frame of a list session: re-anchor, draw, read a key, act. Returns false
// once the session is over, with its closing message in msg
static bool helper_list_session_frame(UI_LAYER *ui_layer, LIST_SESSION *session,
                                      const VIEWS_SYNC *sync, char *msg,
                                      size_t msg_cap) {
    if (!ui_layer_context_valid(ui_layer, session->context)) {
        snprintf(msg, msg_cap, "%s: no longer available.", session->label);
        return false;
    }

    bool picking_targets = session->kind == LIST_SESSION_CONNECT &&
                           session->connect_pick == CONNECT_PICK_TARGETS;
    size_t which = picking_targets ? 1 : 0;
    const DataArgSpec *spec = &session->specs[which];
    LIST_CURSOR *cur = &session->cursors[which];
    LIST_FILTER *filter = &session->filters[which];
    DataActionReq partial = {.type = session->type, .id = session->id};
    if (picking_targets)
        partial.connect.source = session->source_value;
    LIST_ROWS rows = {ui_layer, session->context, spec->list, &partial,
                      helper_list_branch(session, which), filter};

    // a group whose rows all went away cannot be narrowed to any more
    if (filter->group != 0 && !helper_list_group_label(&rows, filter->group))
        filter->group = 0;

    helper_list_cursor_anchor(&rows, cur);

    char path[128];
    helper_list_path(session, which, path, sizeof(path));
    char title[192];
    helper_list_title(&rows, path, title, sizeof(title));
    char header[256];
    snprintf(header, sizeof(header),
             "%s (j/k move, l select, g group%s, h/ESC back)", title,
             picking_targets ? ", c connected" : "");
    helper_list_render(&rows, header, msg, cur->idx);

    int input = helper_input_wait(sync);
    // the data changed - draw the list again, the message stays
    if (input == INPUT_WAIT_REDRAW)
        return true;
    // a result is shown for exactly one frame
    msg[0] = '\0';
    // stdin ended - close the session, the main loop reads EOF again and quits
    if (input == EOF)
        return false;

    if (input == '\e' || input == 'h') {
        if (helper_list_close(session, which))
            return true;
        if (picking_targets) {
            session->connect_pick = CONNECT_PICK_SOURCE;
            return true;
        }
        snprintf(msg, msg_cap, "%s: cancelled.", session->label);
        return false;
    }
    if (input == 'j' || input == 'k') {
        helper_list_cursor_move(&rows, cur, input == 'j');
        return true;
    }
    if (input == 'g') {
        if (helper_list_group_count(&rows, 2) >= 2)
            filter->group = helper_list_group_next(&rows, filter->group);
        return true;
    }
    if (input == 'c' && picking_targets) {
        filter->linked_only = !filter->linked_only;
        return true;
    }
    if (input != 'l')
        return true;

    // with nothing shown the cursor rests on a row the user cannot see
    UI_WINDOWING_SOURCE source = helper_list_source(&rows);
    if (!ui_windowing_is_shown(&source, cur->idx))
        return true;
    DataChoice picked;
    if (!ui_layer_context_list_at(ui_layer, session->context, spec->list,
                                  &partial, rows.branch, cur->idx, &picked))
        return true;
    // a category row is opened, it is never an action value
    if (picked.flags & DATA_CHOICE_BRANCH) {
        if (!helper_list_open(session, which, &picked))
            snprintf(msg, msg_cap, "%s: categories nested too deep.",
                     session->label);
        return true;
    }

    if (session->kind == LIST_SESSION_CONNECT && !picking_targets) {
        session->source_value = picked.value;
        session->connect_pick = CONNECT_PICK_TARGETS;
        // a new source has its own target list
        session->cursors[1] = (LIST_CURSOR){0};
        session->depth[1] = 0;
        return true;
    }

    DataActionReq req = {.type = session->type, .id = session->id};
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
// main loop drives it from then on. scope_from is where the user is - a list
// opens narrowed to it when it can be
static bool helper_action_resolve(UI_LAYER *ui_layer, const UI_NAV_ACTION *chosen,
                                  ContextId scope_from, LIST_SESSION *session,
                                  char *msg, size_t msg_cap) {
    if (!msg || msg_cap == 0)
        return false;
    msg[0] = '\0';
    if (!ui_layer || !chosen || !session)
        return false;

    ContextId context = chosen->context;
    const DataAction *action = &chosen->action;
    const char *label = helper_action_label(action);

    DataArgSpec specs[UI_NAV_ACTION_ARGS_MAX];
    switch (ui_nav_action_input(ui_layer, chosen, specs)) {
    case UI_NAV_INPUT_NONE:
        helper_action_result_msg(ui_nav_action_run(ui_layer, chosen, NULL), label,
                                 msg, msg_cap);
        return false;
    case UI_NAV_INPUT_STEPS:
        helper_action_do_adjust_value(ui_layer, chosen, &specs[0], label, msg,
                                      msg_cap);
        return false;
    case UI_NAV_INPUT_NUMBER:
        helper_action_do_set_value(ui_layer, chosen, &specs[0], label, msg,
                                   msg_cap);
        return false;
    case UI_NAV_INPUT_TEXT:
        printf("\n-- %s --\n", label);
        helper_action_do_text(ui_layer, chosen, &specs[0], label, msg, msg_cap);
        return false;
    case UI_NAV_INPUT_LIST_CHOICE:
        helper_list_session_start(session, LIST_SESSION_CHOICE, context,
                                  action, label, specs, 1);
        helper_list_session_scope(ui_layer, session, scope_from);
        return true;
    case UI_NAV_INPUT_LIST_CONNECT:
        helper_list_session_start(session, LIST_SESSION_CONNECT, context,
                                  action, label, specs, 2);
        helper_list_session_scope(ui_layer, session, scope_from);
        return true;
    case UI_NAV_INPUT_MISCONFIGURED:
        snprintf(msg, msg_cap, "%s: misconfigured.", label);
        return false;
    case UI_NAV_INPUT_UNSUPPORTED:
        break;
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
    // unbuffered, so a key poll() reports is not already sitting in stdio's
    // buffer - and a key in that buffer is never missed by poll()
    setvbuf(stdin, NULL, _IONBF, 0);
    UI_LAYER* ui_layer = ui_layer_init();

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
    // which idx in the state_main_context_current children array is the UI_PURPOSE_HOVERED
    // this will be set to an actual value later - idx 0 can be hidden.
    size_t state_main_hovered_idx = 0;

    // this array will contain all of the states
    UI_STATE* states_all[STATES_COUNT] = {state_main, state_root};

    // state_main's current context, saved under id_root + UI_PURPOSE_CURRENT
    UI_TARGET_LIST* current_list = NULL;
    if (ui_layer_state_entry_set(state_main, id_root, UI_PURPOSE_CURRENT, 1))
        current_list = ui_layer_nav_target_list_begin(state_main, id_root,
                                                      UI_PURPOSE_CURRENT);
    if (!current_list) {
        printf("\nCould not create the current context entry\n");
        helper_program_destroy(ui_layer, states_all, STATES_COUNT);
        return 1;
    }
    // removals arrive children first, so a removed current climbs to its
    // closest surviving ancestor
    ui_layer_target_list_set_stale_policy(
        current_list, (UiStalePolicy){.mode = UI_STALE_TO_SOURCE});
    ui_layer_nav_target_list_add(current_list, id_root);
    ui_layer_nav_target_list_end(state_main);

    // initial navigation mode
    size_t ui_nav_mode = UI_MODE_STANDARD;
    // action candidates for the current drill round. Only meaningful while
    // ui_nav_mode == UI_MODE_ACTION; UI_MODE_STANDARD rebuilds this fresh
    ACTION_CANDIDATE candidates[MAX_ACTION_CANDIDATES] = {0};
    size_t candidate_count = 0;
    // result of the last resolved action, shown once at the top of the frame
    char action_status_msg[128] = {0};
    // the list being browsed while ui_nav_mode == UI_MODE_LIST
    LIST_SESSION list_session = {0};
    // what the input wait keeps up to date while no key is pressed
    const VIEWS_SYNC views_sync = {ui_layer, states_all, STATES_COUNT,
                                   ui_layer_wake_fd(ui_layer, INPUT_WAIT_TICK_MS)};

    while (1) {
        // erase the terminal
        printf("\033[2J\033[H");

        // update the ui_layer - it updates the data underneath
        ui_layer_update_cycle(ui_layer);

        // let each view react to contexts that appeared / were removed
        helper_views_reconcile(&views_sync);

        ContextId state_main_context_current =
            ui_nav_purpose_context(state_main, id_root, UI_PURPOSE_CURRENT);
        // if the state_main_context_current does not have any children
        // the state_main_context_current will be its parent
        if(ui_layer_context_children_count(ui_layer, state_main_context_current) == 0){
            ContextId curr_parent = ui_layer_context_parent_return(ui_layer, state_main_context_current);
            // if the state_main_context_current is not valid or does not have a
            // parent it will jump to the root
            if (!ui_layer_context_valid(ui_layer, curr_parent)) {
                curr_parent = id_root;
            }
            state_main_context_current = curr_parent;
            ui_nav_purpose_context_set(ui_layer, state_main, id_root, UI_PURPOSE_CURRENT, state_main_context_current);
        }

        state_main_hovered_idx = ui_nav_purpose_context_ensure_visible(
            ui_layer, state_main, state_main_context_current, UI_PURPOSE_HOVERED,
            (UiStalePolicy){.mode = UI_STALE_PREV_SIBLING});

        printf("\e[22m");
        // a list session owns the whole frame; it runs after the update cycle
        // above, so it always draws the list as it is now
        if (ui_nav_mode == UI_MODE_LIST) {
            if (!helper_list_session_frame(ui_layer, &list_session,
                                           &views_sync, action_status_msg,
                                           sizeof(action_status_msg)))
                ui_nav_mode = UI_MODE_STANDARD;
            continue;
        }
        if(ui_nav_mode == UI_MODE_ACTION)
            printf("\e[2m");

        UI_NAV_CONTEXT_INFO state_main_current_info;
        if (ui_nav_context_info_get(ui_layer, state_main_context_current,
                                    &state_main_current_info)) {
            printf("----| %s |----\n\n", state_main_current_info.name);

            // the window of children that fits on screen around the hovered
            // one. The builder owns the hidden/limit geometry, so all that is
            // left here is drawing rows
            UI_NAV_CHILDREN children;
            UI_WINDOWING_SOURCE source = ui_nav_children_source(
                ui_layer, state_main_context_current, &children);
            size_t slice_items[VIEW_ROWS];
            UI_WINDOWING_SLICE slice = {.items = slice_items, .capacity = VIEW_ROWS};
            ui_windowing_fill_around_cursor(&source, state_main_hovered_idx, &slice);
            // store all the row ContextIds so the ui_layer_state_reconcile
            // checks these for any changes, not only the
            // state_main_context_current, root and others
            ui_nav_shown_store(ui_layer, state_main, id_root, UI_PURPOSE_SHOWN,
                               state_main_context_current, &slice);

            if (slice.more_before)
                printf("-...-\n");
            for (size_t row = 0; row < slice.count; row++) {
                ContextId child = ui_layer_context_child_at(
                    ui_layer, state_main_context_current, slice.items[row]);
                UI_NAV_CONTEXT_INFO child_info;
                if (!ui_nav_context_info_get(ui_layer, child, &child_info))
                    continue;
                if (row == slice.cursor_slot)
                    printf(">%s", child_info.name);
                else
                    printf("%s", child_info.name);
                if (child_info.latency > 0)
                    printf(" (latency %u)", child_info.latency);
                child_info.value ? printf(" --- %s\n", child_info.value)
                                 : printf("\n");
            }
            if (slice.more_after)
                printf("-...-\n");
        }

        // --------------------------------------------------
        // Actions for the state_main

        // action candidates: either keep drilling the previous round
        // (revalidating first, in case something acted on disappeared
        // between rounds) or, in standard mode, rebuild round 0 fresh
        ContextId state_current_context_hovered = ui_nav_purpose_context(
            state_main, state_main_context_current, UI_PURPOSE_HOVERED);
        // collect the ContextIds that can have actions - from hovered up to root
        ContextId action_sources[ACTION_SOURCES_COUNT] = {0};
        size_t action_idx_counted = 0;
        // find the ContextIds from the current context, travelling up until the root ContextId
        ContextId action_cx_curr = state_main_context_current;
        for(size_t action_idx = 0; action_idx < ACTION_SOURCES_COUNT; action_idx++){
            // always add the hovered id
            if(action_idx == 0){
                action_sources[action_idx] = state_current_context_hovered;
                action_idx_counted += 1;
                continue;
            }
            action_sources[action_idx] = action_cx_curr;
            action_idx_counted += 1;
            ContextId action_cx_parent = ui_layer_context_parent_return(ui_layer, action_cx_curr);
            if(!ui_layer_context_valid(ui_layer, action_cx_parent))
                break;
            action_cx_curr = action_cx_parent;
        }
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
            UI_NAV_ACTION gathered[MAX_ACTION_CANDIDATES];
            candidate_count = 0;
            for (size_t i = 0; i < action_idx_counted; i++)
                candidate_count += ui_nav_actions_gather(
                    ui_layer, action_sources[i], gathered + candidate_count,
                    MAX_ACTION_CANDIDATES - candidate_count);
            for (size_t i = 0; i < candidate_count; i++)
                candidates[i] = (ACTION_CANDIDATE){.entry = gathered[i]};
            helper_action_candidates_assign_letters(candidates, candidate_count,
                                                    true);
        }
        // show(print) possible actions for the current drill round
        printf("\n-------------------------------------------------------------"
               "---------------------------------------");

        for (size_t s = 0; s < action_idx_counted; s++) {
            ContextId src = action_sources[s];
            UI_NAV_CONTEXT_INFO src_info;
            if (!ui_nav_context_info_get(ui_layer, src, &src_info))
                continue;
            if (!helper_action_canditates_has_source(candidates, candidate_count,
                                                    src))
                continue;
            printf("\n");
            printf("%s: ", src_info.name);
            for (size_t i = 0; i < candidate_count; i++) {
                if (candidates[i].entry.context != src)
                    continue;
                if (ui_nav_mode == UI_MODE_ACTION)
                    printf("\e[22m");
                printf("-");
                helper_string_print_underline(candidates[i].entry.action.label,
                                              candidates[i].letter);
                printf("-");
                if (candidates[i].entry.action.active)
                    printf(" (on)");
                if (!candidates[i].entry.action.enabled)
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
        UI_NAV_CONTEXT_INFO cx_root_info;
        if(ui_nav_context_info_get(ui_layer, id_root, &cx_root_info)){
            UI_NAV_CHILDREN chldr;
            UI_WINDOWING_SOURCE src = ui_nav_children_source(ui_layer, id_root, &chldr);
            for(size_t i = 0; i < cx_root_info.child_count; i++){
                // don't show hidden children
                if(!ui_windowing_is_shown(&src, i))
                    continue;
                ContextId root_child =
                    ui_layer_context_child_at(ui_layer, id_root, i);
                UI_NAV_CONTEXT_INFO cx_root_child_info;
                if (ui_nav_context_info_get(ui_layer, root_child,
                                            &cx_root_child_info)) {
                    printf("| %lu_%s |", i, cx_root_child_info.name);
                }
            }
            printf("\n");
        }

        // system messages
        printf("\nNavigate with h,j,k,l. Change param values with -/+\n");
        printf("\nMessages:\n");
        if (action_status_msg[0])
            printf("%s\n", action_status_msg);

        // get user inputs - or draw again when the data changed what is shown
        int input = helper_input_wait(&views_sync);
        if (input == INPUT_WAIT_REDRAW)
            continue;
        // stdin ended (closed pipe, end of a redirected file) - quit like q
        if (input == EOF)
            break;
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
                ContextId scope_from =
                    ui_layer_context_valid(ui_layer,
                                           state_current_context_hovered)
                        ? state_current_context_hovered
                        : state_main_context_current;
                bool listing = helper_action_resolve(
                    ui_layer, &candidates[0].entry, scope_from, &list_session,
                    action_status_msg, sizeof(action_status_msg));
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
                    // change the parameter value directly with - and +, without
                    // action drilling
                case '-':
                    helper_action_do_adjust_value_direct(
                        ui_layer, state_current_context_hovered, -1,
                        action_status_msg, sizeof(action_status_msg));
                    break;
                case '+':
                    helper_action_do_adjust_value_direct(
                        ui_layer, state_current_context_hovered, 1,
                        action_status_msg, sizeof(action_status_msg));
                    break;

                case 'j':
                    state_main_hovered_idx = ui_nav_purpose_step(
                        ui_layer, state_main, state_main_context_current,
                        UI_PURPOSE_HOVERED, state_main_hovered_idx, 1, true, true);
                    break;
                case 'k':
                    state_main_hovered_idx = ui_nav_purpose_step(
                        ui_layer, state_main, state_main_context_current,
                        UI_PURPOSE_HOVERED, state_main_hovered_idx, 1, false, true);
                    break;
                case 'l':
                    ui_nav_context_enter(ui_layer, state_main, id_root,
                                         UI_PURPOSE_CURRENT, UI_PURPOSE_HOVERED);
                    break;
                case 'h':
                    ui_nav_context_exit(ui_layer, state_main, id_root,
                                        UI_PURPOSE_CURRENT);
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

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui_layer.h"
#include "ui_windowing.h"

// Reading and navigating contexts through ui_layer. Returns codes and data,
// never display text.
// Optional helper for ease of use: a UI can ignore it and call ui_layer directly.

typedef struct _ui_nav_context_info{
    const char *name; // borrowed
    size_t child_count;
    bool is_hidden; // hidden by the data layer
    const char *value; // borrowed, NULL when the context has no value
    uint32_t latency; // frames the context adds, 0 for none
} UI_NAV_CONTEXT_INFO;

// a context's children as a UI_WINDOWING_SOURCE: shown means visible
typedef struct _ui_nav_children{
    UI_LAYER *ui_layer;
    ContextId parent;
} UI_NAV_CHILDREN;

// false when context is not valid or has no name
bool ui_nav_context_info_get(UI_LAYER *ui_layer, ContextId context, UI_NAV_CONTEXT_INFO *info);

// a child is visible when ui_nav_context_info_get reads it and it is not
// hidden, so visible means "will be drawn". Reads only validity, name and
// hidden. out_child may be NULL
bool ui_nav_child_visible_at(UI_LAYER *ui_layer, ContextId parent, size_t idx,
                             ContextId *out_child);

// parent's children as a source. children is filled here and must outlive the
// returned source
UI_WINDOWING_SOURCE ui_nav_children_source(UI_LAYER *ui_layer, ContextId parent,
                                           UI_NAV_CHILDREN *children);

// A purpose is the (source, purpose) entry of a UI_STATE holding one context,
// e.g. the hovered child under its parent. Purpose values are the UI's own.

// CONTEXT_ID_INVALID when the entry is missing or empty
ContextId ui_nav_purpose_context(UI_STATE *state, ContextId source, UiPurpose purpose);

// make context the one saved for (source, purpose). false when the entry is
// missing or context is not valid
bool ui_nav_purpose_context_set(UI_LAYER *ui_layer, UI_STATE *state, ContextId source,
                                UiPurpose purpose, ContextId context);

// keep (parent, purpose) on a visible child of parent: the saved one while it
// is visible, else the nearest visible one. A missing entry is created with
// stale_policy for its context. Returns the child's index, parent's child count
// when there is no visible child
size_t ui_nav_purpose_context_ensure_visible(UI_LAYER *ui_layer, UI_STATE *state,
                                             ContextId parent, UiPurpose purpose,
                                             UiStalePolicy stale_policy);

// move (parent, purpose) steps visible children away from from_idx, as
// ui_windowing_step. Returns the child index it moved to, from_idx when it did
// not move
size_t ui_nav_purpose_step(UI_LAYER *ui_layer, UI_STATE *state, ContextId parent,
                           UiPurpose purpose, size_t from_idx, size_t steps, bool forward,
                           bool wrap);

// the current context is the one saved for (current_source, current_purpose).
// Enter makes its hovered_purpose child current when that child has children.
// Both return false when the current context did not change
bool ui_nav_context_enter(UI_LAYER *ui_layer, UI_STATE *state, ContextId current_source,
                          UiPurpose current_purpose, UiPurpose hovered_purpose);
// make the current context's parent current
bool ui_nav_context_exit(UI_LAYER *ui_layer, UI_STATE *state, ContextId current_source,
                         UiPurpose current_purpose);

// save the children of parent that slice holds under (source, purpose), so
// ui_layer_state_reconcile reports a change to any of them. Creates the entry,
// or resizes it when slice->capacity changed
bool ui_nav_shown_store(UI_LAYER *ui_layer, UI_STATE *state, ContextId source,
                        UiPurpose purpose, ContextId parent,
                        const UI_WINDOWING_SLICE *slice);

// most actions ui_nav_actions_gather reads from one context
#define UI_NAV_CONTEXT_ACTIONS_MAX 8

// an action and the context it acts on
typedef struct _ui_nav_action{
    ContextId context;
    DataAction action;
} UI_NAV_ACTION;

// fill out with up to capacity of context's actions. Returns how many it wrote
size_t ui_nav_actions_gather(UI_LAYER *ui_layer, ContextId context, UI_NAV_ACTION *out,
                             size_t capacity);

// the first of context's actions with this type. false when it has none
bool ui_nav_context_action_find(UI_LAYER *ui_layer, ContextId context,
                                DataActionType type, UI_NAV_ACTION *out);

// most argument specs ui_nav_action_input reads for one action
#define UI_NAV_ACTION_ARGS_MAX 5

// the input an action needs, and the specs that describe it
typedef enum {
    UI_NAV_INPUT_NONE,
    UI_NAV_INPUT_STEPS,         // a step count, specs[0]
    UI_NAV_INPUT_NUMBER,        // specs[0]
    UI_NAV_INPUT_TEXT,          // a path or a string, specs[0]
    UI_NAV_INPUT_LIST_CHOICE,   // a row of specs[0]'s list
    UI_NAV_INPUT_LIST_CONNECT,  // a source from specs[0]'s list, then targets
                                // from specs[1]'s
    UI_NAV_INPUT_MISCONFIGURED, // fewer specs than the action needs
    UI_NAV_INPUT_UNSUPPORTED,   // specs no kind above takes, or no action
} UiNavInput;

// fill specs with action's argument specs and return what input it needs
UiNavInput ui_nav_action_input(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                               DataArgSpec specs[UI_NAV_ACTION_ARGS_MAX]);

// Run an action with the input ui_nav_action_input says it needs. out_new gets
// the context the action created, if any, and may be NULL.
DataActionResult ui_nav_action_run(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                   ContextId *out_new);
DataActionResult ui_nav_action_run_adjust(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                          int steps, ContextId *out_new);
DataActionResult ui_nav_action_run_set_value(UI_LAYER *ui_layer,
                                             const UI_NAV_ACTION *action, double value,
                                             ContextId *out_new);
// text is the name to save under for DATA_ACTION_SAVE, else a path. Borrowed
// for the call only
DataActionResult ui_nav_action_run_text(UI_LAYER *ui_layer, const UI_NAV_ACTION *action,
                                        const char *text, ContextId *out_new);

// what narrows a list. The two parts compose
typedef struct _ui_nav_list_filter{
    uint64_t group;   // DataChoice.group_key to keep, 0 = every group
    bool linked_only; // keep only DATA_CHOICE_LINKED rows
} UI_NAV_LIST_FILTER;

// one level of an action argument's list. As a UI_WINDOWING_SOURCE, shown
// means the row passes filter
typedef struct _ui_nav_list_rows{
    UI_LAYER *ui_layer;
    ContextId context;
    DataListId list;               // from the argument's DataArgSpec
    const DataActionReq *partial;  // the request built so far
    uint64_t branch;               // level of the list, 0 = top
    const UI_NAV_LIST_FILTER *filter; // NULL shows every row
} UI_NAV_LIST_ROWS;

// false when there is no row idx
bool ui_nav_list_row_at(const UI_NAV_LIST_ROWS *rows, size_t idx, DataChoice *out_row);

// pages with ui_nav_list_row_at when the list answers 0 for an unknown count
size_t ui_nav_list_count(const UI_NAV_LIST_ROWS *rows);

// rows must outlive the returned source
UI_WINDOWING_SOURCE ui_nav_list_source(UI_NAV_LIST_ROWS *rows);

// 0 when there is no row idx
uint64_t ui_nav_list_value_at(const UI_NAV_LIST_ROWS *rows, size_t idx);

// index of the row holding value, trying hint_idx first. SIZE_MAX when value is
// 0 or no row holds it
size_t ui_nav_list_find(const UI_NAV_LIST_ROWS *rows, uint64_t value, size_t hint_idx);

// a list's cursor and the row it is on, so it can follow that row when the list
// changes. prev / next are the shown rows around it, to place it when its row
// goes away. Zeroed, it lands on the first shown row
typedef struct _ui_nav_list_cursor{
    size_t idx;
    uint64_t value; // 0 until the cursor has landed on a row
    uint64_t prev;  // 0 when there is none
    uint64_t next;  // 0 when there is none
} UI_NAV_LIST_CURSOR;

// The cursor functions return false when the cursor did not land on a shown row
// and was left as it was.

// put the cursor on row idx, e.g. a tapped one
bool ui_nav_list_cursor_set(UI_NAV_LIST_ROWS *rows, UI_NAV_LIST_CURSOR *cursor, size_t idx);

// put the cursor back on its row, wherever the list moved it. A row the filter
// now hides passes it to the next shown row (wrapping), a row that is gone to
// the shown row before where it was
bool ui_nav_list_cursor_follow_row(UI_NAV_LIST_ROWS *rows, UI_NAV_LIST_CURSOR *cursor);

// move the cursor steps shown rows from cursor->idx, as ui_windowing_step
bool ui_nav_list_cursor_step(UI_NAV_LIST_ROWS *rows, UI_NAV_LIST_CURSOR *cursor, size_t steps,
                             bool forward, bool wrap);

// A list's groups are its rows' DataChoice.group_key values other than 0, in
// the order they first appear. The group functions read every row, rows->filter
// is not applied.

// index of the first row in group. SIZE_MAX when no row is in it
size_t ui_nav_list_group_first_row(const UI_NAV_LIST_ROWS *rows, uint64_t group);

// the group after current, the first one when current is 0. 0 (every group)
// after the last one, and when no row is in current
uint64_t ui_nav_list_group_next(const UI_NAV_LIST_ROWS *rows, uint64_t current);

// how many groups the list has, counting no further than limit
size_t ui_nav_list_group_count(const UI_NAV_LIST_ROWS *rows, size_t limit);

// the group of the nearest context, from context up through its parents, that
// declares one the list has. 0 when none does
uint64_t ui_nav_list_group_of_context(const UI_NAV_LIST_ROWS *rows, ContextId context);

// how many categories deep a session's list can be opened
#define UI_NAV_LIST_DEPTH_MAX 16
// the lists one session browses, a connect's source and targets
#define UI_NAV_LIST_SESSION_SPECS 2

// one opened category of a list (a DATA_CHOICE_BRANCH row): its value is the
// branch listed, parent_cursor is the level above's, given back on close
typedef struct _ui_nav_list_level{
    uint64_t branch;
    UI_NAV_LIST_CURSOR parent_cursor;
    bool has_label;
    char label[64]; // copied, a row's label is borrowed
} UI_NAV_LIST_LEVEL;

typedef enum {
    UI_NAV_CONNECT_PICK_SOURCE,
    UI_NAV_CONNECT_PICK_TARGETS,
} UiNavConnectPick;

// an action whose argument is picked from a live list, kept between frames.
// A choice browses specs[0]'s list and runs the action per picked row. A
// connect picks a source from specs[0]'s list, then links to targets from
// specs[1]'s. A category row is opened, never picked. The arrays are per spec
typedef struct _ui_nav_list_session{
    UI_NAV_ACTION action;
    UiNavInput input; // UI_NAV_INPUT_LIST_CHOICE or UI_NAV_INPUT_LIST_CONNECT
    DataArgSpec specs[UI_NAV_LIST_SESSION_SPECS];
    UiNavConnectPick connect_pick;
    // the request built so far: connect.source is set while picking targets
    DataActionReq partial;
    UI_NAV_LIST_CURSOR cursors[UI_NAV_LIST_SESSION_SPECS]; // of the innermost level
    UI_NAV_LIST_FILTER filters[UI_NAV_LIST_SESSION_SPECS]; // for every level
    UI_NAV_LIST_LEVEL levels[UI_NAV_LIST_SESSION_SPECS][UI_NAV_LIST_DEPTH_MAX];
    size_t depth[UI_NAV_LIST_SESSION_SPECS]; // opened levels per spec
} UI_NAV_LIST_SESSION;

// start a session for action, with the input and specs ui_nav_action_input
// gave it. false, session untouched, when input is not a list
bool ui_nav_list_session_start(UI_NAV_LIST_SESSION *session, const UI_NAV_ACTION *action,
                               UiNavInput input, const DataArgSpec *specs);

// narrow specs[0]'s list to the group of context, as ui_nav_list_group_of_context
void ui_nav_list_session_narrow_to(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session,
                                   ContextId context);

// the branch spec_idx's list shows, 0 = its top level
uint64_t ui_nav_list_session_branch(const UI_NAV_LIST_SESSION *session, size_t spec_idx);

// open category row in spec_idx's list, its cursor starts over. false when
// already UI_NAV_LIST_DEPTH_MAX deep
bool ui_nav_list_session_open(UI_NAV_LIST_SESSION *session, size_t spec_idx,
                              const DataChoice *row);

// close the innermost opened category of spec_idx's list, the cursor goes back
// onto its row. false at the top level
bool ui_nav_list_session_close(UI_NAV_LIST_SESSION *session, size_t spec_idx);

// the spec whose list is shown: 1 while a connect picks targets, else 0
size_t ui_nav_list_session_shown_spec(const UI_NAV_LIST_SESSION *session);

// the shown list at its innermost opened level, with its filter. The rows
// point into session
UI_NAV_LIST_ROWS ui_nav_list_session_rows(UI_LAYER *ui_layer,
                                          const UI_NAV_LIST_SESSION *session);

// once per frame, before reading the rows: drop a group no row is in any more
// from the filter, and let the cursor follow its row. false when the action's
// context is gone, the session is over
bool ui_nav_list_session_sync(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session);

typedef enum {
    UI_NAV_LIST_BACK_CLOSED_LEVEL, // closed the innermost opened category
    UI_NAV_LIST_BACK_TO_SOURCE,    // a connect is picking its source again
    UI_NAV_LIST_BACK_OVER,         // nothing to go back to, the session is over
} UiNavListBack;

// step back once in the shown list
UiNavListBack ui_nav_list_session_back(UI_NAV_LIST_SESSION *session);

// narrow the shown list to its next group, as ui_nav_list_group_next. With
// fewer than two groups it goes back to every group. false when the filter
// did not change
bool ui_nav_list_session_next_group(UI_LAYER *ui_layer, UI_NAV_LIST_SESSION *session);

// show only the targets linked to the picked source, or all of them again.
// false unless a connect is picking targets
bool ui_nav_list_session_toggle_linked(UI_NAV_LIST_SESSION *session);

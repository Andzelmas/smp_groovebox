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

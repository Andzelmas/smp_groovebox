#pragma once
#include "params.h"

// Param config files: one json file per owner kind, saying how its params are
// shown - name, increment, default, hidden, category - matched by the params'
// keys (see param_add_param). The app writes a file once, the user edits it.
// Every problem is logged. [main-thread] only

typedef enum {
    PARAM_CONFIG_OK,
    // read: "load" is false, the user values were cleared
    PARAM_CONFIG_NOT_LOADED,
    // read: there is no file at path
    PARAM_CONFIG_MISSING,
    // write: a file is already at path, it is left as it is
    PARAM_CONFIG_EXISTS,
    // the file could not be read or written, is not this format or is
    // another owner's; the user values are untouched
    PARAM_CONFIG_FAILED
} PARAM_CONFIG_RESULT;

// a new file at path for owner (a name that stays the same every run) with
// "load": false and the owner's values of every live param that has a key.
// Never replaces a file
PARAM_CONFIG_RESULT param_config_write(PRM_CONTAIN *param_container,
                                       const char *owner, const char *path);

// replaces the container's user values (param_user_table_set) with the
// params of the file at path - none when its "load" is false. Refused when
// the file is not owner's
PARAM_CONFIG_RESULT param_config_read(PRM_CONTAIN *param_container,
                                      const char *owner, const char *path);

#include "commands.h"
#include "log.h"
#include <stdlib.h>

static int freeze_status = 0; /* 0 = thawed, 1 = frozen */

static cJSON *handle_fsfreeze_status(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    return cJSON_CreateString(freeze_status ? "frozen" : "thawed");
}

static cJSON *handle_fsfreeze_freeze(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    LOG_DEBUG("Filesystem freeze simulated (no macOS equivalent)");
    freeze_status = 1;
    return cJSON_CreateNumber(1);
}

static cJSON *handle_fsfreeze_thaw(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    int thawed = freeze_status ? 1 : 0;
    freeze_status = 0;
    if (thawed)
        LOG_DEBUG("Filesystem thaw simulated");
    return cJSON_CreateNumber(thawed);
}

static cJSON *handle_fstrim(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;
    LOG_DEBUG("fstrim is a no-op on macOS (TRIM managed by OS/storage driver)");
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "paths", cJSON_CreateArray());
    return result;
}

void cmd_fs_init(void)
{
    command_register("guest-fsfreeze-status", handle_fsfreeze_status, 1);
    command_register("guest-fsfreeze-freeze", handle_fsfreeze_freeze, 1);
    command_register("guest-fsfreeze-freeze-list", handle_fsfreeze_freeze, 1);
    command_register("guest-fsfreeze-thaw", handle_fsfreeze_thaw, 1);
    command_register("guest-fstrim", handle_fstrim, 1);
}

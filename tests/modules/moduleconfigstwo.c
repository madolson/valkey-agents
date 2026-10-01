#include "valkeymodule.h"
#include <strings.h>

/* Second module configs module, for testing.
 * Need to make sure that multiple modules with configs don't interfere with each other */
int bool_config;

int getBoolConfigCommand(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(privdata);
    if (!strcasecmp(name, "test")) {
        return bool_config;
    }
    return 0;
}

int setBoolConfigCommand(const char *name, int new, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(err);
    if (!strcasecmp(name, "test")) {
        bool_config = new;
        return VALKEYMODULE_OK;
    }
    return VALKEYMODULE_ERR;
}

/* With any argument, the bool config is registered as json.max-path-limit,
 * which is a server config. The API isn't usable before Init, so the
 * argument's value is not read. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    const char *module_name = argc ? "json" : "configs";
    const char *config_name = argc ? "max-path-limit" : "test";
    if (ValkeyModule_Init(ctx, module_name, 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_RegisterBoolConfig(ctx, config_name, 1, VALKEYMODULE_CONFIG_DEFAULT, getBoolConfigCommand, setBoolConfigCommand, NULL, &argc) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_LoadConfigs(ctx) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}
#include "orca_api.h"
#include "orca_fileio.h"
#include "settings_module.h"
#include <string.h>

static const orca_api_t *s_list_api;

static void settings_list_cb(const char *key, const char *value) {
    s_list_api->console->write(key);
    s_list_api->console->write(": ");
    s_list_api->console->write(value);
    s_list_api->console->write("\r\n");
}

/* Command: list all settings */
static int cmd_settings_list(const orca_api_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    s_list_api = api;
    settings_list(settings_list_cb);
    return 0;
}

/* Command: get a setting */
static int cmd_settings_get(const orca_api_t *api, int argc, char **argv) {
    if (argc != 3) {
        api->console->write("Usage: settings get <key>\r\n");
        return -1;
    }
    
    const char *value = settings_get(argv[2]);
    if (value) {
        api->console->write(value);
        api->console->write("\r\n");
    } else {
        api->console->write("Key not found\r\n");
    }
    return 0;
}

/* Command: set a setting */
static int cmd_settings_set(const orca_api_t *api, int argc, char **argv) {
    if (argc != 4) {
        api->console->write("Usage: settings set <key> <value>\r\n");
        return -1;
    }
    
    settings_set(argv[2], argv[3]);
    api->console->write("Setting set\r\n");
    return 0;
}

/* Command: erase a setting */
static int cmd_settings_erase(const orca_api_t *api, int argc, char **argv) {
    if (argc != 3) {
        api->console->write("Usage: settings erase <key>\r\n");
        return -1;
    }
    
    if (settings_erase(argv[2])) {
        api->console->write("Setting erased\r\n");
    } else {
        api->console->write("Key not found\r\n");
    }
    return 0;
}

/* Command: show settings help */
static int cmd_settings_help(const orca_api_t *api, int argc, char **argv) {
    (void)argc; (void)argv;
    api->console->write("Settings commands:\r\n");
    api->console->write("  set <key> <value>   - set a setting\r\n");
    api->console->write("  get <key>           - get a setting value\r\n");
    api->console->write("  list                - list all settings\r\n");
    api->console->write("  erase <key>         - erase a setting\r\n");
    api->console->write("  help                - show this help\r\n");
    return 0;
}

/* Command entry point */
int orca_cmd_main(const orca_api_t *api, int argc, char **argv) {
    if (argc < 2) {
        api->console->write("Usage: settings <command> [args]\r\n");
        api->console->write("Commands:\r\n");
        api->console->write("  set <key> <value>   - set a setting\r\n");
        api->console->write("  get <key>           - get a setting value\r\n");
        api->console->write("  list                - list all settings\r\n");
        api->console->write("  erase <key>         - erase a setting\r\n");
        api->console->write("  help                - show help\r\n");
        return 0;
    }
    
    if (strcmp(argv[1], "set") == 0) {
        return cmd_settings_set(api, argc, argv);
    }
    
    if (strcmp(argv[1], "get") == 0) {
        return cmd_settings_get(api, argc, argv);
    }
    
    if (strcmp(argv[1], "list") == 0 && argc == 2) {
        cmd_settings_list(api, argc, argv);
        return 0;
    }
    
    if (strcmp(argv[1], "erase") == 0 && argc == 3) {
        cmd_settings_erase(api, argc, argv);
        return 0;
    }
    
    if (strcmp(argv[1], "help") == 0 && argc == 2) {
        cmd_settings_help(api, argc, argv);
        return 0;
    }
    
    api->console->write("Unknown settings command\r\n");
    return -1;
}

/* Module stop function (not needed for this simple module) */
void orca_app_stop(void) {
    /* Nothing to do - module is stateless */
}
#ifndef SETTINGS_MODULE_H
#define SETTINGS_MODULE_H

#include <stdint.h>

void settings_init(void);
void settings_deinit(void);
void settings_set(const char *key, const char *value);
const char* settings_get(const char *key);
bool settings_erase(const char *key);
int settings_list(void (*callback)(const char *key, const char *value));

#endif /* SETTINGS_MODULE_H */
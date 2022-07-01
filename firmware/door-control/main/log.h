#ifndef PORTAL300_LOG_H
#define PORTAL300_LOG_H

#include <esp_log.h>

#define LL_VERBOSE ESP_LOG_DEBUG
#define LL_WARNING ESP_LOG_WARN

#define log_print(_Mod, _Level, _Format, ...) \
  ESP_LOG_LEVEL_LOCAL(_Level, #_Mod, _Format, ##__VA_ARGS__)

#endif
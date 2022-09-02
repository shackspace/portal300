#ifndef PORTAL300_LOG_H
#define PORTAL300_LOG_H

#include <stdbool.h>

enum LogLevel
{
  LL_ERROR   = 0,
  LL_WARNING = 1,
  LL_MESSAGE = 2,
  LL_VERBOSE = 3,
};

enum LogSubSystem
{
  LSS_GENERIC,
  LSS_MQTT,
  LSS_LOGIC,
  LSS_GPIO,
  LSS_SYSTEM,
  LSS_IPC,
  LSS_API,
};

struct LogConsumer
{
  // configure:
  void * user_data;
  void (*log)(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const * msg);

  // internal:
  struct LogConsumer * next;
};

extern enum LogLevel log_level;

bool log_init(void);
void log_deinit(void);

void log_set_level(enum LogSubSystem subsystem, enum LogLevel level);

void log_register_consumer(struct LogConsumer * consumer);

void log_write(enum LogSubSystem subsystem, enum LogLevel level, char const * msg);
void log_print(enum LogSubSystem subsystem, enum LogLevel level, char const * fmt, ...) __attribute__((format(printf, 3, 4)));

void log_perror(enum LogSubSystem subsystem, enum LogLevel level, char const * msg);

char const * log_get_subsystem_name(enum LogSubSystem subsystem);
char const * log_get_level_name(enum LogLevel level);

#endif

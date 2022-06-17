#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define ANSI_COLOR_RED    "\x1B[0;31m"
#define ANSI_COLOR_YELLOW "\x1B[0;33m"
#define ANSI_COLOR_GRAY   "\x1B[0;35m"
#define ANSI_COLOR_RESET  "\x1B[0m"

static char const * const log_level_names[] = 
{
  [LL_ERROR] = "ERROR",
  [LL_WARNING] = "WARNING",
  [LL_MESSAGE] = "MESSAGE",
  [LL_VERBOSE] = "VERBOSE",
};

static char const * const subsystem_names[] =
{
  [LSS_GENERIC] = "generic",
  [LSS_MQTT] = "mqtt",
  [LSS_LOGIC] = "logic",
  [LSS_GPIO] = "gpio",
  [LSS_SYSTEM] = "system",
  [LSS_IPC] = "ipc",
};

static void log_write_stderr(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const *msg);

enum LogLevel log_level = LL_MESSAGE;

static struct LogConsumer * log_consumers = NULL;

static struct LogConsumer stderr_consumer = {
  .log = log_write_stderr,
  .user_data = NULL,
};

bool log_init(void)
{
  log_register_consumer(&stderr_consumer);
  return true;
}

void log_deinit(void)
{
  // nothing to do here yet
}

void log_register_consumer(struct LogConsumer *consumer)
{
  assert(consumer != NULL);
  assert(consumer->next == NULL);
  assert(consumer->log != NULL);

  consumer->next = log_consumers;
  log_consumers = consumer;
}

void log_write(enum LogSubSystem subsystem, enum LogLevel level, char const *msg)
{
  assert(msg != NULL);

  struct LogConsumer * it = log_consumers;
  while(it != NULL) {
    it->log(it->user_data, subsystem, level, msg);
    it = it->next;
  }
}

void log_print(enum LogSubSystem subsystem, enum LogLevel level, char const *fmt, ...)
{
  assert(fmt != NULL);
  
  char log_buffer[8192];

  va_list list;
  va_start(list, fmt);
  vsnprintf(log_buffer, sizeof log_buffer, fmt, list);
  va_end(list);

  log_write(subsystem, level, log_buffer);
}

void log_perror(enum LogSubSystem subsystem, enum LogLevel level, char const *msg)
{
  assert(msg != NULL);

  char log_buffer[8192];

  char const * errmsg = strerror(errno);
  if(errmsg == NULL) {
    errmsg = "unknown error";
  }

  strncpy(log_buffer, msg, sizeof log_buffer);
  strncpy(log_buffer, ": ", sizeof log_buffer);
  strncpy(log_buffer, errmsg, sizeof log_buffer);

  log_write(subsystem, level, log_buffer);
}



static void log_write_stderr(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const *msg)
{
  (void)user_data;
  assert(msg != NULL);
  
  char const * color = "";
  switch(level) {
    case LL_ERROR:   color = ANSI_COLOR_RED; break;
    case LL_WARNING: color = ANSI_COLOR_YELLOW; break;
    case LL_MESSAGE: color = ""; break;
    case LL_VERBOSE: color = ANSI_COLOR_GRAY; break;
  }

  fprintf(stderr, "[%s%s%s] [%s] %s\n", color, log_level_names[level], ANSI_COLOR_RESET, subsystem_names[subsystem], msg);
}


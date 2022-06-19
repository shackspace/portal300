#include "log.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ANSI_COLOR_RED    "\x1B[0;31m"
#define ANSI_COLOR_YELLOW "\x1B[0;33m"
#define ANSI_COLOR_GRAY   "\x1B[0;35m"
#define ANSI_COLOR_RESET  "\x1B[0m"

static enum LogLevel subsystem_max_level[] = {
    [LSS_GENERIC] = LL_VERBOSE,
    [LSS_MQTT]    = LL_VERBOSE,
    [LSS_LOGIC]   = LL_VERBOSE,
    [LSS_GPIO]    = LL_VERBOSE,
    [LSS_SYSTEM]  = LL_VERBOSE,
    [LSS_IPC]     = LL_VERBOSE,
};

static void log_write_stderr(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const * msg);

enum LogLevel log_level = LL_MESSAGE;

static enum LogLevel effective_log_level(enum LogSubSystem subsystem)
{
  enum LogLevel ll = subsystem_max_level[subsystem];
  if (ll > log_level) {
    return log_level;
  }
  else {
    return ll;
  }
}

static struct LogConsumer * log_consumers = NULL;

static struct LogConsumer stderr_consumer = {
    .log       = log_write_stderr,
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

void log_set_level(enum LogSubSystem subsystem, enum LogLevel level)
{
  subsystem_max_level[subsystem] = level;
}

void log_register_consumer(struct LogConsumer * consumer)
{
  assert(consumer != NULL);
  assert(consumer->next == NULL);
  assert(consumer->log != NULL);

  consumer->next = log_consumers;
  log_consumers  = consumer;
}

void log_write(enum LogSubSystem subsystem, enum LogLevel level, char const * msg)
{
  assert(msg != NULL);

  // filter messages by log level. subsystems can have filtered levels
  if (level > effective_log_level(subsystem))
    return;

  struct LogConsumer * it = log_consumers;
  while (it != NULL) {
    it->log(it->user_data, subsystem, level, msg);
    it = it->next;
  }
}

void log_print(enum LogSubSystem subsystem, enum LogLevel level, char const * fmt, ...)
{
  assert(fmt != NULL);

  char log_buffer[8192];

  va_list list;
  va_start(list, fmt);
  vsnprintf(log_buffer, sizeof log_buffer, fmt, list);
  va_end(list);

  log_write(subsystem, level, log_buffer);
}

void log_perror(enum LogSubSystem subsystem, enum LogLevel level, char const * msg)
{
  assert(msg != NULL);

  char log_buffer[8192];

  char const * errmsg = strerror(errno);
  if (errmsg == NULL) {
    errmsg = "unknown error";
  }

  strncpy(log_buffer, msg, sizeof log_buffer);
  strncpy(log_buffer, ": ", sizeof log_buffer);
  strncpy(log_buffer, errmsg, sizeof log_buffer);

  log_write(subsystem, level, log_buffer);
}

static void log_write_stderr(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const * msg)
{
  (void)user_data;
  assert(msg != NULL);

  char const * color = "";
  switch (level) {
  case LL_ERROR: color = ANSI_COLOR_RED; break;
  case LL_WARNING: color = ANSI_COLOR_YELLOW; break;
  case LL_MESSAGE: color = ""; break;
  case LL_VERBOSE: color = ANSI_COLOR_GRAY; break;
  }

  fprintf(stderr, "[%s%s%s] [%s] %s\n", color, log_get_level_name(level), ANSI_COLOR_RESET, log_get_subsystem_name(subsystem), msg);
}

char const * log_get_subsystem_name(enum LogSubSystem subsystem)
{
  if (subsystem == LSS_GENERIC)
    return "generic";
  if (subsystem == LSS_MQTT)
    return "mqtt";
  if (subsystem == LSS_LOGIC)
    return "logic";
  if (subsystem == LSS_GPIO)
    return "gpio";
  if (subsystem == LSS_SYSTEM)
    return "system";
  if (subsystem == LSS_IPC)
    return "ipc";
  return "<<INVALID>>";
}

char const * log_get_level_name(enum LogLevel level)
{
  if (level == LL_ERROR)
    return "ERROR";
  if (level == LL_WARNING)
    return "WARNING";
  if (level == LL_MESSAGE)
    return "MESSAGE";
  if (level == LL_VERBOSE)
    return "VERBOSE";
  return "<<INVALID>>";
}
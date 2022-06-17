#include "ipc.h"
#include "log.h"
#include "mqtt-client.h"

#include <portal300.h>

#include <bits/types/struct_itimerspec.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <mqtt.h>

// Configuration:

#define MQTT_RECONNECT_TIMEOUT 5 // seconds

// Globals:

struct CliOptions
{
  bool         help;
  char const * host_name;
  int          port;
  char const * ca_cert_file;
  char const * client_key_file;
  char const * client_crt_file;
};

static volatile sig_atomic_t shutdown_requested = 0;

static int ipc_sock = -1;

static struct MqttClient * mqtt_client = NULL;

#define POLLFD_IPC       0  // well defined fd, always the unix socket for IPC
#define POLLFD_MQTT      1  // well defined fd, either the timerfd for reconnecting MQTT or the socket for MQTT communications
#define POLLFD_FIRST_IPC 2  // First ipc client socket slot
#define POLLFD_LIMIT     32 // number of maximum socket connections

//! Stack array of pollfds. Everything below POLLFD_FIRST_IPC is pre-intialized and has a static purpose
//! while everything at POLLFD_FIRST_IPC till POLLFD_LIMIT is a dynamic stack of pollfds for ipc client connections.
static struct pollfd pollfds[POLLFD_LIMIT];
static size_t        pollfds_size = POLLFD_FIRST_IPC;

//! Index of an invalid IPC client
static const size_t INVALID_IPC_CLIENT = ~0U;

static void close_ipc_sock(void);
static void close_mqtt_client(void);

static void sigint_handler(int sig, siginfo_t * info, void * ucontext);
static void sigterm_handler(int sig, siginfo_t * info, void * ucontext);

static size_t add_ipc_client(int fd);
static void   remove_ipc_client(size_t index);

static bool try_connect_mqtt(void);
static bool install_signal_handlers(void);
static int  create_reconnect_timeout_timer(int secs);

static bool send_ipc_info(int fd, char const * text);
static bool send_ipc_infof(int fd, char const * fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fetch_timer_fd(int fd);

static bool send_mqtt_msg(char const * topic, char const * data);

// static bool disarm_timer(int timer);
// static bool arm_timer(int timer, bool oneshot, uint32_t ms);

static bool parse_cli(int argc, char ** argv, struct CliOptions * args);

static void print_usage(FILE * stream);

static void panic(char const * msg)
{
  log_print(LSS_SYSTEM, LL_ERROR, "\n\nPANIC: %s\n\n\n", msg);
  exit(EXIT_FAILURE);
}

int main(int argc, char ** argv)
{
  // Initialize libraries and dependencies:

  if (!log_init()) {
    fprintf(stderr, "failed to initialize logging.\n");
    return EXIT_FAILURE;
  }

  if (!install_signal_handlers()) {
    log_print(LSS_SYSTEM, LL_ERROR, "failed to install signal handlers.");
    exit(EXIT_FAILURE);
  }

  if (!mqtt_client_init()) {
    log_write(LSS_MQTT, LL_ERROR, "failed to initialize mqtt client library. aborting.");
    return EXIT_FAILURE;
  }

  (void)argc;
  (void)argv;
  // struct CliOptions const cli = {
  //     .host_name       = "mqtt.portal.shackspace.de",
  //     .port            = 8883,
  //     .ca_cert_file    = "debug/ca.crt",
  //     .client_key_file = "debug/client.key",
  //     .client_crt_file = "debug/client.crt",
  // };
  struct CliOptions cli;
  if (!parse_cli(argc, argv, &cli)) {
    return EXIT_FAILURE;
  }

  if (cli.help) {
    print_usage(stdout);
    return EXIT_SUCCESS;
  }

  // Create MQTT client from CLI info
  mqtt_client = mqtt_client_create(
      cli.host_name,
      cli.port,
      cli.ca_cert_file,
      cli.client_key_file,
      cli.client_crt_file,
      PORTAL300_TOPIC_STATUS_SSH_INTERFACE,
      "offline");
  if (mqtt_client == NULL) {
    log_write(LSS_MQTT, LL_ERROR, "failed to create mqtt client.");
    return EXIT_FAILURE;
  }
  atexit(close_mqtt_client);

  ipc_sock = ipc_create_socket();
  if (ipc_sock == -1) {
    return EXIT_FAILURE;
  }

  if (try_connect_mqtt()) {
    // we successfully connected to MQTT, set up the poll entry for MQTT action:
    pollfds[POLLFD_MQTT] = (struct pollfd){
        .fd      = mqtt_client_get_socket_fd(mqtt_client),
        .events  = POLLIN,
        .revents = 0,
    };
  }
  else {
    log_print(LSS_MQTT, LL_WARNING, "failed to connect to mqtt server, retrying in %d seconds", MQTT_RECONNECT_TIMEOUT);

    // we failed to connect to MQTT, set up a timerfd to retry in some seconds
    int timer = create_reconnect_timeout_timer(MQTT_RECONNECT_TIMEOUT);

    pollfds[POLLFD_MQTT] = (struct pollfd){
        .fd      = timer,
        .events  = POLLIN,
        .revents = 0,
    };
  }

  // Bind and setup the ipc socket, so we can receive ipc messages
  {
    if (bind(ipc_sock, (struct sockaddr const *)&ipc_socket_address, sizeof ipc_socket_address) == -1) {
      log_perror(LSS_IPC, LL_ERROR, "failed to bind ipc socket");
      log_print(LSS_IPC, LL_ERROR, "is another instance of this daemon already running?");
      return EXIT_FAILURE;
    }
    atexit(close_ipc_sock);

    if (listen(ipc_sock, 0) == -1) {
      log_perror(LSS_IPC, LL_ERROR, "failed to listen on ipc socket");
      return EXIT_FAILURE;
    }
    pollfds[POLLFD_IPC] = (struct pollfd){
        .fd      = ipc_sock,
        .events  = POLLIN,
        .revents = 0,
    };
  }

  while (shutdown_requested == false) {
    // sync the mqtt client to send some leftovers
    mqtt_client_sync(mqtt_client);

    int const poll_ret = poll(pollfds, pollfds_size, -1); // wait infinitly for an event
    if (poll_ret == -1) {
      if (errno != EINTR) {
        log_perror(LSS_SYSTEM, LL_ERROR, "central poll failed");
      }
      continue;
    }

    struct timespec loop_start, loop_end;
    clock_gettime(CLOCK_MONOTONIC, &loop_start);

    for (size_t i = 0; i < pollfds_size; i++) {
      const struct pollfd pfd = pollfds[i];
      if (pfd.revents != 0) {
        switch (i) {
        // this is the IPC listener. accept clients here
        case POLLFD_IPC:
        {
          assert(pfd.fd == ipc_sock);

          int client_fd = accept(ipc_sock, NULL, NULL);
          if (client_fd != -1) {
            size_t const index = add_ipc_client(client_fd);
            if (index != INVALID_IPC_CLIENT) {
              log_print(LSS_IPC, LL_MESSAGE, "accepted new IPC client on connection slot %zu", index);
            }
            else {
              if (close(client_fd) == -1) {
                log_perror(LSS_IPC, LL_WARNING, "failed to close ipc socket connection");
              }
            }
          }
          else {
            log_perror(LSS_IPC, LL_WARNING, "failed to accept ipc client");
          }
          break;
        }

        // Incoming MQTT message or connection closure
        case POLLFD_MQTT:
        {
          if (mqtt_client_is_connected(mqtt_client)) {
            // we're connected, so the fd is the mqtt socket.
            // process messages and handle errors here.

            if (!mqtt_client_sync(mqtt_client)) {
              if (mqtt_client_is_connected(mqtt_client)) {
                // TODO: Handle mqtt errors
                log_write(LSS_MQTT, LL_ERROR, "Handle MQTT error here gracefully");
              }
              else {
                log_print(LSS_MQTT, LL_WARNING, "Lost connection to MQTT, reconnecting in %d seconds...", MQTT_RECONNECT_TIMEOUT);
                pollfds[i].fd = create_reconnect_timeout_timer(MQTT_RECONNECT_TIMEOUT);
              }
            }
          }
          else {
            if (!fetch_timer_fd(pfd.fd)) {
              log_perror(LSS_MQTT, LL_ERROR, "failed to read from timerfd");
              log_write(LSS_MQTT, LL_ERROR, "destroying daemon, hoping for restart...");
              exit(EXIT_FAILURE);
            }

            if (try_connect_mqtt()) {
              log_write(LSS_MQTT, LL_MESSAGE, "successfully reconnected to mqtt server.");

              if (close(pfd.fd) == -1) {
                log_perror(LSS_MQTT, LL_WARNING, "failed to destroy timerfd");
              }

              pollfds[i].fd = mqtt_client_get_socket_fd(mqtt_client);
            }
            else {
              log_print(LSS_MQTT, LL_WARNING, "failed to connect to mqtt server, retrying in %d seconds", MQTT_RECONNECT_TIMEOUT);
            }
          }
          break;
        }

        // IPC client message or error
        default:
        {
          if (pfd.revents & POLLERR) {
            log_print(LSS_IPC, LL_MESSAGE, "lost IPC client on connection slot %zu", i);
            remove_ipc_client(i);
          }
          else if (pfd.revents & POLLIN) {
            struct IpcMessage msg;

            enum IpcRcvResult msg_ok = ipc_receive_msg(pfd.fd, &msg);

            switch (msg_ok) {
            case IPC_EOF:
            {
              remove_ipc_client(i);
              log_print(LSS_IPC, LL_MESSAGE, "connection to ipc socket slot %zu closed", i);
              break;
            }
            case IPC_ERROR:
            {
              // we already printed an error message, just try again in the next loop
              break;
            }
            case IPC_SUCCESS:
            {
              switch (msg.type) {
              case IPC_MSG_OPEN_BACK:
              case IPC_MSG_OPEN_FRONT:
              {
                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal opening via %s for (%d, '%.*s', '%.*s').", i, (msg.type == IPC_MSG_OPEN_BACK) ? "back door" : "front door", msg.data.open.member_id, (int)strnlen(msg.data.open.member_nick, sizeof msg.data.open.member_nick), msg.data.open.member_nick, (int)strnlen(msg.data.open.member_name, sizeof msg.data.open.member_name), msg.data.open.member_name);

                // TODO: Handle open message

                send_ipc_infof(pfd.fd, "Portal wird geöffnet, bitte warten...");

                bool ok = send_mqtt_msg(
                    PORTAL300_TOPIC_ACTION_OPEN_DOOR,
                    (msg.type == IPC_MSG_OPEN_BACK) ? DOOR_C2 : DOOR_B2);
                if (!ok) {
                  send_ipc_infof(pfd.fd, "Konnte Portal nicht öffnen!");
                  remove_ipc_client(i);
                  break;
                }

                break;
              }

              case IPC_MSG_CLOSE:
              {
                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal close.", i);

                // TODO: Handle close message

                send_ipc_infof(pfd.fd, "Portal wird geschlossen, bitte warten...");

                break;
              }

              case IPC_MSG_SHUTDOWN:
              {
                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal shutdown.", i);

                send_ipc_infof(pfd.fd, "Shutdown wird zur Zeit noch nicht unterstützt...");
                remove_ipc_client(i);

                break;
              }

              case IPC_MSG_QUERY_STATUS:
              {
                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal status.", i);

                (void)send_ipc_infof(pfd.fd, "Portal-Status:");
                (void)send_ipc_infof(pfd.fd, "  Aktivität:     %s", "???"); // idle, öffnen, schließen
                (void)send_ipc_infof(pfd.fd, "  MQTT:          %s", mqtt_client_is_connected(mqtt_client) ? "Verbunden" : "Nicht verbunden");
                (void)send_ipc_infof(pfd.fd, "  IPC Clients:   %zu", pollfds_size - POLLFD_FIRST_IPC);
                (void)send_ipc_infof(pfd.fd, "  Schließbolzen: %s", "???"); // geöffnet, geschlossen
                (void)send_ipc_infof(pfd.fd, "  Türsensor:     %s", "???"); // geöffnet, geschlossen
                (void)send_ipc_info(pfd.fd, "");
                (void)send_ipc_infof(pfd.fd, "Das Portal ist noch nicht vollständig implementiert. Auf Wiedersehen!");

                // after a status message, we can just drop the client connection
                remove_ipc_client(i);
                break;
              }

              default:
              {
                // Invalid message received. Print error message and kick the client
                log_print(LSS_IPC, LL_WARNING, "received invalid ipc message of type %u", msg.type);
                remove_ipc_client(i);
                break;
              }
              }
            }
            }
          }
          break;
        }
        }
      }
    }

    clock_gettime(CLOCK_MONOTONIC, &loop_end);

    uint64_t total_nsecs = 1000000000UL * (loop_end.tv_sec - loop_start.tv_sec);

    if ((loop_end.tv_nsec - loop_start.tv_nsec) < 0) {
      total_nsecs += loop_end.tv_nsec - loop_start.tv_nsec + 1000000000UL;
      total_nsecs -= 1;
    }
    else {
      total_nsecs += loop_end.tv_nsec - loop_start.tv_nsec;
    }

    if (total_nsecs > 10000000UL) { // 1ms
      double       time = total_nsecs;
      char const * unit = "ns";

      if (total_nsecs >= 1500000000UL) {
        unit = "s";
        time = total_nsecs / 10000000000.0;
      }
      else if (total_nsecs >= 1500000UL) {
        unit = "ms";
        time = total_nsecs / 10000000.0;
      }
      else if (total_nsecs >= 1500UL) {
        unit = "us";
        time = total_nsecs / 1000.0;
      }

      log_print(LSS_SYSTEM, LL_WARNING, "main loop is hanging, took %.3f %s", time, unit);
    }
  }

  return EXIT_SUCCESS;
}

static bool send_ipc_info(int fd, char const * text)
{
  struct IpcMessage msg = {
      .type      = IPC_MSG_INFO,
      .data.info = "",
  };

  strncpy(msg.data.info, text, sizeof msg.data.info);

  return ipc_send_msg(fd, msg);
}

static bool send_ipc_infof(int fd, char const * fmt, ...)
{
  struct IpcMessage msg = {
      .type      = IPC_MSG_INFO,
      .data.info = "",
  };

  va_list list;
  va_start(list, fmt);
  vsnprintf(msg.data.info, sizeof msg.data.info, fmt, list);
  va_end(list);

  return ipc_send_msg(fd, msg);
}

static bool try_connect_mqtt()
{
  assert(mqtt_client != NULL);

  if (!mqtt_client_connect(mqtt_client)) {
    return false;
  }

  if (!mqtt_client_publish(mqtt_client, PORTAL300_TOPIC_STATUS_SSH_INTERFACE, "online", 2)) {
    log_print(LSS_MQTT, LL_ERROR, "failed to publish message to mqtt server.");
    return false;
  }

  if (!mqtt_client_subscribe(mqtt_client, "#")) {
    log_print(LSS_MQTT, LL_ERROR, "failed to subscribe to topic '#' on mqtt server.");
    return false;
  }

  return true;
}

/// Sends a mqtt message.
/// - `topic` is a NUL terminated string.
/// - `data` is a pointer to the message payload.
/// - `data_length` is either 0 for a NUL terminated payload or the length of the payload in bytes.
static bool send_mqtt_msg(char const * topic, char const * data)
{
  assert(topic != NULL);
  assert(data != NULL);

  if (!mqtt_client_is_connected(mqtt_client)) {
    log_print(LSS_MQTT, LL_ERROR, "failed to publish message to mqtt server: not connected.");
    return false;
  }

  if (!mqtt_client_publish(mqtt_client, topic, data, 2)) {
    log_print(LSS_MQTT, LL_ERROR, "failed to publish message to mqtt server.");
    return false;
  }

  return true;
}

static bool install_signal_handlers()
{
  static struct sigaction const sigint_action = {
      .sa_sigaction = sigint_handler,
      .sa_flags     = SA_SIGINFO,
  };
  if (sigaction(SIGINT, &sigint_action, NULL) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to set SIGINT handler");
    return false;
  }

  static struct sigaction const sigterm_action = {
      .sa_sigaction = sigterm_handler,
      .sa_flags     = SA_SIGINFO,
  };
  if (sigaction(SIGTERM, &sigterm_action, NULL) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to set SIGTERM handler");
    return false;
  }

  return true;
}

static int create_reconnect_timeout_timer(int secs)
{
  const struct itimerspec restart_timeout = {
      .it_value = {
          .tv_sec  = secs,
          .tv_nsec = 0,
      },
      .it_interval = {
          .tv_sec  = secs,
          .tv_nsec = 0,
      },
  };

  int timer = timerfd_create(CLOCK_MONOTONIC, 0);
  if (timer == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to create timerfd");
    log_print(LSS_SYSTEM, LL_ERROR, "destroying daemon, hoping for restart...");
    exit(EXIT_FAILURE);
  }

  if (timerfd_settime(timer, 0, &restart_timeout, NULL) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to arm timerfd");
    log_print(LSS_SYSTEM, LL_ERROR, "destroying daemon, hoping for restart...");
    exit(EXIT_FAILURE);
  }

  return timer;
}

// static bool configure_timerfd(int timer, bool oneshot, uint32_t ms)
// {
//   assert(timer != -1);

//   const struct timespec given_ms = {
//     .tv_sec = ms / 1000 ,
//     .tv_nsec = 1000000 * (ms % 1000),
//   };

//   const struct timespec null_time = {
//     .tv_sec = 0,
//     .tv_nsec = 0,
//   };

//   struct itimerspec timeout;
//   if(oneshot) {
//     timeout = (struct itimerspec) {
//       .it_value = given_ms,
//       .it_interval = null_time,
//     };
//   }
//   else {
//     timeout = (struct itimerspec) {
//       .it_value = given_ms,
//       .it_interval = given_ms,
//     };
//   }

//   if(timerfd_settime(timer, 0, &timeout, NULL) == -1) {
//     log_perror(LSS_SYSTEM, LL_ERROR, "failed to arm timerfd");
//     log_print(LSS_SYSTEM, LL_ERROR, "destroying daemon, hoping for restart...");
//     exit(EXIT_FAILURE);
//   }

//   return timer;
// }

// static bool disarm_timer(int timer)
// {
//   return configure_timerfd(timer, false, 0);
// }

// static bool arm_timer(int timer, bool oneshot, uint32_t ms)
// {
//   assert(ms > 0);
//   return configure_timerfd(timer, oneshot, ms);
// }

static size_t add_ipc_client(int fd)
{
  if (pollfds_size >= POLLFD_LIMIT) {
    log_print(LSS_IPC, LL_WARNING, "cannot accept ipc client: too many ipc connections!");
    return INVALID_IPC_CLIENT;
  }

  size_t index = pollfds_size;
  pollfds_size += 1;

  pollfds[index] = (struct pollfd){
      .fd      = fd,
      .events  = POLLIN,
      .revents = 0,
  };

  return index;
}

static void remove_ipc_client(size_t index)
{
  assert(index >= POLLFD_FIRST_IPC);
  assert(index < pollfds_size);

  // close the socket when we remove a client connection
  if (close(pollfds[index].fd) == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to close ipc client");
  }

  // swap-remove with the last index
  // NOTE: This doesn't hurt us when (index == pollfds_size-1), as we're gonna wipe the element then anyways
  pollfds[index] = pollfds[pollfds_size - 1];

  pollfds_size -= 1;
  memset(&pollfds[pollfds_size], 0xAA, sizeof(struct pollfd));
}

static void close_ipc_sock()
{
  if (close(ipc_sock) == -1) {
    log_perror(LSS_IPC, LL_WARNING, "failed to close ipc socket properly");
  }
  ipc_sock = -1;

  if (unlink(ipc_socket_address.sun_path) == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to delete socket handle");
  }
}

static void close_mqtt_client()
{
  mqtt_client_destroy(mqtt_client);
  mqtt_client = NULL;
}

static void sigint_handler(int sig, siginfo_t * info, void * ucontext)
{
  (void)sig;
  (void)info;
  (void)ucontext;
  shutdown_requested = 1;
}

static void sigterm_handler(int sig, siginfo_t * info, void * ucontext)
{
  (void)sig;
  (void)info;
  (void)ucontext;
  shutdown_requested = 1;
}

static bool fetch_timer_fd(int fd)
{
  uint64_t counter;
  int      res = read(fd, &counter, sizeof counter);
  if (res == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to read from timerfd");
    return false;
  }
  return true;
}

static bool parse_cli(int argc, char ** argv, struct CliOptions * args)
{
  *args = (struct CliOptions){
      .help            = false,
      .host_name       = "mqtt.portal.shackspace.de",
      .port            = 8883,
      .ca_cert_file    = NULL,
      .client_key_file = NULL,
      .client_crt_file = NULL,
  };

  {
    int opt;
    while ((opt = getopt(argc, argv, "hH:p:k:c:C:")) != -1) {
      switch (opt) {

      case 'h':
      { // help
        args->help = true;
        return true;
      }

      case 'H':
      { // Host
        args->host_name = strdup(optarg);
        if (args->host_name == NULL) {
          panic("out of memory");
        }
        break;
      }

      case 'p':
      { // port
        errno          = 0;
        char * end_ptr = optarg;
        args->port     = strtol(optarg, &end_ptr, 10);
        if ((errno != 0) || (end_ptr != (optarg + strlen(optarg))) || (args->port <= 0) || (args->port >= 65535)) {
          fprintf(stderr, "invalid port number: %s\n", optarg);
          return false;
        }

        break;
      }

      case 'k':
      {
        // client certificate key file
        args->client_key_file = strdup(optarg);
        if (args->client_key_file == NULL) {
          panic("out of memory");
        }
        break;
      }

      case 'c':
      {
        // client certificate file
        args->client_crt_file = strdup(optarg);
        if (args->client_crt_file == NULL) {
          panic("out of memory");
        }
        break;
      }

      case 'C':
      { // ca certificate file
        args->ca_cert_file = strdup(optarg);
        if (args->ca_cert_file == NULL) {
          panic("out of memory");
        }
        break;
      }

      default:
      {
        // unknown argument, error message is already printed by getopt
        return false;
      }
      }
    }
  }

  if (optind != argc) {
    print_usage(stderr);
    return false;
  }

  bool params_ok = true;

  if (args->ca_cert_file == NULL) {
    fprintf(stderr, "Missing option -C\n");
    params_ok = false;
  }
  if (args->client_crt_file == NULL) {
    fprintf(stderr, "Missing option -c\n");
    params_ok = false;
  }
  if (args->client_key_file == NULL) {
    fprintf(stderr, "Missing option -k\n");
    params_ok = false;
  }
  if (args->host_name == NULL) {
    fprintf(stderr, "Missing option -H\n");
    params_ok = false;
  }

  if (params_ok == false) {
    return false;
  }

  return true;
}

static void print_usage(FILE * stream)
{
  static const char usage_msg[] =
      "portal-daemon [-h] -H <host> -C <ca certificate> -c <client certificate> -k <client key>\n"
      "TODO!\n";

  fprintf(stream, usage_msg);
}

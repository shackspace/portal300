#include "ipc.h"
#include "log.h"
#include "mqtt-client.h"
#include "state-machine.h"

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
#include <termios.h>
#include <sys/ioctl.h>

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
  char const * serial_device_name;
};

struct DeviceStatus
{
  bool ssh_interface;
  bool door_control_b2;
  bool door_control_c2;
  bool busch_interface;
};

static struct DeviceStatus device_status = {
    .ssh_interface   = true, // we're always online
    .door_control_b2 = false,
    .door_control_c2 = false,
    .busch_interface = false,
};

enum IpcDisconnectFlag
{
  IPC_DISCONNECT_ON_LOCKED    = (1 << 0),
  IPC_DISCONNECT_ON_OPEN      = (1 << 1),
  IPC_DISCONNECT_ON_NO_CHANGE = (1 << 2),
  IPC_DISCONNECT_ON_ERROR     = (1 << 3),
};

struct IpcClientInfo
{
  uint32_t client_id;
  uint32_t disconnect_flags;
  bool     forward_logs;

  char * nick_name;
  char * full_name;
  int    member_id;
};

static volatile sig_atomic_t shutdown_requested = 0;

static int ipc_sock   = -1;
static int sm_timerfd = -1;

static struct MqttClient * mqtt_client = NULL;

#define POLLFD_IPC       0  // well defined fd: always the unix socket for IPC
#define POLLFD_MQTT      1  // well defined fd: either the timerfd for reconnecting MQTT or the socket for MQTT communications
#define POLLFD_SM_TIMER  2  // well defined fd: timerfd for answering state machine requests
#define POLLFD_FIRST_IPC 3  // First ipc client socket slot
#define POLLFD_LIMIT     32 // number of maximum socket connections

//! Stack array of pollfds. Everything below POLLFD_FIRST_IPC is pre-intialized and has a static purpose
//! while everything at POLLFD_FIRST_IPC till POLLFD_LIMIT is a dynamic stack of pollfds for ipc client connections.
static struct pollfd pollfds[POLLFD_LIMIT];
struct IpcClientInfo ipc_client_info_storage[POLLFD_LIMIT];
static uint32_t      pollfds_size = POLLFD_FIRST_IPC;

//! Index of an invalid IPC client
static const uint32_t INVALID_IPC_CLIENT = ~0U;

static void close_ipc_sock(void);
static void close_mqtt_client(void);

static void sigint_handler(int sig, siginfo_t * info, void * ucontext);
static void sigterm_handler(int sig, siginfo_t * info, void * ucontext);

static size_t add_ipc_client(int fd);
static void   remove_ipc_client(size_t index);
static void   remove_all_ipc_clients(uint32_t disconnect_flags);

static bool try_connect_mqtt(void);
static bool install_signal_handlers(void);
static int  create_reconnect_timeout_timer(int secs);

static bool send_ipc_info(int fd, char const * text);
static bool send_ipc_infof(int fd, char const * fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fetch_timer_fd(int fd);

static bool send_mqtt_msg(char const * topic, char const * data);

static void mqtt_handle_message(void * user_data, char const * topic, char const * data);

static bool disarm_timer(int timer);
static bool arm_timer(int timer, bool oneshot, uint32_t ms);

static bool parse_cli(int argc, char ** argv, struct CliOptions * args);

static void print_usage(FILE * stream);

static void panic(char const * msg)
{
  log_print(LSS_SYSTEM, LL_ERROR, "\n\nPANIC: %s\n\n\n", msg);
  exit(EXIT_FAILURE);
}

static void state_machine_signal_handler(void * user_data, void * context, enum SM_Signal signal);

static bool push_signal(enum SM_Signal sig, uint32_t client_id);
static bool pop_signal(enum SM_Signal * sig, uint32_t * client_id);

static struct StateMachine global_state_machine;

static uint32_t find_ipc_client_by_id(uint32_t client_id);

static uint32_t fetch_next_client_id(void);

static void print_log_to_ipc_clients(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const * msg);

static struct LogConsumer ipc_client_logger = {
    .log       = print_log_to_ipc_clients,
    .user_data = NULL,
};

static void close_sm_timerfd(void);

static void update_api_status(void);

struct CliOptions cli;

int main(int argc, char ** argv)
{
  // Initialize libraries and dependencies:
  if (!log_init()) {
    fprintf(stderr, "failed to initialize logging.\n");
    return EXIT_FAILURE;
  }

  sm_init(&global_state_machine, state_machine_signal_handler, NULL);

  log_set_level(LSS_IPC, LL_WARNING);

  sm_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (sm_timerfd == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to create state machine timerfd");
    return EXIT_FAILURE;
  }
  atexit(close_sm_timerfd);

  pollfds[POLLFD_SM_TIMER] = (struct pollfd){
      .fd     = sm_timerfd,
      .events = POLLIN,
  };

  if (!install_signal_handlers()) {
    log_print(LSS_SYSTEM, LL_ERROR, "failed to install signal handlers.");
    return EXIT_FAILURE;
  }

  if (!mqtt_client_init()) {
    log_write(LSS_MQTT, LL_ERROR, "failed to initialize mqtt client library. aborting.");
    return EXIT_FAILURE;
  }

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
      "offline",
      mqtt_handle_message,
      NULL);
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

    if (!ipc_set_flags(ipc_sock)) {
      log_perror(LSS_IPC, LL_ERROR, "failed to change permissions on ipc socket");
      return EXIT_FAILURE;
    }
  }

  log_register_consumer(&ipc_client_logger);

  while (shutdown_requested == false) {
    {
      enum SM_Signal signal;
      uint32_t       client_id;
      while (pop_signal(&signal, &client_id)) {
        uint32_t const               ipc_client_index = find_ipc_client_by_id(client_id);
        uint32_t const               ipc_client_valid = (ipc_client_index != INVALID_IPC_CLIENT);
        struct IpcClientInfo * const ipc_client_data  = ipc_client_valid ? &ipc_client_info_storage[ipc_client_index] : NULL;

        switch (signal) {
        case SIGNAL_OPEN_DOOR_B:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Unlocking building front door.");

          // Wenn der shack aktuell sicher "offen" ist, senden wir eine Nachricht an die Fronttüre:
          if (!send_mqtt_msg(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_B))) {
            log_print(LSS_SYSTEM, LL_ERROR, "Failed to send message to open B.");
          }

          break;

        case SIGNAL_OPEN_DOOR_C:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Unlocking building back door.");
          if (!send_mqtt_msg(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_C))) {
            log_print(LSS_SYSTEM, LL_ERROR, "Failed to send message to open C.");
          }
          break;

        case SIGNAL_OPEN_DOOR_B2_SAFE:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Safely opening inner front door.");
          if (!send_mqtt_msg(PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE, DOOR_NAME(DOOR_B2))) {
            log_print(LSS_SYSTEM, LL_ERROR, "Failed to send message to safely open B2");
          }
          break;

        case SIGNAL_OPEN_DOOR_C2_SAFE:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Safely opening inner back door.");
          if (!send_mqtt_msg(PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE, DOOR_NAME(DOOR_C2))) {
            log_print(LSS_SYSTEM, LL_ERROR, "Failed to send message to safely open C2");
          }
          break;

        case SIGNAL_OPEN_DOOR_B2_UNSAFE:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Opening inner front door.");
          if (!send_mqtt_msg(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_B2))) {
            log_print(LSS_SYSTEM, LL_ERROR, "Failed to send message to open B2");
          }
          break;

        case SIGNAL_OPEN_DOOR_C2_UNSAFE:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Opening inner back door.");
          if (!send_mqtt_msg(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_C2))) {
            log_print(LSS_SYSTEM, LL_ERROR, "Failed to send message to open C2");
          }
          break;

        case SIGNAL_LOCK_ALL:
        {
          bool ok;
          log_print(LSS_SYSTEM, LL_MESSAGE, "Sending request to lock all doors...");

          // Close both doors
          ok = send_mqtt_msg(PORTAL300_TOPIC_ACTION_LOCK_DOOR, DOOR_NAME(DOOR_C2));
          if (!ok) {
            log_print(LSS_SYSTEM, LL_ERROR, "Could not send message to close door C2");
            break;
          }

          ok = send_mqtt_msg(PORTAL300_TOPIC_ACTION_LOCK_DOOR, DOOR_NAME(DOOR_B2));
          if (!ok) {
            log_print(LSS_SYSTEM, LL_ERROR, "Could not send message to close door B2");
            break;
          }

          break;
        }

        case SIGNAL_UNLOCK_SUCCESSFUL:
        {
          log_print(LSS_SYSTEM, LL_MESSAGE, "Received state machine signal: SIGNAL_UNLOCK_SUCCESSFUL");

          break;
        }

        case SIGNAL_LOCK_SUCCESSFUL:
        {
          log_print(LSS_SYSTEM, LL_MESSAGE, "shack was successfully locked");
          remove_all_ipc_clients(IPC_DISCONNECT_ON_LOCKED);
          break;
        }

        case SIGNAL_OPEN_SUCCESSFUL:
        {
          log_print(LSS_SYSTEM, LL_MESSAGE, "shack was successfully unlocked");
          remove_all_ipc_clients(IPC_DISCONNECT_ON_OPEN);
          break;
        }

        case SIGNAL_CHANGE_KEYHOLDER:
        {
          if (ipc_client_valid) {
            log_print(LSS_SYSTEM, LL_MESSAGE, "Changing active keyholder to %s", ipc_client_data->nick_name);
          }
          else {
            log_print(LSS_SYSTEM, LL_MESSAGE, "Could not transfer keyholder status: Could not detect active keyholder.");
          }
          remove_all_ipc_clients(IPC_DISCONNECT_ON_OPEN);
          break;
        }

        case SIGNAL_CANNOT_HANDLE_REQUEST:
        {
          log_print(LSS_SYSTEM, LL_MESSAGE, "Could not handle user request.");

          if (ipc_client_valid) {
            int fd = pollfds[ipc_client_index].fd;
            send_ipc_info(fd, "Could not handle your request right now. Another process is still in action.");

            remove_ipc_client(ipc_client_index);
          }
          break;
        }

        case SIGNAL_UNLOCK_TIMEOUT:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Nobody entered the building, locking doors again...");
          break;

        case SIGNAL_STATE_CHANGE:
          log_print(LSS_SYSTEM, LL_MESSAGE, "shackspace is now %s", sm_shack_state_name(sm_get_shack_state(&global_state_machine)));
          break;

        case SIGNAL_NO_STATE_CHANGE:
          log_print(LSS_SYSTEM, LL_MESSAGE, "shackspace is still %s", sm_shack_state_name(sm_get_shack_state(&global_state_machine)));
          remove_all_ipc_clients(IPC_DISCONNECT_ON_NO_CHANGE);
          break;

        case SIGNAL_START_TIMEOUT:
          log_print(LSS_SYSTEM, LL_VERBOSE, "handling timeout request. timeout is in %d ms.", STATE_MACHINE_TIMEOUT_MS);
          arm_timer(sm_timerfd, true, STATE_MACHINE_TIMEOUT_MS);
          break;

        case SIGNAL_CANCEL_TIMEOUT:
          log_print(LSS_SYSTEM, LL_VERBOSE, "cancelling timeout request.");
          disarm_timer(sm_timerfd);
          break;

        case SIGNAL_USER_REQUESTED_TIMED_OUT:
          log_print(LSS_SYSTEM, LL_MESSAGE, "Requested operation timed out. Not able to lock/open shackspace!");
          remove_all_ipc_clients(IPC_DISCONNECT_ON_ERROR);
          break;
        }
      }
    }

    // sync the mqtt client to send some leftovers
    mqtt_client_sync(mqtt_client);

    update_api_status();

    int const poll_ret = poll(pollfds, pollfds_size, -1); // wait infinitly for an event
    if (poll_ret == -1) {
      if (errno != EINTR) {
        log_perror(LSS_SYSTEM, LL_ERROR, "central poll failed");
      }
      continue;
    }

    struct timespec loop_start, loop_end;
    clock_gettime(CLOCK_MONOTONIC, &loop_start);

    for (size_t pfd_index = 0; pfd_index < pollfds_size; pfd_index++) {
      const struct pollfd pfd = pollfds[pfd_index];
      if (pfd.revents != 0) {
        switch (pfd_index) {
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
                pollfds[pfd_index].fd = create_reconnect_timeout_timer(MQTT_RECONNECT_TIMEOUT);
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

              pollfds[pfd_index].fd = mqtt_client_get_socket_fd(mqtt_client);
            }
            else {
              log_print(LSS_MQTT, LL_WARNING, "failed to connect to mqtt server, retrying in %d seconds", MQTT_RECONNECT_TIMEOUT);
            }
          }
          break;
        }

        case POLLFD_SM_TIMER:
        {
          printf("ready?\n");
          if (fetch_timer_fd(pfd.fd)) {
            sm_apply_event(&global_state_machine, EVENT_TIMEOUT, NULL);
          }
          else {
            log_perror(LSS_SYSTEM, LL_ERROR, "failed to fetch state machine timerfd");
          }

          break;
        }

        // IPC client message or error
        default:
        {
          if (pfd.revents & POLLERR) {
            log_print(LSS_IPC, LL_MESSAGE, "lost IPC client on connection slot %zu", pfd_index);
            remove_ipc_client(pfd_index);
          }
          else if (pfd.revents & POLLIN) {
            struct IpcClientInfo * const ipc_client_data = &ipc_client_info_storage[pfd_index];

            struct IpcMessage msg;
            enum IpcRcvResult msg_ok = ipc_receive_msg(pfd.fd, &msg);

            switch (msg_ok) {
            case IPC_EOF:
            {
              remove_ipc_client(pfd_index);
              log_print(LSS_IPC, LL_MESSAGE, "connection to ipc socket slot %zu closed", pfd_index);
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
                ipc_client_data->forward_logs = true;
                ipc_client_data->disconnect_flags |= (IPC_DISCONNECT_ON_OPEN | IPC_DISCONNECT_ON_NO_CHANGE | IPC_DISCONNECT_ON_ERROR);

                size_t const nick_len = strnlen(msg.data.open.member_nick, IPC_MAX_NICK_LEN);
                size_t const name_len = strnlen(msg.data.open.member_name, IPC_MAX_NAME_LEN);

                if (nick_len == 0 || name_len == 0 || msg.data.open.member_id <= 0) {
                  send_ipc_info(pfd.fd, "Es wurden keine gültigen Member-Daten übertragen!");
                  remove_ipc_client(pfd_index);
                  break;
                }

                free(ipc_client_data->nick_name);
                free(ipc_client_data->full_name);

                ipc_client_data->nick_name = malloc(nick_len + 1);
                ipc_client_data->full_name = malloc(name_len + 1);

                if (ipc_client_data->nick_name == NULL) {
                  free(ipc_client_data->nick_name);
                  free(ipc_client_data->full_name);

                  send_ipc_info(pfd.fd, "Out of memory!");
                  remove_ipc_client(pfd_index);
                  break;
                }
                if (ipc_client_data->full_name == NULL) {
                  free(ipc_client_data->nick_name);
                  free(ipc_client_data->full_name);

                  send_ipc_info(pfd.fd, "Out of memory!");
                  remove_ipc_client(pfd_index);
                  break;
                }

                memcpy(ipc_client_data->nick_name, msg.data.open.member_nick, nick_len);
                memcpy(ipc_client_data->full_name, msg.data.open.member_name, name_len);
                ipc_client_data->nick_name[nick_len] = 0;
                ipc_client_data->full_name[name_len] = 0;
                ipc_client_data->member_id           = msg.data.open.member_id;

                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal opening via %s for (%d, '%.*s', '%.*s').", pfd_index, (msg.type == IPC_MSG_OPEN_BACK) ? "back door" : "front door", msg.data.open.member_id, (int)strnlen(msg.data.open.member_nick, sizeof msg.data.open.member_nick), msg.data.open.member_nick, (int)strnlen(msg.data.open.member_name, sizeof msg.data.open.member_name), msg.data.open.member_name);

                send_ipc_infof(pfd.fd, "Portal wird geöffnet, bitte warten...");

                sm_apply_event(
                    &global_state_machine,
                    (msg.type == IPC_MSG_OPEN_BACK) ? EVENT_SSH_OPEN_BACK_REQUEST : EVENT_SSH_OPEN_FRONT_REQUEST,
                    &ipc_client_data->client_id);

                // // Open outer door
                // ok = send_mqtt_msg(
                //     PORTAL300_TOPIC_ACTION_OPEN_DOOR,
                //     (msg.type == IPC_MSG_OPEN_BACK) ? DOOR_NAME(DOOR_C) : DOOR_NAME(DOOR_B));
                // if (!ok) {
                //   send_ipc_infof(pfd.fd, "Konnte Portal nicht öffnen!");
                //   remove_ipc_client(i);
                //   break;
                // }

                // // Open inner door
                // ok = send_mqtt_msg(
                //     PORTAL300_TOPIC_ACTION_OPEN_DOOR,
                //     (msg.type == IPC_MSG_OPEN_BACK) ? DOOR_NAME(DOOR_C2) : DOOR_NAME(DOOR_B2));
                // if (!ok) {
                //   send_ipc_infof(pfd.fd, "Konnte Portal nicht öffnen!");
                //   remove_ipc_client(i);
                //   break;
                // }

                break;
              }

              case IPC_MSG_CLOSE:
              {
                ipc_client_data->forward_logs = true;
                ipc_client_data->disconnect_flags |= (IPC_DISCONNECT_ON_LOCKED | IPC_DISCONNECT_ON_NO_CHANGE | IPC_DISCONNECT_ON_ERROR);

                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal close.", pfd_index);

                send_ipc_infof(pfd.fd, "Portal wird geschlossen, bitte warten...");

                sm_apply_event(
                    &global_state_machine,
                    EVENT_SSH_CLOSE_REQUEST,
                    &ipc_client_data->client_id);

                break;
              }

              case IPC_MSG_FORCE_OPEN:
              {
                mqtt_client_publish(mqtt_client, PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_B), 2);
                mqtt_client_publish(mqtt_client, PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_B2), 2);
                mqtt_client_publish(mqtt_client, PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_C), 2);
                mqtt_client_publish(mqtt_client, PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE, DOOR_NAME(DOOR_C2), 2);
                remove_ipc_client(pfd_index);
                break;
              }

              case IPC_MSG_SYSTEM_RESET:
              {
                ipc_client_data->forward_logs = true;
                log_print(LSS_IPC, LL_MESSAGE, "Starting system reset!");
                mqtt_client_publish(mqtt_client, PORTAL300_TOPIC_ACTION_RESET, "*", 2);
              }

              case IPC_MSG_SHUTDOWN:
              {
                ipc_client_data->forward_logs = true;

                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal shutdown.", pfd_index);

                send_ipc_infof(pfd.fd, "Shutdown wird zur Zeit noch nicht unterstützt...");
                remove_ipc_client(pfd_index);

                break;
              }

              case IPC_MSG_QUERY_STATUS:
              {
                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal status.", pfd_index);

                (void)send_ipc_infof(pfd.fd, "Portal-Status:");
                (void)send_ipc_infof(pfd.fd, "  Space Status:    %s", sm_shack_state_name(sm_get_shack_state(&global_state_machine)));
                (void)send_ipc_infof(pfd.fd, "  Aktivität:       %s", sm_state_name(&global_state_machine));
                (void)send_ipc_infof(pfd.fd, "  MQTT:            %s", mqtt_client_is_connected(mqtt_client) ? "Verbunden" : "Nicht verbunden");
                (void)send_ipc_infof(pfd.fd, "  IPC Clients:     %u", (pollfds_size - POLLFD_FIRST_IPC));
                (void)send_ipc_infof(pfd.fd, "Tür-Status:");
                (void)send_ipc_infof(pfd.fd, "  B2:              %s", sm_door_state_name(global_state_machine.door_b2)); // geöffnet, geschlossen
                (void)send_ipc_infof(pfd.fd, "  C2:              %s", sm_door_state_name(global_state_machine.door_c2)); // geöffnet, geschlossen
                (void)send_ipc_infof(pfd.fd, "Geräte-Status:");
                (void)send_ipc_infof(pfd.fd, "  ssh_interface:   %s", device_status.ssh_interface ? "online" : "offline");
                (void)send_ipc_infof(pfd.fd, "  door_control_b2: %s", device_status.door_control_b2 ? "online" : "offline");
                (void)send_ipc_infof(pfd.fd, "  door_control_c2: %s", device_status.door_control_c2 ? "online" : "offline");
                (void)send_ipc_infof(pfd.fd, "  busch_interface: %s", device_status.busch_interface ? "online" : "offline");

                // after a status message, we can just drop the client connection
                remove_ipc_client(pfd_index);
                break;
              }

              case IPC_MSG_SIMPLE_STATUS:
              {
                log_print(LSS_IPC, LL_MESSAGE, "client %zu requested simple portal status.", pfd_index);

                (void)send_ipc_infof(pfd.fd, "%s\n", sm_shack_state_name(sm_get_shack_state(&global_state_machine)));

                // after a status message, we can just drop the client connection
                remove_ipc_client(pfd_index);
                break;
              }

              default:
              {
                // Invalid message received. Print error message and kick the client
                log_print(LSS_IPC, LL_WARNING, "received invalid ipc message of type %u", msg.type);
                remove_ipc_client(pfd_index);
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

#define MAX_SIGNAL_RINGBUFFER_ITEMS 64
static struct
{
  uint32_t       read_offset, size;
  enum SM_Signal signals[MAX_SIGNAL_RINGBUFFER_ITEMS];
  uint32_t       client_ids[MAX_SIGNAL_RINGBUFFER_ITEMS];
} signal_ring_buffer = {
    .read_offset = 0,
    .size        = 0,
};

static bool push_signal(enum SM_Signal sig, uint32_t client_id)
{
  if (signal_ring_buffer.size >= MAX_SIGNAL_RINGBUFFER_ITEMS)
    return false;

  size_t write_index                         = (signal_ring_buffer.read_offset + signal_ring_buffer.size) % MAX_SIGNAL_RINGBUFFER_ITEMS;
  signal_ring_buffer.signals[write_index]    = sig;
  signal_ring_buffer.client_ids[write_index] = client_id;
  signal_ring_buffer.size += 1;

  return true;
}

static bool pop_signal(enum SM_Signal * sig, uint32_t * client_id)
{
  assert(sig != NULL);
  assert(client_id != NULL);
  if (signal_ring_buffer.size == 0)
    return false;

  *sig       = signal_ring_buffer.signals[signal_ring_buffer.read_offset];
  *client_id = signal_ring_buffer.client_ids[signal_ring_buffer.read_offset];

  signal_ring_buffer.size -= 1;
  signal_ring_buffer.read_offset += 1;
  signal_ring_buffer.read_offset %= MAX_SIGNAL_RINGBUFFER_ITEMS;

  return true;
}

void state_machine_signal_handler(void * user_data, void * context, enum SM_Signal signal)
{
  (void)user_data; // is always NULL anyways

  uint32_t client_id;
  if (context != NULL) {
    client_id = *(uint32_t *)context;
  }
  else {
    client_id = INVALID_IPC_CLIENT;
  }

  if (!push_signal(signal, client_id)) {
    log_print(LSS_SYSTEM, LL_ERROR, "Could not handle signal from state machine: ring buffer full!");
  }
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

  log_print(LSS_SYSTEM, LL_VERBOSE, "Sending mqtt message '%s': %s", topic, data);

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

static bool streq(char const * a, char const * b)
{
  return (strcmp(a, b) == 0);
}

static bool parse_system_status(char const * status)
{
  if (streq(status, PORTAL300_STATUS_SYSTEM_ONLINE))
    return true;
  if (streq(status, PORTAL300_STATUS_SYSTEM_OFFLINE))
    return false;
  log_print(LSS_SYSTEM, LL_WARNING, "Received invalid system status: '%s'", status);
  return false;
}

//! Handles all incoming MQTT messages
static void mqtt_handle_message(void * user_data, char const * topic, char const * data)
{
  (void)user_data;

  log_print(LSS_SYSTEM, LL_VERBOSE, "Received mqtt message '%s': %s", topic, data);

  if (streq(topic, PORTAL300_TOPIC_EVENT_DOORBELL)) {
    sm_apply_event(&global_state_machine, EVENT_DOORBELL_FRONT, NULL);
  }
  else if (streq(topic, PORTAL300_TOPIC_STATUS_DOOR_B2)) {
    if (streq(data, PORTAL300_STATUS_DOOR_LOCKED))
      sm_apply_event(&global_state_machine, EVENT_DOOR_B2_LOCKED, NULL);
    else if (streq(data, PORTAL300_STATUS_DOOR_CLOSED))
      sm_apply_event(&global_state_machine, EVENT_DOOR_B2_CLOSED, NULL);
    else if (streq(data, PORTAL300_STATUS_DOOR_OPENED))
      sm_apply_event(&global_state_machine, EVENT_DOOR_B2_OPENED, NULL);
    else
      log_print(LSS_SYSTEM, LL_WARNING, "door B2 status update sent invalid status: %s", data);
  }
  else if (streq(topic, PORTAL300_TOPIC_STATUS_DOOR_C2)) {
    if (streq(data, PORTAL300_STATUS_DOOR_LOCKED))
      sm_apply_event(&global_state_machine, EVENT_DOOR_C2_LOCKED, NULL);
    else if (streq(data, PORTAL300_STATUS_DOOR_CLOSED))
      sm_apply_event(&global_state_machine, EVENT_DOOR_C2_CLOSED, NULL);
    else if (streq(data, PORTAL300_STATUS_DOOR_OPENED))
      sm_apply_event(&global_state_machine, EVENT_DOOR_C2_OPENED, NULL);
    else
      log_print(LSS_SYSTEM, LL_WARNING, "door C2 status update sent invalid status: %s", data);
  }
  else if (streq(topic, PORTAL300_TOPIC_STATUS_SSH_INTERFACE)) {
    device_status.ssh_interface = parse_system_status(data);
    log_print(LSS_SYSTEM, LL_MESSAGE, "device 'ssh interface' is now %s", device_status.ssh_interface ? "online" : "offline");
  }
  else if (streq(topic, PORTAL300_TOPIC_STATUS_DOOR_CONTROL_B2)) {
    device_status.door_control_b2 = parse_system_status(data);
    log_print(LSS_SYSTEM, LL_MESSAGE, "device 'door control b2' is now %s", device_status.door_control_b2 ? "online" : "offline");
  }
  else if (streq(topic, PORTAL300_TOPIC_STATUS_DOOR_CONTROL_C2)) {
    device_status.door_control_c2 = parse_system_status(data);
    log_print(LSS_SYSTEM, LL_MESSAGE, "device 'door control c2' is now %s", device_status.door_control_c2 ? "online" : "offline");
  }
  else if (streq(topic, PORTAL300_TOPIC_STATUS_BUSCH_INTERFACE)) {
    device_status.busch_interface = parse_system_status(data);
    log_print(LSS_SYSTEM, LL_MESSAGE, "device 'busch interface' is now %s", device_status.busch_interface ? "online" : "offline");
  }
  else if (streq(topic, PORTAL300_TOPIC_EVENT_BUTTON)) {
    if (streq(data, DOOR_NAME(DOOR_B2)))
      sm_apply_event(&global_state_machine, EVENT_BUTTON_B2, NULL);
    else if (streq(data, DOOR_NAME(DOOR_C2)))
      sm_apply_event(&global_state_machine, EVENT_BUTTON_C2, NULL);
    else
      log_print(LSS_SYSTEM, LL_WARNING, "Received button event for unhandled door %s", data);
  }
  else if (streq(topic, PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE)) {
    // Silently ignore message
  }
  else if (streq(topic, PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE)) {
    // Silently ignore message
  }
  else if (streq(topic, PORTAL300_TOPIC_ACTION_LOCK_DOOR)) {
    // Silently ignore message
  }
  else if (strstr(topic, PORTAL300_TOPIC_PREFIX) == topic) {
    // we only need to manage topics for portal300
    log_print(LSS_SYSTEM, LL_WARNING, "Received data for unhandled topic '%s': %s", topic, data);
  }
  else if (strstr(topic, PORTAL300_TOPIC_ACTION_RESET) == topic) {
    log_print(LSS_SYSTEM, LL_WARNING, "Received global system reset. Shutting down!");
    exit(1);
  }
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

static bool configure_timerfd(int timer, bool oneshot, uint32_t ms)
{
  assert(timer != -1);

  const struct timespec given_ms = {
      .tv_sec  = ms / 1000,
      .tv_nsec = 1000000 * (ms % 1000),
  };

  const struct timespec null_time = {
      .tv_sec  = 0,
      .tv_nsec = 0,
  };

  struct itimerspec timeout;
  if (oneshot) {
    timeout = (struct itimerspec){
        .it_value    = given_ms,
        .it_interval = null_time,
    };
  }
  else {
    timeout = (struct itimerspec){
        .it_value    = given_ms,
        .it_interval = given_ms,
    };
  }

  if (timerfd_settime(timer, 0, &timeout, NULL) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to arm timerfd");
    log_print(LSS_SYSTEM, LL_ERROR, "destroying daemon, hoping for restart...");
    exit(EXIT_FAILURE);
  }

  return timer;
}

static bool disarm_timer(int timer)
{
  return configure_timerfd(timer, false, 0);
}

static bool arm_timer(int timer, bool oneshot, uint32_t ms)
{
  assert(ms > 0);
  return configure_timerfd(timer, oneshot, ms);
}

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

  ipc_client_info_storage[index] = (struct IpcClientInfo){
      .client_id        = fetch_next_client_id(),
      .disconnect_flags = 0,
      .forward_logs     = false,

      .nick_name = NULL,
      .full_name = NULL,
      .member_id = -1,
  };

  return index;
}

static void remove_all_ipc_clients(uint32_t disconnect_flags)
{
  size_t i = POLLFD_FIRST_IPC;
  while (i < pollfds_size) {
    if (ipc_client_info_storage[i].disconnect_flags & disconnect_flags) {
      remove_ipc_client(i);
    }
    else {
      i += 1;
    }
  }
}

static void remove_ipc_client(size_t index)
{
  assert(index >= POLLFD_FIRST_IPC);
  assert(index < pollfds_size);

  // close the socket when we remove a client connection
  if (close(pollfds[index].fd) == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to close ipc client");
  }

  free(ipc_client_info_storage[index].nick_name);
  free(ipc_client_info_storage[index].full_name);

  // swap-remove with the last index
  // NOTE: This doesn't hurt us when (index == pollfds_size-1), as we're gonna wipe the element then anyways
  pollfds[index]                 = pollfds[pollfds_size - 1];
  ipc_client_info_storage[index] = ipc_client_info_storage[pollfds_size - 1];

  pollfds_size -= 1;
  memset(&pollfds[pollfds_size], 0xAA, sizeof(struct pollfd));
  memset(&ipc_client_info_storage[pollfds_size], 0xAA, sizeof(struct IpcClientInfo));
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
      .help               = false,
      .host_name          = "mqtt.portal.shackspace.de",
      .port               = 8883,
      .ca_cert_file       = NULL,
      .client_key_file    = NULL,
      .client_crt_file    = NULL,
      .serial_device_name = "/dev/portal-status",
  };

  {
    int opt;
    while ((opt = getopt(argc, argv, "hH:p:k:c:C:vP:")) != -1) {
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

      case 'P':
      {
        args->serial_device_name = strdup(optarg);
        if (args->serial_device_name == NULL) {
          panic("out of memory");
        }
        break;
      }

      case 'v':
      { // verbose
        log_set_level(LSS_IPC, LL_VERBOSE);
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

static uint32_t find_ipc_client_by_id(uint32_t client_id)
{
  for (size_t i = POLLFD_FIRST_IPC; i < pollfds_size; i++) {
    if (ipc_client_info_storage[i].client_id == client_id) {
      return i;
    }
  }
  return INVALID_IPC_CLIENT;
}

//! Stores the next valid ipc client id. All values are valid except for
//! `INVALID_IPC_CLIENT`. Assumption made: Not more than `intMax(uint32_t)-1`
//! clients are connected at the same time which should be possible.
static uint32_t next_ipc_client_id = 0;

static uint32_t fetch_next_client_id(void)
{
  uint32_t next = next_ipc_client_id;

_increment_id:
  next_ipc_client_id += 1;
  if (next_ipc_client_id == INVALID_IPC_CLIENT) {
    next_ipc_client_id += 1;
  }

  // search through the array of all active clients.
  // If the next ID is still in use, continue incrementing until we can be safe that the
  // client id is not used.
  for (size_t i = POLLFD_FIRST_IPC; i < pollfds_size; i++) {
    if (ipc_client_info_storage[i].client_id == next_ipc_client_id) {
      goto _increment_id;
    }
  }

  return next;
}

void print_log_to_ipc_clients(void * user_data, enum LogSubSystem subsystem, enum LogLevel level, char const * msg)
{
  (void)user_data; // we don't need that

  for (size_t i = POLLFD_FIRST_IPC; i < pollfds_size; i++) {
    if (ipc_client_info_storage[i].forward_logs) {
      if (subsystem == LSS_SYSTEM && level == LL_MESSAGE) {
        send_ipc_info(pollfds[i].fd, msg);
      }
      else {
        send_ipc_infof(
            pollfds[i].fd,
            "[%s] [%s] %s",
            log_get_level_name(level),
            log_get_subsystem_name(subsystem),
            msg);
      }
    }
  }
}

static void close_sm_timerfd(void)
{
  if (close(sm_timerfd) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to destroy state machine timerfd");
  }
  sm_timerfd = -1;
}

// inspired by https://stackoverflow.com/a/6947758
static bool configure_serial_port(int fd, int baud_rate)
{
  struct termios tty;
  if (tcgetattr(fd, &tty) != 0) {
    log_perror(LSS_API, LL_ERROR, "failed to get serial port configuration");
    return false;
  }

  cfsetospeed(&tty, baud_rate);
  cfsetispeed(&tty, baud_rate);

  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
  // disable IGNBRK for mismatched speed tests; otherwise receive break
  // as \000 chars
  tty.c_iflag &= ~IGNBRK; // disable break processing
  tty.c_lflag = 0;        // no signaling chars, no echo,
                          // no canonical processing
  tty.c_oflag     = 0;    // no remapping, no delays
  tty.c_cc[VMIN]  = 0;    // read doesn't block
  tty.c_cc[VTIME] = 5;    // 0.5 seconds read timeout

  tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

  tty.c_cflag |= (CLOCAL | CREAD);   // ignore modem controls,
                                     // enable reading
  tty.c_cflag &= ~(PARENB | PARODD); // shut off parity
  // tty.c_cflag |= parity;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    log_perror(LSS_API, LL_ERROR, "failed to set serial port configuration");
    return false;
  }
  return true;
}

#define PORTAL_SIGNAL_OPEN   0x12 // DC2
#define PORTAL_SIGNAL_CLOSED 0x14 // DC4

static void update_api_status(void)
{
  if (cli.serial_device_name == NULL) {
    return;
  }

  int const device = open(cli.serial_device_name, O_RDWR);
  if (device == -1) {
    log_perror(LSS_API, LL_ERROR, "failed to open serial status device");
    return;
  }

  if (!configure_serial_port(device, B115200)) {
    // already logged the error inside the func
    return;
  }

  bool const is_open = (sm_get_shack_state(&global_state_machine) == SHACK_OPEN);

  uint8_t const msg = is_open ? PORTAL_SIGNAL_OPEN : PORTAL_SIGNAL_CLOSED;

  if (write(device, &msg, 1) != 1) {
    log_perror(LSS_API, LL_ERROR, "failed to write status");
  }

  if (close(device) == -1) {
    log_perror(LSS_API, LL_ERROR, "failed to close serial status device");
  }
}

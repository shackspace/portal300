#include "ipc.h"
#include "log.h"
#include "mqtt-client.h"
#include "state-machine.h"
#include "gpio.h"

#include <bits/types/struct_itimerspec.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <time.h>

#include <mqtt.h>

// Configuration:

#define MQTT_RECONNECT_TIMEOUT 5 // seconds

#define PORTAL_GPIO_CHIP "/dev/gpiochip0"
#define PORTAL_GPIO_LOCKED          4 // GPIO  4, Wiring 8, Pin 3
#define PORTAL_GPIO_CLOSED         22 // GPIO 22, Wiring 3, Pin 15
#define PORTAL_GPIO_BUTTON          9 // GPIO  9, Wiring 13, Pin 21
#define PORTAL_GPIO_TRIGGER_CLOSE  13 // GPIO 13, Wiring 23, Pin 33
#define PORTAL_GPIO_TRIGGER_OPEN   19 // GPIO 19, Wiring 24, Pin 35
#define PORTAL_GPIO_ACUSTIC_SIGNAL 26 // GPIO 26, Wiring 25, Pin 37

// Globals:

struct CliOptions {
  char const * host_name;
  int port;
  char const * ca_cert_file;
  char const * client_key_file;
  char const * client_crt_file;
};

struct GpioDefs {
  // inputs
  struct GpioPin locked;
  struct GpioPin closed;
  struct GpioPin button;

  // outputs:
  struct GpioPin trigger_close; 
  struct GpioPin trigger_open;  
  struct GpioPin acustic_signal;
};

static volatile sig_atomic_t shutdown_requested = 0;

static int ipc_sock = -1;

static struct MqttClient * mqtt_client = NULL;

#define POLLFD_IPC          0 // well defined fd, always the unix socket for IPC 
#define POLLFD_MQTT         1 // well defined fd, either the timerfd for reconnecting MQTT or the socket for MQTT communications
#define POLLFD_GPIO_LOCKED  2 // well defined fd, polls for inputs
#define POLLFD_GPIO_CLOSED  3 // well defined fd, polls for inputs
#define POLLFD_GPIO_BUTTON  4 // well defined fd, polls for inputs
#define POLLFD_SM_TIMEOUT   5 // well defined fd, is a timerfd that will trigger when the state machine should receive a timeout
#define POLLFD_FIRST_IPC    6 // First ipc client socket slot
#define POLLFD_LIMIT       32 // number of maximum socket connections

//! Stack array of pollfds. Everything below POLLFD_FIRST_IPC is pre-intialized and has a static purpose
//! while everything at POLLFD_FIRST_IPC till POLLFD_LIMIT is a dynamic stack of pollfds for ipc client connections.
static struct pollfd pollfds[POLLFD_LIMIT];
static size_t pollfds_size = POLLFD_FIRST_IPC;

static int sm_timeout_fd = -1;

//! Index of an invalid IPC client
static const size_t INVALID_IPC_CLIENT = ~0U;

struct GpioHandle gpio_core;

static struct GpioDefs gpio = {0};

static void close_ipc_sock(void);
static void close_mqtt_client(void);
static void close_sm_timeout_fd(void);

static void sigint_handler(int sig, siginfo_t *info, void *ucontext);
static void sigterm_handler(int sig, siginfo_t *info, void *ucontext);

static size_t add_ipc_client(int fd);
static void remove_ipc_client(size_t index);

static bool try_connect_mqtt(void);
static bool install_signal_handlers(void);
static int create_reconnect_timeout_timer(int secs);

static bool send_ipc_info(int fd, char const * text);
static bool send_ipc_infof(int fd, char const * fmt, ...) __attribute__ ((format (printf, 2, 3)));

static bool create_and_open_gpio(struct GpioPin * gpio, struct GpioConfig config, char const * label, int identifier);

static void close_all_gpios(void);

static void statemachine_handle_signal(struct StateMachine *sm, enum PortalSignal signal);
static void statemachine_handle_setTimeout(struct StateMachine *sm, uint32_t ms);
static void statemachine_handle_setIo(struct StateMachine *sm, enum PortalIo io, bool state);

static bool update_state_machine_io(struct StateMachine * sm);

static bool fetch_timer_fd(int fd);

static bool disarm_timer(int timer);
static bool arm_timer(int timer, bool oneshot, uint32_t ms);

static enum DoorState fetch_door_state(void);

int main(int argc, char **argv) {
  // Initialize libraries and dependencies:

  if(!install_signal_handlers()) {
    log_print(LSS_SYSTEM, LL_ERROR, "failed to install signal handlers.");
    exit(EXIT_FAILURE);
  }

  sm_timeout_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if(sm_timeout_fd == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to create sm_timeout_fd");
    return EXIT_FAILURE;
  }
  atexit(close_sm_timeout_fd);

  pollfds[POLLFD_SM_TIMEOUT] = (struct pollfd) {
    .fd = sm_timeout_fd,
    .events = POLLIN,
    .revents = 0,
  };

  if(!mqtt_client_init()) {
    log_write(LSS_MQTT, LL_ERROR, "failed to initialize mqtt client library. aborting.");
    return EXIT_FAILURE;
  }

  // create GPIO management types and open GPIOs
  {
    if(!gpio_open(&gpio_core, PORTAL_GPIO_CHIP)) {
      return EXIT_FAILURE;
    }
    atexit(close_all_gpios);


    static const struct GpioConfig regular_input = {
      .direction = GPIO_DIR_IN,
      .edge = GPIO_EDGE_BOTH,
      .bias = GPIO_BIAS_PULL_UP,
      .drive = GPIO_DRIVE_DEFAULT,
      .inverted = true,
      .debounce_us = 1000, // 1ms debouncing
    };
    static const struct GpioConfig regular_output = {
      .direction = GPIO_DIR_OUT,
      .edge = GPIO_EDGE_NONE,
      .bias = GPIO_BIAS_DEFAULT,
      .drive = GPIO_DRIVE_DEFAULT, // push pull
      .inverted = false,
    };

    if(!create_and_open_gpio(&gpio.locked, regular_input, "portal.locked", PORTAL_GPIO_LOCKED)) {
      log_write(LSS_GPIO, LL_ERROR, "failed to open gpio 'locked'.");
      return EXIT_FAILURE;
    }
    if(!create_and_open_gpio(&gpio.closed, regular_input, "portal.closed", PORTAL_GPIO_CLOSED)) {
      log_write(LSS_GPIO, LL_ERROR, "failed to open gpio 'closed'.");
      return EXIT_FAILURE;
    }
    if(!create_and_open_gpio(&gpio.button, regular_input, "portal.button", PORTAL_GPIO_BUTTON)) {
      log_write(LSS_GPIO, LL_ERROR, "failed to open gpio 'button'.");
      return EXIT_FAILURE;
    }
    if(!create_and_open_gpio(&gpio.trigger_close, regular_output, "portal.trigger_close", PORTAL_GPIO_TRIGGER_CLOSE)) {
      log_write(LSS_GPIO, LL_ERROR, "failed to open gpio 'trigger_close'.");
      return EXIT_FAILURE;
    }
    if(!create_and_open_gpio(&gpio.trigger_open, regular_output, "portal.trigger_open", PORTAL_GPIO_TRIGGER_OPEN)) {
      log_write(LSS_GPIO, LL_ERROR, "failed to open gpio 'trigger_open'.");
      return EXIT_FAILURE;
    }
    if(!create_and_open_gpio(&gpio.acustic_signal, regular_output, "portal.acustic_signal", PORTAL_GPIO_ACUSTIC_SIGNAL)) {
      log_write(LSS_GPIO, LL_ERROR, "failed to open gpio 'acustic_signal'.");
      return EXIT_FAILURE;
    }

    pollfds[POLLFD_GPIO_LOCKED] = (struct pollfd) {
      .fd = gpio_fd(gpio.locked),
      .events = POLLIN | POLLERR | POLLOUT,
      .revents = 0,
    };
    if(pollfds[POLLFD_GPIO_LOCKED].fd == -1) {
      log_write(LSS_GPIO, LL_ERROR, "failed to get fd for gpio locked.");
      return EXIT_FAILURE;
    }

    pollfds[POLLFD_GPIO_CLOSED] = (struct pollfd) {
      .fd = gpio_fd(gpio.closed),
      .events = POLLIN | POLLERR | POLLOUT,
      .revents = 0,
    };
    if(pollfds[POLLFD_GPIO_CLOSED].fd == -1) {
      log_write(LSS_GPIO, LL_ERROR, "failed to get fd for gpio closed.");
      return EXIT_FAILURE;
    }

    pollfds[POLLFD_GPIO_BUTTON] = (struct pollfd) {
      .fd = gpio_fd(gpio.button),
      .events = POLLIN | POLLERR | POLLOUT,
      .revents = 0,
    };
    if(pollfds[POLLFD_GPIO_BUTTON].fd == -1) {
      log_write(LSS_GPIO, LL_ERROR, "failed to get fd for gpio button.");
      return EXIT_FAILURE;
    }
  }

  if(gpio.locked.io == NULL
     || gpio.closed.io == NULL 
     || gpio.button.io == NULL 
     || gpio.trigger_close.io == NULL 
     || gpio.trigger_open.io == NULL 
     || gpio.acustic_signal.io == NULL) {
    log_write(LSS_GPIO, LL_ERROR, "failed to create GPIO objects.");
    return EXIT_FAILURE;
  }

  (void)argc;
  (void)argv;
  struct CliOptions const cli = {
    .host_name = "mqtt.portal",
    .port = 8883,
    .ca_cert_file = "debug/ca.crt",
    .client_key_file = "debug/client.key",
    .client_crt_file = "debug/client.crt",
  };

  // Create MQTT client from CLI info
  mqtt_client = mqtt_client_create(
    cli.host_name,
    cli.port,
    cli.ca_cert_file,
    cli.client_key_file,
    cli.client_crt_file
  );
  if(mqtt_client == NULL) {
    log_write(LSS_MQTT, LL_ERROR, "failed to create mqtt client.");
    return EXIT_FAILURE;
  }
  atexit(close_mqtt_client);
  
  ipc_sock = ipc_create_socket();
  if (ipc_sock == -1) {
    return EXIT_FAILURE;
  }

  if(try_connect_mqtt())
  {
    // we successfully connected to MQTT, set up the poll entry for MQTT action:
    pollfds[POLLFD_MQTT] = (struct pollfd) {
      .fd = mqtt_client_get_socket_fd(mqtt_client),
      .events = POLLIN,
      .revents = 0,
    };
  }
  else
  {
    log_print(LSS_MQTT, LL_WARNING, "failed to connect to mqtt server, retrying in %d seconds", MQTT_RECONNECT_TIMEOUT);

    // we failed to connect to MQTT, set up a timerfd to retry in some seconds
    int timer = create_reconnect_timeout_timer(MQTT_RECONNECT_TIMEOUT);
 
    pollfds[POLLFD_MQTT] = (struct pollfd) {
      .fd = timer,
      .events = POLLIN,
      .revents = 0,
    };
  }

  // Bind and setup the ipc socket, so we can receive ipc messages 
  {
    if(bind(ipc_sock, (struct sockaddr const *) &ipc_socket_address, sizeof ipc_socket_address) == -1) {
      log_perror(LSS_IPC, LL_ERROR, "failed to bind ipc socket");
      log_print(LSS_IPC, LL_ERROR, "is another instance of this daemon already running?");
      return EXIT_FAILURE;
    }
    atexit(close_ipc_sock);

    if(listen(ipc_sock, 0) == -1) {
      log_perror(LSS_IPC, LL_ERROR, "failed to listen on ipc socket");
      return EXIT_FAILURE;
    }
    pollfds[POLLFD_IPC] = (struct pollfd) {
      .fd = ipc_sock,
      .events = POLLIN,
      .revents = 0,
    };
  }

  enum DoorState initial_door_state = fetch_door_state();

  struct StateMachine portal_state;

  sm_init(&portal_state, initial_door_state, statemachine_handle_signal, statemachine_handle_setTimeout, statemachine_handle_setIo, NULL);
  
  if(!update_state_machine_io(&portal_state)) {
    log_write(LSS_GPIO, LL_ERROR, "failed to get initial state of GPIO pins!");
    return EXIT_FAILURE;
  }

  // TODO: Set sane state 
  gpio_write(gpio.acustic_signal, 0);
  gpio_write(gpio.trigger_close, 0);
  gpio_write(gpio.trigger_open, 0);

  while(shutdown_requested == false) {
    int const poll_ret = poll(pollfds, pollfds_size, -1); // wait infinitly for an event
    if(poll_ret == -1) {
      if(errno != EINTR) {
        log_perror(LSS_SYSTEM, LL_ERROR, "central poll failed");
      }
      continue;
    }

    struct timespec loop_start, loop_end;
    clock_gettime(CLOCK_MONOTONIC, &loop_start);

    for(size_t i = 0; i < pollfds_size; i++) {
      const struct pollfd pfd = pollfds[i];
      if(pfd.revents != 0) {
        switch(i) {
          // this is the IPC listener. accept clients here
          case POLLFD_IPC: {
            assert(pfd.fd == ipc_sock);

            int client_fd = accept(ipc_sock, NULL, NULL);
            if(client_fd != -1) {
              size_t const index = add_ipc_client(client_fd);
              if(index != INVALID_IPC_CLIENT) {
                 log_print(LSS_IPC, LL_MESSAGE, "accepted new IPC client on connection slot %zu", index);
              }
              else {
                if(close(client_fd) == -1) {
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
          case POLLFD_MQTT: {
            if(mqtt_client_is_connected(mqtt_client)) {
              // we're connected, so the fd is the mqtt socket.
              // process messages and handle errors here.

              if(!mqtt_client_sync(mqtt_client)) {
                if(mqtt_client_is_connected(mqtt_client)) {
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
              if(!fetch_timer_fd(pfd.fd)) {
                log_perror(LSS_MQTT, LL_ERROR, "failed to read from timerfd");
                log_write(LSS_MQTT, LL_ERROR, "destroying daemon, hoping for restart...");
                exit(EXIT_FAILURE);
              }

              if(try_connect_mqtt()) {
                log_write(LSS_MQTT, LL_MESSAGE, "successfully reconnected to mqtt server.");

                if(close(pfd.fd) == -1) {
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

          // GPIO events
          case POLLFD_GPIO_LOCKED:
          case POLLFD_GPIO_CLOSED:
          case POLLFD_GPIO_BUTTON: {
            if(i == POLLFD_GPIO_LOCKED) {
              enum GpioEdge edge;
              if(!gpio_get_event(gpio.locked, &edge)) {
                log_write(LSS_GPIO, LL_ERROR, "failed to read event for gpio locked");
              }
            }
            else if(i == POLLFD_GPIO_CLOSED) {
              enum GpioEdge edge;
              if(!gpio_get_event(gpio.closed, &edge)) {
                log_write(LSS_GPIO, LL_ERROR, "failed to read event for gpio closed");
              }
            }
            else if(i == POLLFD_GPIO_BUTTON) {
              enum GpioEdge edge;
              if(!gpio_get_event(gpio.button, &edge)) {
                log_write(LSS_GPIO, LL_ERROR, "failed to read event for gpio button");
              }
            }
            else {
              // unknown GPIO input event, please fix
              __builtin_unreachable();
            }

            if(!update_state_machine_io(&portal_state)) {
              log_write(LSS_GPIO, LL_ERROR, "failed to get initial state of GPIO pins!");
              return EXIT_FAILURE;
            }

            bool button_state;
            if(gpio_read(gpio.button, &button_state)) {

              log_print(LSS_GPIO, LL_VERBOSE, "button = %s", button_state ? "pressed" : "released");

              if(button_state) {
                enum PortalError err = sm_send_event(&portal_state, EVENT_CLOSE);
                if(err == SM_SUCCESS) {
                  log_write(LSS_GPIO, LL_MESSAGE, "Triggering physical closing by button. Portal is now closing...");
                }
                else {
                  log_print(LSS_GPIO, LL_ERROR, "Failed to close portal via physical button: error_code=%u", err);
                }
              }

            }
            else {
              log_write(LSS_GPIO, LL_ERROR, "failed to query button state");
            }
            break;
          }

          // StateMachine requested timeout arrived, forward it. 
          case POLLFD_SM_TIMEOUT: {
            
            if(!fetch_timer_fd(sm_timeout_fd)) {
              log_write(LSS_LOGIC, LL_ERROR, "failed to fetch data from state machine."); 
              log_write(LSS_LOGIC, LL_ERROR, "destroying daemon, hoping for restart...");
              exit(EXIT_FAILURE);
            }

            enum PortalError err = sm_send_event(&portal_state, EVENT_TIMEOUT);
            switch(err) {
              // good case:
              case SM_SUCCESS: {
                break;
              }

              // this should not happen, log it when it does
              case SM_ERR_IN_PROGRESS: {
                log_write(LSS_LOGIC, LL_ERROR, "unexpected error from sm_send_event(EVENT_TIMEOUT): in progress.");
                break;
              }
              
              case SM_ERR_UNEXPECTED: {
                log_write(LSS_LOGIC, LL_ERROR, "timerfd triggered while state machine did not expect a EVENT_TIMEOUT. Did some race condition happen?"); 
                break;
              }
            }

            break;
          }

          // IPC client message or error
          default: {
            if(pfd.revents & POLLERR) {
              log_print(LSS_IPC, LL_MESSAGE, "lost IPC client on connection slot %zu", i);
              remove_ipc_client(i);
            }
            else if(pfd.revents & POLLIN) {
              struct IpcMessage msg;

              enum IpcRcvResult msg_ok = ipc_receive_msg(pfd.fd, &msg);

              switch(msg_ok) {
                case IPC_EOF: {
                  remove_ipc_client(i);
                  log_print(LSS_IPC, LL_MESSAGE, "connection to ipc socket slot %zu closed", i);
                  break;
                }
                case IPC_ERROR: {
                  // we already printed an error message, just try again in the next loop
                  break;
                }
                case IPC_SUCCESS: {
                  switch(msg.type) {
                    case IPC_MSG_OPEN: {
                      log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal opening for (%d, '%.*s', '%.*s').", 
                        i,
                        msg.data.open.member_id,
                        (int)strnlen(msg.data.open.member_nick, sizeof msg.data.open.member_nick),
                        msg.data.open.member_nick,
                        (int)strnlen(msg.data.open.member_name, sizeof msg.data.open.member_name),
                        msg.data.open.member_name
                      );

                      enum PortalError err = sm_send_event(&portal_state, EVENT_OPEN);
                      if(err != SM_SUCCESS) {
                        send_ipc_infof(pfd.fd, "Konnte Portal nicht öffnen:");
                        switch(err) {
                          case SM_ERR_IN_PROGRESS: send_ipc_infof(pfd.fd, "Eine Portal-Aktion ist grade in Bearbeitung. Bitte spreche dich mit dem Keyholder an der anderen Türe ab."); break;
                          case SM_ERR_UNEXPECTED:  send_ipc_infof(pfd.fd, "Die Anfrage ÖFFNEN kann aktuell nicht verarbeitet werden."); break;
                          default:                 send_ipc_infof(pfd.fd, "Ein unbekannter Fehler trat auf: ErrorCode=%d.", err); break;
                        }
                        remove_ipc_client(i);
                        break;
                      }

                      send_ipc_infof(pfd.fd, "Portal wird geöffnet, bitte warten...");

                      break;
                    }

                    case IPC_MSG_CLOSE: {
                      log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal close.", i);
                      
                      enum PortalError err = sm_send_event(&portal_state, EVENT_CLOSE);
                      if(err != SM_SUCCESS) {
                        send_ipc_infof(pfd.fd, "Konnte Portal nicht schließen:");
                        switch(err) {
                          case SM_ERR_IN_PROGRESS: send_ipc_infof(pfd.fd, "Eine Portal-Aktion ist grade in Bearbeitung. Bitte spreche dich mit dem Keyholder an der anderen Türe ab."); break;
                          case SM_ERR_UNEXPECTED:  send_ipc_infof(pfd.fd, "Die Anfrage SCHLIESSEN kann aktuell nicht verarbeitet werden."); break;
                          default:                 send_ipc_infof(pfd.fd, "Ein unbekannter Fehler trat auf: ErrorCode=%d.", err); break;
                        }
                        remove_ipc_client(i);
                        break;
                      }

                      send_ipc_infof(pfd.fd, "Portal wird geschlossen, bitte warten...");

                      break;
                    }

                    case IPC_MSG_SHUTDOWN: {
                      log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal shutdown.", i);

                      send_ipc_infof(pfd.fd, "Shutdown wird zur Zeit noch nicht unterstützt...");
                      remove_ipc_client(i);

                      break;
                    }

                    case IPC_MSG_QUERY_STATUS: {
                      log_print(LSS_IPC, LL_MESSAGE, "client %zu requested portal status.", i);

                      (void)send_ipc_infof(pfd.fd, "Portal-Status:");
                      (void)send_ipc_infof(pfd.fd, "  Aktivität:     %s", "???"); // idle, öffnen, schließen
                      (void)send_ipc_infof(pfd.fd, "  MQTT:          %s",  mqtt_client_is_connected(mqtt_client) ? "Verbunden" : "Nicht verbunden");
                      (void)send_ipc_infof(pfd.fd, "  IPC Clients:   %zu", pollfds_size - POLLFD_FIRST_IPC);
                      (void)send_ipc_infof(pfd.fd, "  Schließbolzen: %s", "???"); // geöffnet, geschlossen
                      (void)send_ipc_infof(pfd.fd, "  Türsensor:     %s", "???"); // geöffnet, geschlossen
                      (void)send_ipc_info(pfd.fd, "");
                      (void)send_ipc_infof(pfd.fd, "Das Portal ist noch nicht vollständig implementiert. Auf Wiedersehen!");

                      // after a status message, we can just drop the client connection
                      remove_ipc_client(i);
                      break;
                    }

                    default: {
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
    } else {
        total_nsecs += loop_end.tv_nsec - loop_start.tv_nsec;
    }

    if(total_nsecs > 10000000UL) { // 1ms
      double time = total_nsecs;
      char const * unit = "ns";

      if(total_nsecs >= 1500000000UL) {
        unit = "s";
        time = total_nsecs / 10000000000.0;
      }
      else if(total_nsecs >= 1500000UL) {
        unit = "ms";
        time = total_nsecs / 10000000.0;
      }
      else if(total_nsecs >= 1500UL) {
        unit = "us";
        time = total_nsecs / 1000.0;
      }

      log_print(LSS_SYSTEM, LL_WARNING, "main loop is hanging, took %.3f %s", time, unit);
    }
  }

  return EXIT_SUCCESS;
}

static enum DoorState fetch_door_state()
{
  bool closed, locked;
  if(!gpio_read(gpio.closed, &closed)) {
    log_print(LSS_GPIO, LL_ERROR, "failed to query gpio.closed");
    return false;
  }
  if(!gpio_read(gpio.locked, &locked)) {
    log_print(LSS_GPIO, LL_ERROR, "failed to query gpio.locked");
    return false;
  }

  enum DoorState state = sm_compute_state(locked, !closed);

  return state;
}

static bool update_state_machine_io(struct StateMachine * sm)
{
  assert(sm != NULL); 
  
  enum DoorState state = fetch_door_state();
  sm_change_door_state(sm, state);

  return true;
}

static void statemachine_handle_signal(struct StateMachine *sm, enum PortalSignal signal)
{
  (void)sm;
  switch(signal) {
    case SIGNAL_OPENING: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "opening");
      break;
    }

    case SIGNAL_LOCKING: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "locking");
      break;
    }

    case SIGNAL_UNLOCKED: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "unlocked");
      break;
    }

    case SIGNAL_OPENED: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "opened");
      break;
    }

    case SIGNAL_NO_ENTRY: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "no entry");
      break;
    }

    case SIGNAL_LOCKED: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "locked");
      break;
    }

    case SIGNAL_ERROR_LOCKING: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "error locking");
      break;
    }

    case SIGNAL_ERROR_OPENING: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "error opening");
      break;
    }

    case SIGNAL_CLOSE_TIMEOUT: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "close timeout");
      break;
    }

    case SIGNAL_WAIT_FOR_DOOR_CLOSED: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "wait for door closed");
      break;
    }

    case SIGNAL_DOOR_MANUALLY_UNLOCKED: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "door manually unlocked");
      break;
    }

    case SIGNAL_DOOR_MANUALLY_LOCKED: {
      log_print(LSS_LOGIC, LL_VERBOSE, "received state machine signal: %s", "door manually locked");
      break;
    }

  }
}

static void statemachine_handle_setTimeout(struct StateMachine *sm, uint32_t ms)
{
  (void)sm;
  if(ms == 0) {
    log_print(LSS_LOGIC, LL_VERBOSE, "received state machine timeout cancel request");
    if(!disarm_timer(sm_timeout_fd)) {
      log_print(LSS_LOGIC, LL_ERROR, "failed to disarm timerfd. state machine will receive unwanted EVENT_TIMEOUT!");
    }
  }
  else {
    log_print(LSS_LOGIC, LL_VERBOSE, "received state machine timeout request: %u ms", ms);
    if(!arm_timer(sm_timeout_fd, true, ms)) {
      log_print(LSS_LOGIC, LL_ERROR, "failed to arm timerfd. state machine will not receive requested EVENT_TIMEOUT!");
    }
  }
}

void statemachine_handle_setIo(struct StateMachine *sm, enum PortalIo io, bool state)
{
  (void)sm;
  switch(io) {
    case IO_TRIGGER_CLOSE: {
      if(!gpio_write(gpio.trigger_close, state)) {
        log_print(LSS_GPIO, LL_ERROR, "failed to write GPIO pin 'trigger open' from state machine. this is bad.");
      }
      break;
    }
    
    case IO_TRIGGER_OPEN: {
      if(!gpio_write(gpio.trigger_open, state)) {
        log_print(LSS_GPIO, LL_ERROR, "failed to write GPIO pin 'trigger open' from state machine. this is bad.");
      }
      break;
    }
  }
}

static bool send_ipc_info(int fd, char const * text)
{
  struct IpcMessage msg = {
    .type = IPC_MSG_INFO,
    .data.info = "",
  };

  strncpy(msg.data.info, text, sizeof msg.data.info);

  return ipc_send_msg(fd, msg);
}

static bool send_ipc_infof(int fd, char const * fmt, ...) 
{
  struct IpcMessage msg = {
    .type = IPC_MSG_INFO,
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

  if(!mqtt_client_connect(mqtt_client)) {
    return false;
  }

  if(!mqtt_client_publish(mqtt_client, "system/demo", "Hello, World!", 2)) {
    log_print(LSS_MQTT, LL_ERROR, "failed to publish message to mqtt server.");
    return false;
  }
  
  if(!mqtt_client_subscribe(mqtt_client, "#")) {
    log_print(LSS_MQTT, LL_ERROR, "failed to subscribe to topic '#' on mqtt server.");
    return false;
  }

  return true;
}

static bool install_signal_handlers()
{
  static struct sigaction const sigint_action = {
    .sa_sigaction = sigint_handler,
    .sa_flags = SA_SIGINFO,
  };
  if(sigaction(SIGINT, &sigint_action, NULL) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to set SIGINT handler");
    return false;
  }

  static struct sigaction const sigterm_action = {
    .sa_sigaction = sigterm_handler,
    .sa_flags = SA_SIGINFO,
  };
  if(sigaction(SIGTERM, &sigterm_action, NULL) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to set SIGTERM handler");
    return false;
  }

  return true;
}

static int create_reconnect_timeout_timer(int secs)
{
  const struct itimerspec restart_timeout = {
    .it_value = {
      .tv_sec = secs,
      .tv_nsec = 0,
    },
    .it_interval = {
      .tv_sec = secs,
      .tv_nsec = 0,
    },
  };

  int timer = timerfd_create(CLOCK_MONOTONIC, 0);
  if(timer == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to create timerfd");
    log_print(LSS_SYSTEM, LL_ERROR, "destroying daemon, hoping for restart...");
    exit(EXIT_FAILURE);
  }

  if(timerfd_settime(timer, 0, &restart_timeout, NULL) == -1) {
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
    .tv_sec = ms / 1000 ,
    .tv_nsec = 1000000 * (ms % 1000),
  };

  const struct timespec null_time = {
    .tv_sec = 0,
    .tv_nsec = 0,
  };
  
  struct itimerspec timeout;
  if(oneshot) {
    timeout = (struct itimerspec) {
      .it_value = given_ms,
      .it_interval = null_time,
    };
  }
  else {
    timeout = (struct itimerspec) {
      .it_value = given_ms,
      .it_interval = given_ms,
    };
  }

  if(timerfd_settime(timer, 0, &timeout, NULL) == -1) {
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

static size_t add_ipc_client(int fd) {
  if(pollfds_size >= POLLFD_LIMIT) {
    log_print(LSS_IPC, LL_WARNING, "cannot accept ipc client: too many ipc connections!");
    return INVALID_IPC_CLIENT;
  }

  size_t index = pollfds_size;
  pollfds_size += 1;

  pollfds[index] = (struct pollfd) {
    .fd = fd,
    .events = POLLIN,
    .revents = 0,
  };

  return index;
}

static void remove_ipc_client(size_t index) {
  assert(index >= POLLFD_FIRST_IPC);
  assert(index < pollfds_size);

  // close the socket when we remove a client connection
  if(close(pollfds[index].fd) == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to close ipc client");
  }

  // swap-remove with the last index
  // NOTE: This doesn't hurt us when (index == pollfds_size-1), as we're gonna wipe the element then anyways
  pollfds[index] = pollfds[pollfds_size - 1];
  
  pollfds_size -= 1;
  memset(&pollfds[pollfds_size], 0xAA, sizeof(struct pollfd));
}


static void close_ipc_sock() {
  if(close(ipc_sock) == -1) {
    log_perror(LSS_IPC, LL_WARNING, "failed to close ipc socket properly");
  }
  ipc_sock = -1;

  if(unlink(ipc_socket_address.sun_path) == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to delete socket handle");
  }
}

static void close_mqtt_client() {
  mqtt_client_destroy(mqtt_client);
  mqtt_client = NULL;
}


static void sigint_handler(int sig, siginfo_t *info, void *ucontext) {
  (void)sig;
  (void)info;
  (void)ucontext;
  shutdown_requested = 1;
}

static void sigterm_handler(int sig, siginfo_t *info, void *ucontext) {
  (void)sig;
  (void)info;
  (void)ucontext;
  shutdown_requested = 1;
}

static bool create_and_open_gpio(struct GpioPin * pin, struct GpioConfig config, char const * label, int line)
{
  assert(pin != NULL);
  assert(label != NULL);
  strncpy(config.label, label, sizeof config.label);
  return gpio_open_pin(&gpio_core, pin, config, line);
}

static void close_all_gpios(void)
{
  if(gpio.locked.io != NULL) gpio_close_pin(&gpio.locked);
  if(gpio.closed.io != NULL) gpio_close_pin(&gpio.closed);
  if(gpio.button.io != NULL) gpio_close_pin(&gpio.button);
  if(gpio.trigger_close.io != NULL) gpio_close_pin(&gpio.trigger_close);
  if(gpio.trigger_open.io != NULL) gpio_close_pin(&gpio.trigger_open);
  if(gpio.acustic_signal.io != NULL) gpio_close_pin(&gpio.acustic_signal);
  gpio_close(&gpio_core);
}

static void close_sm_timeout_fd(void)
{
  if(close(sm_timeout_fd) == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to close timeout fd");
  }
  sm_timeout_fd = -1;
}

static bool fetch_timer_fd(int fd)
{
  uint64_t counter;
  int res = read(fd, &counter, sizeof counter);
  if(res == -1) {
    log_perror(LSS_SYSTEM, LL_ERROR, "failed to read from timerfd");
    return false;
  }
  return true;
}

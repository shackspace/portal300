
#include "ipc.h"
#include "mqtt-client.h"

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

#include <gpio.h>
#include <mqtt.h>

#define MQTT_RECONNECT_TIMEOUT 5 // seconds


static volatile sig_atomic_t shutdown_requested = 0;

static int ipc_sock = -1;

static struct MqttClient * mqtt_client = NULL;

#define POLLFD_IPC        0 // well defined fd, always the unix socket for IPC 
#define POLLFD_MQTT       1 // well defined fd, either the timerfd for reconnecting MQTT or the socket for MQTT communications
#define POLLFD_FIRST_IPC  2 // First ipc client socket slot
#define POLLFD_LIMIT     32 // number of maximum socket connections

static struct pollfd pollfds[POLLFD_LIMIT];
static size_t pollfds_size = 2;

static const size_t INVALID_IPC_CLIENT = ~0U;

struct CliOptions {
  char const * host_name;
  int port;
  char const * ca_cert_file;
  char const * client_key_file;
  char const * client_crt_file;
};

static void close_ipc_sock();
static void close_mqtt_client();

static void sigint_handler(int sig, siginfo_t *info, void *ucontext);
static void sigterm_handler(int sig, siginfo_t *info, void *ucontext);

static size_t add_ipc_client(int fd);
static void remove_ipc_client(size_t index);

static bool try_connect_mqtt();
static bool install_signal_handlers();
static int create_timeout_timer(int secs);

static bool send_ipc_info(int fd, char const * text);
static bool send_ipc_infof(int fd, char const * fmt, ...) __attribute__ ((format (printf, 2, 3)));

int main(int argc, char **argv) {

  // Initialize libraries and dependencies:

  if(!install_signal_handlers()) {
    fprintf(stderr, "failed to install signal handlers.\n");
    exit(EXIT_FAILURE);
  }

  if(!mqtt_client_init()) {
    fprintf(stderr, "failed to initialize mqtt client library. aborting.\n");
    return EXIT_FAILURE;
  }

  struct CliOptions const cli = {
    .host_name = "localhost",
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
    fprintf(stderr, "failed to create mqtt client.\n");
    return EXIT_FAILURE;
  }
  atexit(close_mqtt_client);
  
  (void)argc;
  (void)argv;

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
    fprintf(stderr, "failed to connect to mqtt server, retrying in %d seconds\n", MQTT_RECONNECT_TIMEOUT);

    // we failed to connect to MQTT, set up a timerfd to retry in some seconds
    int timer = create_timeout_timer(MQTT_RECONNECT_TIMEOUT);
 
    pollfds[POLLFD_MQTT] = (struct pollfd) {
      .fd = timer,
      .events = POLLIN,
      .revents = 0,
    };
  }

  // Bind and setup the ipc socket, so we can receive ipc messages 
  {
    if(bind(ipc_sock, (struct sockaddr const *) &ipc_socket_address, sizeof ipc_socket_address) == -1) {
      perror("failed to bind ipc socket");
      fprintf(stderr, "is another instance of this daemon already running?\n");
      return EXIT_FAILURE;
    }
    atexit(close_ipc_sock);

    if(listen(ipc_sock, 0) == -1) {
      perror("failed to listen on ipc socket");
      return EXIT_FAILURE;
    }
    pollfds[POLLFD_IPC] = (struct pollfd) {
      .fd = ipc_sock,
      .events = POLLIN,
      .revents = 0,
    };
  }

  while(shutdown_requested == false) {
    int const poll_ret = poll(pollfds, pollfds_size, -1); // wait infinitly for an event
    if(poll_ret == -1) {
      if(errno != EINTR) {
        perror("poll failed");
      }
      continue;
    }

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
                fprintf(stderr, "accepted new IPC client on connection slot %zu\n", index);
              }
              else {
                if(close(client_fd) == -1) {
                  perror("failed to close ipc socket connection");
                }
              }
            }
            else {
              perror("failed to accept ipc client");
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
                  fprintf(stderr, "Handle MQTT error here gracefully\n");
                }
                else {
                  fprintf(stderr, "Lost connection to MQTT, reconnecting in %d seconds...\n", MQTT_RECONNECT_TIMEOUT);
                  pollfds[i].fd = create_timeout_timer(MQTT_RECONNECT_TIMEOUT);
                }
              }
            }
            else {
              uint64_t counter;
              int res = read(pfd.fd, &counter, sizeof counter);
              if(res == -1) {
                perror("failed to read from timerfd");
                fprintf(stderr, "destroying daemon, hoping for restart...\n");
                exit(EXIT_FAILURE);
              }

              if(try_connect_mqtt(mqtt_client)) {
                fprintf(stderr, "successfully reconnected to mqtt server.\n");

                if(close(pfd.fd) == -1) {
                  perror("failed to destroy timerfd");
                }

                pollfds[i].fd = mqtt_client_get_socket_fd(mqtt_client);
              }
              else {
                  fprintf(stderr, "failed to connect to mqtt server, retrying in %d seconds\n", MQTT_RECONNECT_TIMEOUT);
              }
            }
            break;
          }

          // IPC client message or error
          default: {
            if(pfd.revents & POLLERR) {
              fprintf(stderr, "lost IPC client on connection slot %zu\n", i);
              remove_ipc_client(i);
            }
            else if(pfd.revents & POLLIN) {
              struct IpcMessage msg;

              enum IpcRcvResult msg_ok = ipc_receive_msg(pfd.fd, &msg);

              switch(msg_ok) {
                case IPC_EOF: {
                  remove_ipc_client(i);
                  fprintf(stderr, "connection to ipc socket slot %zu closed\n", i);
                  break;
                }
                case IPC_ERROR: {
                  // we already printed an error message, just try again in the next loop
                  break;
                }
                case IPC_SUCCESS: {
                  switch(msg.type) {
                    case IPC_MSG_OPEN: {
                      fprintf(stderr, "client %zu requested portal opening for (%d, '%.*s', '%.*s').\n", 
                        i,
                        msg.data.open.member_id,
                        (int)strnlen(msg.data.open.member_nick, sizeof msg.data.open.member_nick),
                        msg.data.open.member_nick,
                        (int)strnlen(msg.data.open.member_name, sizeof msg.data.open.member_name),
                        msg.data.open.member_name
                      );
                      break;
                    }

                    case IPC_MSG_CLOSE: {
                      fprintf(stderr, "client %zu requested portal close.\n", i);
                      break;
                    }

                    case IPC_MSG_SHUTDOWN: {
                      fprintf(stderr, "client %zu requested portal shutdown.\n", i);
                      break;
                    }

                    case IPC_MSG_QUERY_STATUS: {
                      fprintf(stderr, "client %zu requested portal status.\n", i);

                      (void)send_ipc_infof(pfd.fd, "Portal-Status:");
                      (void)send_ipc_infof(pfd.fd, "  Aktivität:     %s", "???"); // idle, öffnen, schließen
                      (void)send_ipc_infof(pfd.fd, "  MQTT:          %s",  mqtt_client_is_connected(mqtt_client) ? "Verbunden" : "Nicht verbunden");
                      (void)send_ipc_infof(pfd.fd, "  IPC Clients:   %lu", pollfds_size - POLLFD_FIRST_IPC);
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
                      fprintf(stderr, "received invalid ipc message of type %u\n", msg.type);
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
  }

  return EXIT_SUCCESS;
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
    fprintf(stderr, "failed to publish message to mqtt server.\n");
    return false;
  }
  
  if(!mqtt_client_subscribe(mqtt_client, "#")) {
    fprintf(stderr, "failed to subscribe to topic '#' on mqtt server.\n");
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
    perror("failed to set SIGINT handler");
    return false;
  }

  static struct sigaction const sigterm_action = {
    .sa_sigaction = sigterm_handler,
    .sa_flags = SA_SIGINFO,
  };
  if(sigaction(SIGTERM, &sigterm_action, NULL) == -1) {
    perror("failed to set SIGTERM handler");
    return false;
  }

  return true;
}

static int create_timeout_timer(int secs)
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
    perror("failed to create timerfd");
    fprintf(stderr, "destroying daemon, hoping for restart...\n");
    exit(EXIT_FAILURE);
  }

  if(timerfd_settime(timer, 0, &restart_timeout, NULL) == -1) {
    perror("failed to arm timerfd");
    fprintf(stderr, "destroying daemon, hoping for restart...\n");
    exit(EXIT_FAILURE);
  }

  return timer;
}

static size_t add_ipc_client(int fd) {
  if(pollfds_size >= POLLFD_LIMIT) {
    fprintf(stderr, "cannot accept ipc client: too many ipc connections!\n");
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
    perror("failed to close ipc client");
  }

  // swap-remove with the last index
  // NOTE: This doesn't hurt us when (index == pollfds_size-1), as we're gonna wipe the element then anyways
  pollfds[index] = pollfds[pollfds_size - 1];
  
  pollfds_size -= 1;
  memset(&pollfds[pollfds_size], 0xAA, sizeof(struct pollfd));
}


static void close_ipc_sock() {
  if(close(ipc_sock) == -1) {
    perror("failed to close ipc socket properly");
  }
  ipc_sock = -1;

  if(unlink(ipc_socket_address.sun_path) == -1) {
    perror("failed to delete socket handle");
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

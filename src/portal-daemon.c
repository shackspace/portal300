#include <asm-generic/socket.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>

#include <stdlib.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include <gpio.h>
#include <mqtt.h>

#include "ipc.h"

#define POLLFD_IPC        0 // well defined socket
#define POLLFD_MQTT       1 // well defined socket
#define POLLFD_FIRST_IPC  2 // first ipc socket
#define POLLFD_LIMIT     32 // number of maximum socket connections

static volatile sig_atomic_t shutdown_requested = 0;

static int ipc_sock = -1;
static int mqtt_sock = -1;

static void close_ipc_sock();
static void close_mqtt_sock();

static void sigint_handler(int sig, siginfo_t *info, void *ucontext);
static void sigterm_handler(int sig, siginfo_t *info, void *ucontext);

static struct pollfd pollfds[POLLFD_LIMIT];
static size_t pollfds_size = 2;

static const size_t INVALID_IPC_CLIENT = ~0U;

static size_t add_ipc_client(int fd);
static void remove_ipc_client(size_t index);

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  struct sigaction const sigint_action = {
    .sa_sigaction = sigint_handler,
    .sa_flags = SA_SIGINFO,
  };

  if(sigaction(SIGINT, &sigint_action, NULL) == -1) {
    perror("failed to set SIGINT handler");
    return EXIT_FAILURE;
  }

  struct sigaction const sigterm_action = {
    .sa_sigaction = sigterm_handler,
    .sa_flags = SA_SIGINFO,
    .sa_restorer = NULL,
  };

  if(sigaction(SIGTERM, &sigterm_action, NULL) == -1) {
    perror("failed to set SIGTERM handler");
    return EXIT_FAILURE;
  }

  ipc_sock = ipc_create_socket();
  if (ipc_sock == -1) {
    return EXIT_FAILURE;
  }

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
  pollfds[POLLFD_MQTT] = (struct pollfd) {
    .fd = mqtt_sock,
    .events = POLLIN,
    .revents = 0,
  };

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
            assert(false); // TODO: Implement MQTT connection handling
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

                      (void)ipc_send_msg(pfd.fd, (struct IpcMessage) {
                        .type = IPC_MSG_INFO,
                        .data.info = "Das Portal ist noch nicht vollständig implementiert. Auf Wiedersehen!",
                      });

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

  if(unlink(ipc_socket_address.sun_path) == -1) {
    perror("failed to delete socket handle");
  }
}

static void close_mqtt_sock() {
  if(close(mqtt_sock) == -1) {
    perror("failed to close mqtt socket properly");
  }
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

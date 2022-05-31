#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

#include "ipc.h"

static void print_usage(FILE * stream);
static void panic(char const * msg);

enum PortalAction {
  PA_OPEN = 1,
  PA_CLOSE = 2,
  PA_SHUTDOWN = 3,
  PA_STATUS = 4,
};

struct PortalArgs {
  bool help;
  int member_id;
  char const * member_nick;
  char const * member_name;
  enum PortalAction action;
};

static bool parse_cli(int argc, char ** argv, struct PortalArgs * args);

static int connecToDaemon();


static int ipc_socket = -1;
static void close_ipc_socket();

int main(int argc, char ** argv)
{
  struct PortalArgs cli;
  if(parse_cli(argc, argv, &cli) == false) {
    return EXIT_FAILURE;
  }
  if(cli.help) {
    print_usage(stdout);
    return EXIT_SUCCESS;
  }

  ipc_socket = connecToDaemon();
  if(ipc_socket == -1) {
    return EXIT_FAILURE;
  }
  atexit(close_ipc_socket);

  // TODO: Open connection to local socket

  // printf("member_id   = %d\n", cli.member_id);
  // printf("member_nick = %s\n", cli.member_nick);
  // printf("member_name = %s\n", cli.member_name);

  switch(cli.action) {
    case PA_OPEN: {
      struct IpcMessage msg = {
        .type = IPC_MSG_OPEN,
        .data.open.member_id = cli.member_id,
      };
      strncpy(msg.data.open.member_name, cli.member_name, IPC_MAX_NAME_LEN);
      strncpy(msg.data.open.member_nick, cli.member_nick, IPC_MAX_NICK_LEN);

      if(!ipc_send_msg(ipc_socket, msg)) {
        return EXIT_FAILURE;
      }
      break;
    }
    case PA_CLOSE: {
      bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage) {
        .type = IPC_MSG_CLOSE,
      });
      if(!ok) {
        return EXIT_FAILURE;
      }
      break;
    }
    case PA_SHUTDOWN: {
      bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage) {
        .type = IPC_MSG_SHUTDOWN,
      });
      if(!ok) {
        return EXIT_FAILURE;
      }
      break;
    }
    case PA_STATUS: {
      bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage) {
        .type = IPC_MSG_QUERY_STATUS,
      });
      if(!ok) {
        return EXIT_FAILURE;
      }
      break;
    }
  }

  while(true) {
    struct IpcMessage msg;
    enum IpcRcvResult msg_ok = ipc_receive_msg(ipc_socket, &msg);
    if(msg_ok == IPC_EOF) {
      break;
    }
    else if(msg_ok == IPC_ERROR) {
      continue;
    }
    else if(msg_ok == IPC_SUCCESS) {
      switch(msg.type) {
        case IPC_MSG_INFO: {
          size_t const len = strnlen(msg.data.info, sizeof msg.data.info);
          fprintf(stdout, "%.*s\n", (int)len, msg.data.info);
          break;
        }

        default:
          fprintf(stderr, "received invalid ipc message of type %u\n", msg.type);
          break;
      }
    }
    else {
      __builtin_unreachable();
    }
  }

  return EXIT_SUCCESS;
}

static void print_usage(FILE * stream)
{
  static const char usage_msg[] = 
    "portal-trigger [-h] -i <id> -f <name> -n <nick> <action>" "\n"
    "" "\n"
    "Opens or closes the shackspace portal." "\n"
    "" "\n"
    "The following <action>s are available:" "\n"
    "  open       the portal will be unlocked." "\n"
    "  close      the portal will be closed." "\n"
    "  shutdown   the shackspace will be shut down." "\n"
    "  status     the current status of this portal will be printed." "\n"
    "" "\n"
    "Options:" "\n"
    "  -h         Print this help text." "\n"
    "  -i <id>    The member id of the keyholder." "\n"
    "  -f <name>  The full name of the keyholder." "\n"
    "  -n <nick>  The nick name of the keyholder." "\n"
  ;

  fprintf(stream, usage_msg);
}

static void close_ipc_socket()
{
  if(close(ipc_socket) == -1) {
    perror("failed to close ipc socket");
  }
}

static void panic(char const * msg)
{
  fprintf(stderr, "\n\nPANIC: %s\n\n\n", msg);
  exit(EXIT_FAILURE);
}

static bool parse_cli(int argc, char ** argv, struct PortalArgs * args)
{
  *args = (struct PortalArgs) {
    .help = false,
    .member_id = 0,
    .member_nick = NULL,
    .member_name = NULL,
    .action = 0,
  };

  {
    int opt;
    while ((opt = getopt(argc, argv, "hn:f:i:")) != -1) {
      switch (opt) {
        case 'n': { // nick name
          args->member_nick = strdup(optarg);
          if(args->member_nick == NULL) {
            panic("out of memory");
          }
          break;
        }
        case 'f': { // full name
          args->member_name = strdup(optarg);
          if(args->member_name == NULL) {
            panic("out of memory");
          }
          break;
        }
        case 'i': { // member id
          errno = 0;
          char * end_ptr = optarg;
          args->member_id = strtol(optarg, &end_ptr, 10);
          if((errno != 0) || (end_ptr != (optarg + strlen(optarg))) || (args->member_id < 0)) {
            fprintf(stderr, "invalid member id: %s\n", optarg);
            return false;
          }
          break;
        }

        case 'h': {
          args->help = true;
          return true;
        }

        default: {
          // unknown argument, error message is already printed by getopt
          return false;
        }
      }
    }
  }

  if (optind >= argc) {
     print_usage(stderr);
     return false;
  }

  const char * const action_str = argv[optind];

  if(strcmp(action_str, "open") == 0) {
    args->action = PA_OPEN;
  }
  else if(strcmp(action_str, "close") == 0) {
    args->action = PA_CLOSE;
  }
  else if(strcmp(action_str, "shutdown") == 0) {
    args->action = PA_SHUTDOWN;
  }
  else if(strcmp(action_str, "status") == 0) {
    args->action = PA_STATUS;
  }
  else {
    fprintf(stderr, "Invalid action: %s\n", action_str);
    return false;
  }

  bool params_ok = true;

  if(args->member_id == 0) {
    fprintf(stderr, "Option -i is missing!\n");
    params_ok = false;
  }
  else if(args->member_id < 0) {
    fprintf(stderr, "Option -i requires a positive member id!\n");
    params_ok = false;
  }

  if(args->member_nick == NULL) {
    fprintf(stderr, "Option -n is missing!\n");
    params_ok = false;
  }
  else if(strlen(args->member_nick) > IPC_MAX_NICK_LEN) {
    fprintf(stderr, "A member nick name has a limit of %u bytes, but %zu bytes were given!\n", IPC_MAX_NICK_LEN, strlen(args->member_nick));
    params_ok = false;
  }

  if(args->member_name == NULL) {
    fprintf(stderr, "Option -f is missing!\n");
  }
  else if(strlen(args->member_name) > IPC_MAX_NAME_LEN) {
    fprintf(stderr, "A member nick name has a limit of %u bytes, but %zu bytes were given!\n", IPC_MAX_NAME_LEN, strlen(args->member_name));
    params_ok = false;
  }
  
  if(params_ok == false) {
    return false;
  }

  return true;
}

static int connecToDaemon()
{
  int sock = ipc_create_socket();
  if(sock == -1) {
    return -1;
  }

  if(connect(sock, (struct sockaddr const *) &ipc_socket_address, sizeof ipc_socket_address) == -1) {
    fprintf(stderr, "failed to connect to daemon: %s\n", strerror(errno));
    close(sock);
    return -1;
  }

  return sock;
}
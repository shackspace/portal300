#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <assert.h>

#include "ipc.h"
#include "log.h"

static void print_usage(FILE * stream);
static void panic(char const * msg);

enum PortalAction
{
  PA_OPEN_FRONT    = 1,
  PA_OPEN_BACK     = 2,
  PA_CLOSE         = 3,
  PA_SHUTDOWN      = 4,
  PA_STATUS        = 5,
  PA_SIMPLE_STATUS = 6,
};

struct PortalArgs
{
  bool              help;
  int               member_id;
  char const *      member_nick;
  char const *      member_name;
  enum PortalAction action;
};

static bool parse_cli(int argc, char ** argv, struct PortalArgs * args);

static int connecToDaemon(void);

static int  ipc_socket = -1;
static void close_ipc_socket(void);

int main(int argc, char ** argv)
{
  if (!log_init()) {
    fprintf(stderr, "failed to initialize logging.\n");
    return EXIT_FAILURE;
  }

  struct PortalArgs cli;
  if (parse_cli(argc, argv, &cli) == false) {
    return EXIT_FAILURE;
  }
  if (cli.help) {
    print_usage(stdout);
    return EXIT_SUCCESS;
  }

  ipc_socket = connecToDaemon();
  if (ipc_socket == -1) {
    return EXIT_FAILURE;
  }
  atexit(close_ipc_socket);

  // TODO: Open connection to local socket

  // printf("member_id   = %d\n", cli.member_id);
  // printf("member_nick = %s\n", cli.member_nick);
  // printf("member_name = %s\n", cli.member_name);

  switch (cli.action) {
  case PA_OPEN_BACK:
  case PA_OPEN_FRONT:
  {
    struct IpcMessage msg = {
        .type                = (cli.action == PA_OPEN_FRONT) ? IPC_MSG_OPEN_FRONT : IPC_MSG_OPEN_BACK,
        .data.open.member_id = cli.member_id,
    };
    strncpy(msg.data.open.member_name, cli.member_name, IPC_MAX_NAME_LEN);
    strncpy(msg.data.open.member_nick, cli.member_nick, IPC_MAX_NICK_LEN);

    if (!ipc_send_msg(ipc_socket, msg)) {
      return EXIT_FAILURE;
    }
    break;
  }
  case PA_CLOSE:
  {
    bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage){
                                                 .type = IPC_MSG_CLOSE,
                                             });
    if (!ok) {
      return EXIT_FAILURE;
    }
    break;
  }
  case PA_SHUTDOWN:
  {
    bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage){
                                                 .type = IPC_MSG_SHUTDOWN,
                                             });
    if (!ok) {
      return EXIT_FAILURE;
    }
    break;
  }
  case PA_STATUS:
  {
    bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage){
                                                 .type = IPC_MSG_QUERY_STATUS,
                                             });
    if (!ok) {
      return EXIT_FAILURE;
    }
    break;
  }
  case PA_SIMPLE_STATUS:
  {
    bool const ok = ipc_send_msg(ipc_socket, (struct IpcMessage){
                                                 .type = IPC_MSG_SIMPLE_STATUS,
                                             });
    if (!ok) {
      return EXIT_FAILURE;
    }
    break;
  }
  }

  while (true) {
    struct IpcMessage msg;
    enum IpcRcvResult msg_ok = ipc_receive_msg(ipc_socket, &msg);
    if (msg_ok == IPC_EOF) {
      break;
    }
    else if (msg_ok == IPC_ERROR) {
      continue;
    }
    else if (msg_ok == IPC_SUCCESS) {
      switch (msg.type) {
      case IPC_MSG_INFO:
      {
        size_t const len = strnlen(msg.data.info, sizeof msg.data.info);
        fprintf(stdout, "%.*s\n", (int)len, msg.data.info);
        break;
      }

      default:
        log_print(LSS_SYSTEM, LL_WARNING, "received invalid ipc message of type %u\n", msg.type);
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
      "portal-trigger [-h] -i <id> -f <name> -n <nick> <action>"
      "\n"
      ""
      "\n"
      "Opens or closes the shackspace portal."
      "\n"
      ""
      "\n"
      "The following <action>s are available:"
      "\n"
      "  open       the portal will be unlocked."
      "\n"
      "  close      the portal will be closed."
      "\n"
      "  shutdown   the shackspace will be shut down."
      "\n"
      "  status     the current status of this portal will be printed."
      "\n"
      ""
      "\n"
      "Options:"
      "\n"
      "  -h         Print this help text."
      "\n"
      "  -i <id>    The member id of the keyholder."
      "\n"
      "  -f <name>  The full name of the keyholder."
      "\n"
      "  -n <nick>  The nick name of the keyholder."
      "\n";

  fprintf(stream, usage_msg);
}

static void close_ipc_socket()
{
  if (close(ipc_socket) == -1) {
    perror("failed to close ipc socket");
  }
}

static void panic(char const * msg)
{
  log_print(LSS_SYSTEM, LL_ERROR, "\n\nPANIC: %s\n\n\n", msg);
  exit(EXIT_FAILURE);
}

static bool parse_action(char const * action_str, enum PortalAction * action)
{
  assert(action_str != NULL);
  assert(action != NULL);

  if (strcmp(action_str, "open-front") == 0) {
    *action = PA_OPEN_FRONT;
  }
  else if (strcmp(action_str, "open-back") == 0) {
    *action = PA_OPEN_BACK;
  }
  else if (strcmp(action_str, "close") == 0) {
    *action = PA_CLOSE;
  }
  else if (strcmp(action_str, "shutdown") == 0) {
    *action = PA_SHUTDOWN;
  }
  else if (strcmp(action_str, "status") == 0) {
    *action = PA_STATUS;
  }
  else if (strcmp(action_str, "simple-status") == 0) {
    *action = PA_SIMPLE_STATUS;
  }
  else {
    return false;
  }
  return true;
}

static bool parse_cli(int argc, char ** argv, struct PortalArgs * args)
{
  *args = (struct PortalArgs){
      .help        = false,
      .member_id   = 0,
      .member_nick = NULL,
      .member_name = NULL,
      .action      = 0,
  };

  {
    int opt;
    while ((opt = getopt(argc, argv, "hn:f:i:")) != -1) {
      switch (opt) {
      case 'n':
      { // nick name
        args->member_nick = strdup(optarg);
        if (args->member_nick == NULL) {
          panic("out of memory");
        }
        break;
      }
      case 'f':
      { // full name
        args->member_name = strdup(optarg);
        if (args->member_name == NULL) {
          panic("out of memory");
        }
        break;
      }
      case 'i':
      { // member id
        errno           = 0;
        char * end_ptr  = optarg;
        args->member_id = strtol(optarg, &end_ptr, 10);
        if ((errno != 0) || (end_ptr != (optarg + strlen(optarg))) || (args->member_id < 0)) {
          fprintf(stderr, "invalid member id: %s\n", optarg);
          return false;
        }
        break;
      }

      case 'h':
      {
        args->help = true;
        return true;
      }

      default:
      {
        // unknown argument, error message is already printed by getopt
        return false;
      }
      }
    }
  }

  // Allow SSH_ORIGINAL_COMMAND to be used to open the portal as well.
  char const * ssh_original_command = getenv("SSH_ORIGINAL_COMMAND");
  if (ssh_original_command == NULL) {
    ssh_original_command = ""; // sane default
  }

  // verify that if we have a ssh action, it must be a valid one.
  bool ssh_action_ok = parse_action(ssh_original_command, &args->action);
  if (!ssh_action_ok && strcmp(ssh_original_command, "") != 0) {
    print_usage(stderr);
    return false;
  }

  if (!ssh_action_ok && optind >= argc) {
    // If we don't have a ssh action, we require a action passed
    // on the command line.
    print_usage(stderr);
    return false;
  }

  if (!ssh_action_ok) {
    // not having an OK ssh action here means we got no action via SSH,
    // as invalid options are filtered out earlier alreaedy.

    const char * const action_str = argv[optind];
    if (!parse_action(action_str, &args->action)) {
      fprintf(stderr, "Invalid action: %s\n", action_str);
      return false;
    }
  }

  bool params_ok = true;

  bool requires_user_info = (args->action == PA_OPEN_FRONT) || (args->action == PA_OPEN_BACK);

  if (requires_user_info) {
    if (args->member_id == 0) {
      fprintf(stderr, "Option -i is missing!\n");
      params_ok = false;
    }
    else if (args->member_id < 0) {
      fprintf(stderr, "Option -i requires a positive member id!\n");
      params_ok = false;
    }

    if (args->member_nick == NULL) {
      fprintf(stderr, "Option -n is missing!\n");
      params_ok = false;
    }
    else if (strlen(args->member_nick) > IPC_MAX_NICK_LEN) {
      fprintf(stderr, "A member nick name has a limit of %u bytes, but %zu bytes were given!\n", IPC_MAX_NICK_LEN, strlen(args->member_nick));
      params_ok = false;
    }

    if (args->member_name == NULL) {
      fprintf(stderr, "Option -f is missing!\n");
    }
    else if (strlen(args->member_name) > IPC_MAX_NAME_LEN) {
      fprintf(stderr, "A member nick name has a limit of %u bytes, but %zu bytes were given!\n", IPC_MAX_NAME_LEN, strlen(args->member_name));
      params_ok = false;
    }
  }

  if (params_ok == false) {
    return false;
  }

  return true;
}

static int connecToDaemon()
{
  int sock = ipc_create_socket();
  if (sock == -1) {
    return -1;
  }

  if (connect(sock, (struct sockaddr const *)&ipc_socket_address, sizeof ipc_socket_address) == -1) {
    log_print(LSS_SYSTEM, LL_ERROR, "failed to connect to daemon: %s\n", strerror(errno));
    close(sock);
    return -1;
  }

  return sock;
}

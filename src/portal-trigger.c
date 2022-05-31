#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

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

  // TODO: Open connection to local socket

  printf("member_id   = %d\n", cli.member_id);
  printf("member_nick = %s\n", cli.member_nick);
  printf("member_name = %s\n", cli.member_name);

  switch(cli.action) {
    case PA_OPEN: {
      printf("action = OPEN\n");
      break;
    }
    case PA_CLOSE: {
      printf("action = CLOSE\n");
      break;
    }
    case PA_SHUTDOWN: {
      printf("action = SHUTDOWN\n");
      break;
    }
    case PA_STATUS: {
      printf("action = STATUS\n");
      break;
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

  if(args->member_id == 0) {
    fprintf(stderr, "Option -i is missing!\n");
  }

  if(args->member_nick == NULL) {
    fprintf(stderr, "Option -n is missing!\n");

  }
  if(args->member_name == NULL) {
    fprintf(stderr, "Option -f is missing!\n");
  }
  
  if((args->member_id == 0) || (args->member_nick == NULL) || (args->member_name == NULL)) {
    return false;
  }

  return true;
}
#include "ipc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <grp.h>

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "log.h"

const struct sockaddr_un ipc_socket_address = {
    .sun_family = AF_UNIX,
    .sun_path   = "/tmp/portal300-ipc.socket",
};

int ipc_create_socket()
{
  int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (sock == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to create ipc socket");
    return -1;
  }

  return sock;
}

bool ipc_set_flags(int sock)
{
  // we use chmod because fchmod doesn't seem to work on sockets.
  (void)sock;

  // Make socket readable/writeable by group/owner
  static mode_t const mode = 0666;
  if (chmod(ipc_socket_address.sun_path, mode) == -1) {
    log_perror(LSS_IPC, LL_ERROR, "failed to set permission for IPC socket");
    return false;
  }

  return true;
}

bool ipc_send_msg(int sock, struct IpcMessage msg)
{
  // we can safely send/receive the message as it is in-memory as we
  // are always on the same machine with unix sockets.
  // WARNING: This assumption requires to have IpcMessage not contain any pointers.
  ssize_t const len = write(sock, &msg, sizeof msg);
  if (len < 0) {
    log_perror(LSS_IPC, LL_ERROR, "failed to send ipc message");
    return false;
  }
  if (len != sizeof msg) {
    log_print(LSS_IPC, LL_ERROR, "sent partial ipc message. only transferred %zu of %zu bytes", (size_t)len, sizeof msg);
    return false;
  }
  return true;
}

// TODO: Handle EOF and error differently!
enum IpcRcvResult ipc_receive_msg(int sock, struct IpcMessage * msg)
{
  // we can safely send/receive the message as it is in-memory as we
  // are always on the same machine with unix sockets.
  // WARNING: This assumption requires to have IpcMessage not contain any pointers.
  ssize_t const len = read(sock, msg, sizeof *msg);
  if (len < 0) {
    perror("failed to send ipc message");
    return IPC_ERROR;
  }
  else if (len == 0) {
    return IPC_EOF;
  }
  else if (len != sizeof *msg) {
    log_print(LSS_IPC, LL_ERROR, "received partial ipc message. only transferred %zu of %zu bytes", (size_t)len, sizeof *msg);
    return IPC_ERROR;
  }
  return IPC_SUCCESS;
}
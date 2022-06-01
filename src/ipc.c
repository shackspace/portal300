#include <sys/un.h>
#include <sys/socket.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "ipc.h"

const struct sockaddr_un ipc_socket_address  = {
  .sun_family = AF_UNIX,
  .sun_path = "/tmp/portal300-ipc.socket",
};

int ipc_create_socket()
{
  int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if(sock == -1) {
    fprintf(stderr, "failed to create ipc socket: %s\n", strerror(errno));
    return -1;
  }
  return sock;
}

bool ipc_send_msg(int sock, struct IpcMessage msg)
{
  // we can safely send/receive the message as it is in-memory as we 
  // are always on the same machine with unix sockets.
  // WARNING: This assumption requires to have IpcMessage not contain any pointers. 
  ssize_t const len = write(sock, &msg, sizeof msg);
  if(len < 0) {
    perror("failed to send ipc message");
    return false;
  }
  if(len != sizeof msg) {
    fprintf(stderr, "sent partial ipc message. only transferred %zu of %zu bytes", (size_t)len, sizeof msg);
    return false;
  }
  return true;
}

// TODO: Handle EOF and error differently!
enum IpcRcvResult ipc_receive_msg(int sock, struct IpcMessage *msg)
{
  // we can safely send/receive the message as it is in-memory as we 
  // are always on the same machine with unix sockets.
  // WARNING: This assumption requires to have IpcMessage not contain any pointers.
  ssize_t const len = read(sock, msg, sizeof *msg);
  if(len < 0) {
    perror("failed to send ipc message");
    return IPC_ERROR;
  }
  else if(len == 0) {
    return IPC_EOF;
  }
  else if(len != sizeof *msg) {
    fprintf(stderr, "received partial ipc message. only transferred %zu of %zu bytes", (size_t)len, sizeof *msg);
    return IPC_ERROR;
  }
  return IPC_SUCCESS;
}
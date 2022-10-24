#ifndef PORTAL300_IPC_H
#define PORTAL300_IPC_H

#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>

#define IPC_MAX_INFOSTR_LEN 1024
#define IPC_MAX_NICK_LEN    256
#define IPC_MAX_NAME_LEN    256

enum IcpMessageType
{
  // client to daemon
  IPC_MSG_OPEN_FRONT    = 101,
  IPC_MSG_OPEN_BACK     = 102,
  IPC_MSG_CLOSE         = 103,
  IPC_MSG_SHUTDOWN      = 104,
  IPC_MSG_QUERY_STATUS  = 105,
  IPC_MSG_SIMPLE_STATUS = 106,
  IPC_MSG_SYSTEM_RESET  = 107,
  IPC_MSG_FORCE_OPEN    = 108,

  // daemon to client
  IPC_MSG_INFO = 201,
};

enum IpcRcvResult
{
  IPC_SUCCESS = 0,
  IPC_EOF     = 1,
  IPC_ERROR   = 2,
};

struct IpcMessageOpenData
{
  int  member_id;
  char member_nick[IPC_MAX_NICK_LEN]; // not necessarily 0 terminated!
  char member_name[IPC_MAX_NAME_LEN]; // not necessarily 0 terminated!
};

struct IpcMessage
{
  // THIS STRUCT MUST NOT CONTAIN ANY POINTERS!
  enum IcpMessageType type;
  union
  {
    struct IpcMessageOpenData open;
    char                      info[IPC_MAX_INFOSTR_LEN]; // not necessarily 0 terminated!
  } data;
};

// Maximum message length for IPC
#define IPC_MAX_MSG_LEN sizeof(struct IpcMessage)

extern const struct sockaddr_un ipc_socket_address;

//! Creates a new IPC socket that has the right configuration for IPC.
//! Returns -1 on error, otherwise a file handle.
int ipc_create_socket(void);

//! Must be called after successfully binding the socket so
//! the permissions of the socket are correct.
bool ipc_set_flags(int fd);

//! Sends an ipc message via the given socket
//! returns true on success.
bool ipc_send_msg(int sock, struct IpcMessage msg);

//! Receives an ipc message via the given socket.
//! returns true on success.
enum IpcRcvResult ipc_receive_msg(int sock, struct IpcMessage * msg);

#endif // PORTAL300_IPC_H
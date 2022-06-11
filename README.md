# Portal 300 Source Code

**WARNING: This code is not production-ready yet. Please do not use!**

## Applications

### `portal-trigger`

The user frontend to control the portal. Triggers actions on the current device.

```
portal-trigger [-h] -i <id> -f <name> -n <nick> <action>

Opens or closes the shackspace portal.

The following <action>s are available:
  open       the portal will be unlocked.
  close      the portal will be closed.
  shutdown   the shackspace will be shut down.
  status     the current status of this portal will be printed.

Options:
  -h         Print this help text.
  -i <id>    The member id of the keyholder.
  -f <name>  The full name of the keyholder.
  -n <nick>  The nick name of the keyholder.
```

### `portal-daemon`

The core logic for a portal device. Handles:

- IPC messages from `portal-trigger`
- MQTT control messages
- Sensory and button input via GPIOs

## Building

General Requirements:

- GNU `make`
- `gcc`
- `ar`
- `libssl-dev`

Additional packages that need to be installed for Raspbian OS besides the default ones:

- `git`
- `libssl-dev`

```sh-session
[user@host portal300]$ make -B
[user@host portal300]$ ls bin
portal-daemon  portal-trigger
[user@host portal300]$
```

## Architecture

![architectural diagram](docs/architecture.svg)

### Decisions

- Using C as it's a stable programming environment
  - We depend on software that is already there for years and won't change quickly
- Using daemon/trigger architecture
  - to decouple the security relevant logic from the user-facing application
  - to manage double-use by multiple people
- Using MQTT for communications
  - Established standard
  - Available software (mosquitto) and libraries (mqtt-c)
  - TLS support with client certificates

## Fault Vectors

This section contains a list of recognized fault vectors that can bring portal activity down or allows unauthorized users to enter the building.

- Usage mistakes
  - Early SSH disconnect
  - Concurrent use of open/close APIs
- Programming mistakes
  - Out of bounds access
  - Missing error handling
- Attack vectors
  - IP spoofing / MITM
  - Storage manipulation

## Internal Architecture

### Daemon

The daemon initializes itself and falls into a large endless loop which receives all application relevant events via `poll`.
This allows us to handle everything asynchronously without multithreading and react to events in a low time. It also saves energy
for when no communication happens.

### Trigger

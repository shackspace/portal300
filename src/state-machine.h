#ifndef PORTAL300_STATE_MACHINE_H
#define PORTAL300_STATE_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum DoorState {
  DOOR_CLOSED = 0, // open=0, locked=0
  DOOR_LOCKED = 1, // open=0, locked=1
  DOOR_OPEN = 2,   // open=1, locked=0
  DOOR_FAULT = 3,  // open=1, locked=1
};

//! Events are sent from the user to the state machine.
//! A user is the controller of the state machine.
enum PortalEvent {
  EVENT_OPEN,
  EVENT_CLOSE,
  EVENT_TIMEOUT,
};

//! Signals are events that are sent from the state machine to the user.
//! A user is the controller of the state machine.
enum PortalSignal {
  //! The state machine begins the portal unlocking process
  SIGNAL_OPENING,

  //! The state machine begins the portal locking process
  SIGNAL_LOCKING,

  //! The door bolt was removed from the lock, door can now be opened.
  SIGNAL_UNLOCKED,

  //! The door was successfully unlocked and was physically opened
  SIGNAL_OPENED,

  //! Nobody entered the door after unlocking, the door will now be closed again
  SIGNAL_NO_ENTRY,

  //! The door was successfully locked, the locking bolt is now engaged
  SIGNAL_LOCKED,

  //! The door failed to engange the door bolt in a reasonable time.
  SIGNAL_ERROR_LOCKING,

  //! The door failed to remove the door bolt from the lock in a reasonable
  //! time.
  SIGNAL_ERROR_OPENING,
};

enum PortalError {
  //! Operation was successful
  SM_SUCCESS = 0,

  //! There is currently a Portal operation in progress, try again later
  SM_ERR_IN_PROGRESS = 1,

  //! The event was sent in an unexpected context, for example when no timeout
  //! was requested.
  SM_ERR_UNEXPECTED = 2,
};

struct StateMachine;

typedef void (*StateMachineSignal)(struct StateMachine *sm,
                                   enum PortalSignal signal);
typedef void (*StateMachineSetTimeout)(struct StateMachine *sm, uint32_t ms);

struct StateMachine {
  //! General purpose user data that can be used to obtain a external context
  //! for event handling.
  void *user_data;

  //! The state machine wants to tell the user that an event happened.
  StateMachineSignal signal;

  //! The state machine requests an `EVENT_TIMEOUT` in the given number of
  //! milliseconds. If `ms` is `0`, the timeout is request is cleared and
  //! no `EVENT_TIMEOUT` should be sent anymore.
  StateMachineSetTimeout setTimeout;

  // internals:
  unsigned int door_state;
  unsigned int logic_state;
};

//! Initializes a state machine.
//! - `signal` is a mandatory callback that is used for signaling relevant state
//!   changes to the user.
//! - `setTimeout` is a mandatory callback that is used to request/cancel
//!   `EVENT_TIMEOUT`.
//! - `user_data` is an optional pointer to *anything*. This can be used in the
//!   callbacks to obtain a well-known context from the state machine.
void sm_init(struct StateMachine *sm, StateMachineSignal signal,
             StateMachineSetTimeout setTimeout, void *user_data);

//! Notifies the state machine of a change in the door state.
void sm_change_door_state(struct StateMachine *sm, enum DoorState new_state);

//! Sends a event to the state machine. This is either user-triggered or
//! generated automatically like timeouts. Returns an error when the action
//! could not be handled.
enum PortalError sm_send_event(struct StateMachine *sm, enum PortalEvent event);

//! Computes a `DoorState` from two booleans `locked` and `open`.
//! - `locked` means the door sensor for "locking bolt is not enganged"
//! - `open` means the door sensor for "door is closed" does not trigger.
enum DoorState sm_compute_state(bool locked, bool open);

#endif // PORTAL300_STATE_MACHINE_H

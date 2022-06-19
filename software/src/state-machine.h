#ifndef PORTAL300_DAEMON_STATE_MACHINE_H
#define PORTAL300_DAEMON_STATE_MACHINE_H

#include <stdbool.h>

enum SM_Event
{
  // door status changes:
  EVENT_DOOR_B2_OPENED, // door B2 changed its state to open
  EVENT_DOOR_B2_CLOSED, // door B2 changed its state to closed
  EVENT_DOOR_B2_LOCKED, // door B2 changed its state to locked

  EVENT_DOOR_C2_OPENED, // door C2 changed its state to open
  EVENT_DOOR_C2_CLOSED, // door C2 changed its state to closed
  EVENT_DOOR_C2_LOCKED, // door C2 changed its state to locked

  // user interactions:
  EVENT_SSH_OPEN_FRONT_REQUEST, // user requested front door entry via SSH interface
  EVENT_SSH_OPEN_BACK_REQUEST,  // user requested back door entry via SSH interface
  EVENT_SSH_CLOSE_REQUEST,      // user requested lock down via SSH interface

  EVENT_BUTTON_C2, // User pressed the button on door C2
  EVENT_BUTTON_B2, // User pressed the button on door B2

  // other events
  EVENT_DOORBELL_FRONT, // Someone rang the door bell.
};

enum SM_Signal
{
  SIGNAL_OPEN_DOOR_B,         // opens door B (temporary, unlocks itself after short time)
  SIGNAL_OPEN_DOOR_C,         // opens door C (temporary, unlocks itself after short time)
  SIGNAL_OPEN_DOOR_B2_SAFE,   // opens door B2 permanently, with automatic close after 60 seconds if not opened
  SIGNAL_OPEN_DOOR_C2_SAFE,   // opens door C2 permanently, with automatic close after 60 seconds if not opened
  SIGNAL_OPEN_DOOR_B2_UNSAFE, // opens door B2 permanently, without waiting for any event
  SIGNAL_OPEN_DOOR_C2_UNSAFE, // opens door C2 permanently, without waiting for any event
  SIGNAL_LOCK_ALL,            // lock all lockable doors

  SIGNAL_CHANGE_KEYHOLDER, // the shack is already open, but we transfer the key holder right
  SIGNAL_STATE_CHANGE,     // the shack changed it's open state
  SIGNAL_NO_STATE_CHANGE,  // A state change was requested, but that state is already present

  SIGNAL_UNLOCK_SUCCESSFUL, // shackspace was successfully unlocked
  SIGNAL_LOCK_SUCCESSFUL,   // shackspace was successfully locked
  SIGNAL_OPEN_SUCCESSFUL,   // shackspace was successfully opened

  SIGNAL_CANNOT_HANDLE_REQUEST, // we cannot handle the request right now, as something is still in progress
};

enum DoorState
{
  DOOR_UNOBSERVED = 0,
  DOOR_OPEN       = 1,
  DOOR_CLOSED     = 2,
  DOOR_LOCKED     = 3,
};

enum ShackState
{
  SHACK_UNOBSERVED      = 0,
  SHACK_OPEN            = 1,
  SHACK_UNLOCKED_VIA_B2 = 2,
  SHACK_UNLOCKED_VIA_C2 = 3,
  SHACK_LOCKED          = 4,
};

struct StateMachine;

//! Signal handler callback
//! - `user_data` is the pointer passed into `sm_init`,
//! - `context` is the pointer passed into `sm_apply_event`.
//! - `signal` is the response from the state machine
typedef void (*StateMachineSignal)(void * user_data, void * context, enum SM_Signal signal);

struct StateMachine
{
  int             state;
  enum DoorState  door_c2;
  enum DoorState  door_b2;
  enum ShackState last_shack_state;

  StateMachineSignal on_signal;
  void *             user_data;
};

void sm_init(struct StateMachine * sm, StateMachineSignal signal_handler, void * user_data);

void sm_apply_event(struct StateMachine * sm, enum SM_Event event, void * context);

enum ShackState sm_get_shack_state(struct StateMachine const * sm);

char const * sm_shack_state_name(enum ShackState state);
char const * sm_door_state_name(enum DoorState state);
char const * sm_state_name(struct StateMachine const * sm);

#endif

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
  SIGNAL_CLOSE_ALL,           // closes all closable doors

  SIGNAL_CHANGE_KEYHOLDER, // the shack is already open, but we transfer the key holder right

  SIGNAL_UNLOCK_SUCCESSFUL, // shackspace was successfully unlocked
  SIGNAL_LOCK_SUCCESSFUL,   // shackspace was successfully locked
  SIGNAL_OPEN_SUCCESSFUL,   // shackspace was successfully opened

  SIGNAL_CANNOT_HANDLE_REQUEST, // we cannot handle the request right now, as something is still in progress
};

enum DoorState
{
  DOOR_OPEN   = 0,
  DOOR_CLOSED = 1,
  DOOR_LOCKED = 2,
};

enum ShackState
{
  SHACK_OPEN            = 0,
  SHACK_UNLOCKED_VIA_B2 = 1,
  SHACK_UNLOCKED_VIA_C2 = 2,
  SHACK_LOCKED          = 3,
};

struct StateMachine
{
  int            state;
  enum DoorState door_c2;
  enum DoorState door_b2;
};

void sm_init(struct StateMachine * sm);

void sm_apply_event(struct StateMachine * sm, enum SM_Event event, void * user_context);

enum ShackState sm_get_state(struct StateMachine const * sm);

#endif

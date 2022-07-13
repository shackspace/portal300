#include "state-machine.h"

#include <assert.h>
#include <stddef.h>

enum State
{
  STATE_IDLE,
  STATE_WAIT_FOR_LOCKED,
  STATE_WAIT_FOR_OPEN_VIA_B,
  STATE_WAIT_FOR_OPEN_VIA_C,
};

void sm_init(struct StateMachine * sm, StateMachineSignal signal_handler, void * user_data)
{
  assert(sm != NULL);
  assert(signal_handler != NULL);
  *sm = (struct StateMachine){
      .state = STATE_IDLE,

      .door_c2          = DOOR_UNOBSERVED,
      .door_b2          = DOOR_UNOBSERVED,
      .last_shack_state = SHACK_UNOBSERVED,

      .on_signal = signal_handler,
      .user_data = user_data,
  };
}

static bool is_open_request(enum SM_Event event)
{
  if (event == EVENT_SSH_OPEN_FRONT_REQUEST)
    return true;
  if (event == EVENT_SSH_OPEN_BACK_REQUEST)
    return true;
  return false;
}

static bool is_close_request(enum SM_Event event, enum ShackState shack_state)
{
  if (event == EVENT_SSH_CLOSE_REQUEST)
    return true;
  if (event == EVENT_BUTTON_C2 && shack_state != SHACK_LOCKED)
    return true;
  if (event == EVENT_BUTTON_B2 && shack_state != SHACK_LOCKED)
    return true;
  return false;
}

enum DoorFilter
{
  DOOR_B2  = 1,
  DOOR_C2  = 2,
  DOOR_ANY = DOOR_B2 | DOOR_C2,
};

static bool is_door_unlock_event(enum SM_Event event, enum DoorFilter filter)
{
  if (event == EVENT_DOOR_B2_OPENED && (filter & DOOR_B2))
    return true;
  if (event == EVENT_DOOR_B2_CLOSED && (filter & DOOR_B2))
    return true;
  if (event == EVENT_DOOR_C2_OPENED && (filter & DOOR_C2))
    return true;
  if (event == EVENT_DOOR_C2_CLOSED && (filter & DOOR_C2))
    return true;
  return false;
}

static bool is_door_lock_event(enum SM_Event event, enum DoorFilter filter)
{
  if (event == EVENT_DOOR_B2_LOCKED && (filter & DOOR_B2))
    return true;
  if (event == EVENT_DOOR_C2_LOCKED && (filter & DOOR_C2))
    return true;
  return false;
}

static bool is_state_wait_for_open(enum State state)
{
  if (state == STATE_WAIT_FOR_OPEN_VIA_B)
    return true;
  if (state == STATE_WAIT_FOR_OPEN_VIA_C)
    return true;
  return false;
}

static bool is_ssh_request(enum SM_Event event)
{
  if (event == EVENT_SSH_OPEN_BACK_REQUEST)
    return true;
  if (event == EVENT_SSH_OPEN_FRONT_REQUEST)
    return true;
  if (event == EVENT_SSH_CLOSE_REQUEST)
    return true;
  return false;
}

// Applies changes to the door states based on a event.
static void sm_change_door_state(struct StateMachine * sm, enum SM_Event event)
{
  if (event == EVENT_DOOR_B2_OPENED) {
    sm->door_b2 = DOOR_OPEN;
  }
  if (event == EVENT_DOOR_B2_CLOSED) {
    sm->door_b2 = DOOR_CLOSED;
  }
  if (event == EVENT_DOOR_B2_LOCKED) {
    sm->door_b2 = DOOR_LOCKED;
  }

  if (event == EVENT_DOOR_C2_OPENED) {
    sm->door_c2 = DOOR_OPEN;
  }
  if (event == EVENT_DOOR_C2_CLOSED) {
    sm->door_c2 = DOOR_CLOSED;
  }
  if (event == EVENT_DOOR_C2_LOCKED) {
    sm->door_c2 = DOOR_LOCKED;
  }
}

static void send_signal(struct StateMachine * sm, void * context, enum SM_Signal signal)
{
  sm->on_signal(sm->user_data, context, signal);
}

void sm_apply_event(struct StateMachine * sm, enum SM_Event event, void * user_context)
{
  // incorporate all door change events *BEFORE* computing the current state
  // of shackspace!
  sm_change_door_state(sm, event);

  enum ShackState shack_state = sm_get_shack_state(sm);
  enum State      sm_state    = sm->state;

#define SIGNAL(_Signal) send_signal(sm, user_context, _Signal)

  // Check if the shack space changed its state
  if (shack_state != sm->last_shack_state) {
    sm->last_shack_state = shack_state;
    SIGNAL(SIGNAL_STATE_CHANGE);
    // don't return here, this is just a notification!
  }

  if (event == EVENT_TIMEOUT && sm_state != STATE_IDLE) {
    // the requested action timed out
    sm->state = STATE_IDLE;
    SIGNAL(SIGNAL_USER_REQUESTED_TIMED_OUT);
    return;
  }

  if (event == EVENT_DOOR_B2_LOCKED && sm_state == STATE_WAIT_FOR_OPEN_VIA_B) {
    // after a request to unlock via B2, door B2 was successfully unlocked, but never opened and
    // has auto-closed itself again.
    sm->state = STATE_WAIT_FOR_LOCKED;
    SIGNAL(SIGNAL_START_TIMEOUT);  // trigger potential timeout for the locking operation
    SIGNAL(SIGNAL_UNLOCK_TIMEOUT); // signal via ssh that nobody entered the shack and we're locking again.
    return;
  }

  if (event == EVENT_DOOR_C2_LOCKED && sm_state == STATE_WAIT_FOR_OPEN_VIA_C) {
    // after a request to unlock via C2, door C2 was successfully unlocked, but never opened and
    // has auto-closed itself again.
    sm->state = STATE_WAIT_FOR_LOCKED;
    SIGNAL(SIGNAL_START_TIMEOUT);  // trigger potential timeout for the locking operation
    SIGNAL(SIGNAL_UNLOCK_TIMEOUT); // signal via ssh that nobody entered the shack and we're locking again.
    return;
  }

  if (event == EVENT_DOOR_B2_OPENED && shack_state != SHACK_OPEN && sm_state == STATE_WAIT_FOR_OPEN_VIA_B) {
    // after a request to unlock via B2, door B2 was successfully opened by a user, now unlock the other door:
    SIGNAL(SIGNAL_OPEN_DOOR_C2_UNSAFE);
    return;
  }

  if (event == EVENT_DOOR_C2_OPENED && shack_state != SHACK_OPEN && sm_state == STATE_WAIT_FOR_OPEN_VIA_C) {
    // after a request to unlock via C2, door C2 was successfully opened by a user, now unlock the other door:
    SIGNAL(SIGNAL_OPEN_DOOR_B2_UNSAFE);
    return;
  }

  if (is_door_unlock_event(event, DOOR_ANY) && shack_state == SHACK_OPEN && is_state_wait_for_open(sm_state)) {
    // unlock process completed, both doors open
    sm->state = STATE_IDLE;
    SIGNAL(SIGNAL_CANCEL_TIMEOUT);
    SIGNAL(SIGNAL_OPEN_SUCCESSFUL);
    return;
  }

  if (is_door_lock_event(event, DOOR_ANY) && shack_state == SHACK_LOCKED && sm_state == STATE_WAIT_FOR_LOCKED) {
    // lock successful
    sm->state = STATE_IDLE;
    SIGNAL(SIGNAL_CANCEL_TIMEOUT);
    SIGNAL(SIGNAL_LOCK_SUCCESSFUL);
    return;
  }

  if (event == EVENT_DOORBELL_FRONT && shack_state == SHACK_OPEN) {
    // When the shack is open, ringing the door bell will unlock the front door.
    SIGNAL(SIGNAL_OPEN_DOOR_B);
    return;
  }

  if (event == EVENT_BUTTON_C2 && shack_state == SHACK_LOCKED) {
    // When shack is fully locked, you can leave the building through the back
    // by clicking the close button on C2 again (unlocks door C).
    SIGNAL(SIGNAL_OPEN_DOOR_C);
    return;
  }

  if (event == EVENT_BUTTON_B2 && shack_state == SHACK_LOCKED) {
    // When shack is fully locked, you can leave the building through the front
    // by clicking the close button on B2 again (unlocks door B).
    SIGNAL(SIGNAL_OPEN_DOOR_B);
    return;
  }

  if (event == EVENT_SSH_OPEN_FRONT_REQUEST && shack_state != SHACK_OPEN && sm_state == STATE_IDLE) {
    // When shack is closed and open is requested, let the user in and begin unlocking the door.
    sm->state = STATE_WAIT_FOR_OPEN_VIA_B;
    SIGNAL(SIGNAL_START_TIMEOUT);
    SIGNAL(SIGNAL_OPEN_DOOR_B2_SAFE);
    SIGNAL(SIGNAL_OPEN_DOOR_B);
    return;
  }

  if (event == EVENT_SSH_OPEN_BACK_REQUEST && shack_state != SHACK_OPEN && sm_state == STATE_IDLE) {
    // When shack is closed and open is requested, let the user in and begin unlocking the door.
    sm->state = STATE_WAIT_FOR_OPEN_VIA_C;
    SIGNAL(SIGNAL_START_TIMEOUT);
    SIGNAL(SIGNAL_OPEN_DOOR_C2_SAFE);
    SIGNAL(SIGNAL_OPEN_DOOR_C);
    return;
  }

  if (is_open_request(event) && shack_state == SHACK_OPEN && sm_state == STATE_IDLE) {
    // Transfer key ownership when the shack was successfully unlocked when no process is in action.
    SIGNAL(SIGNAL_CHANGE_KEYHOLDER);
    return;
  }

  if (is_close_request(event, shack_state) && shack_state != SHACK_LOCKED && sm_state == STATE_IDLE) {
    // Any close request immediatly triggers a closing process when nothing else is happening right now
    sm->state = STATE_WAIT_FOR_LOCKED;
    SIGNAL(SIGNAL_START_TIMEOUT);
    SIGNAL(SIGNAL_LOCK_ALL);
    return;
  }

  if (is_ssh_request(event) && sm_state != STATE_IDLE) {
    // we currently cannot take requests, as we're still performing a process
    SIGNAL(SIGNAL_CANNOT_HANDLE_REQUEST);
    return;
  }

  if (event == EVENT_SSH_CLOSE_REQUEST && shack_state == SHACK_LOCKED) {
    SIGNAL(SIGNAL_NO_STATE_CHANGE);
    return;
  }

  if (is_ssh_request(event)) {
    // we currently cannot take requests, as we're still performing a process
    SIGNAL(SIGNAL_CANNOT_HANDLE_REQUEST);
    return;
  }

#undef SIGNAL
}

enum ShackState sm_get_shack_state(struct StateMachine const * sm)
{
  if (sm->door_c2 == DOOR_UNOBSERVED || sm->door_b2 == DOOR_UNOBSERVED) {
    return SHACK_UNOBSERVED;
  }

  bool locked_b2 = (sm->door_b2 == DOOR_LOCKED);
  bool locked_c2 = (sm->door_c2 == DOOR_LOCKED);

  if (locked_b2 && locked_c2)
    return SHACK_LOCKED;
  else if (!locked_b2 && !locked_c2)
    return SHACK_OPEN;
  else if (!locked_c2)
    return SHACK_UNLOCKED_VIA_C2;
  else
    return SHACK_UNLOCKED_VIA_B2;
}

char const * sm_shack_state_name(enum ShackState state)
{
  if (state == SHACK_UNOBSERVED)
    return "unobserved";
  if (state == SHACK_OPEN)
    return "open";
  if (state == SHACK_UNLOCKED_VIA_B2)
    return "unlocked via b2";
  if (state == SHACK_UNLOCKED_VIA_C2)
    return "unlocked via c2";
  if (state == SHACK_LOCKED)
    return "locked";
  return "<<INVALID>>";
}

char const * sm_door_state_name(enum DoorState state)
{
  if (state == DOOR_LOCKED)
    return "locked";
  if (state == DOOR_UNOBSERVED)
    return "unobserved";
  if (state == DOOR_OPEN)
    return "open";
  if (state == DOOR_CLOSED)
    return "closed";
  return "<<INVALID>>";
}

char const * sm_state_name(struct StateMachine const * sm)
{
  if (sm->state == STATE_IDLE)
    return "idle";
  if (sm->state == STATE_WAIT_FOR_OPEN_VIA_B)
    return "wait for shack entry via B";
  if (sm->state == STATE_WAIT_FOR_OPEN_VIA_C)
    return "wait for shack entry via C";
  if (sm->state == STATE_WAIT_FOR_LOCKED)
    return "wait for shack locked";

  return "<<INVALID>>";
}
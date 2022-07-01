#include "state-machine.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>

#define TIMEOUT_BUTTON_PRESS   100   // ms (Duration of a motor button press)
#define TIMEOUT_ENGAGE_BOLT    20000 // ms (Typically twice the duration of the time needed to lock the door bolt)   TODO: Figure out the right timing
#define TIMEOUT_DISENGAGE_BOLT 20000 // ms (Typically twice the duration of the time needed to unlock the door bolt) TODO: Figure out the right timing
#define TIMEOUT_USER_OPEN      60000 // ms (Timeout for waiting until the door was opened after a OPEN event)
#define TIMEOUT_USER_CLOSE     30000 // ms (Timeout for waiting until a user closes the door after a CLOSE event)

// state machine states, description see set_state function!
enum State
{
  // other
  STATE_IDLE,

  // open process
  STATE_INIT_OPEN,
  STATE_WAIT_FOR_UNLOCKED,
  STATE_WAIT_FOR_OPEN,

  // close process
  STATE_WAIT_FOR_CLOSED,
  STATE_INIT_LOCK,
  STATE_WAIT_FOR_LOCKED,
};

static const char * const state_names[] = {
    [STATE_IDLE]              = "idle",
    [STATE_INIT_OPEN]         = "init open",
    [STATE_WAIT_FOR_UNLOCKED] = "wait for unlocked",
    [STATE_WAIT_FOR_OPEN]     = "wait for open",
    [STATE_WAIT_FOR_CLOSED]   = "wait for closed",
    [STATE_INIT_LOCK]         = "init lock",
    [STATE_WAIT_FOR_LOCKED]   = "wait for locked",
};

static char const * const door_state_names[] = {
    [DOOR_CLOSED] = "closed",
    [DOOR_LOCKED] = "locked",
    [DOOR_OPEN]   = "open",
    [DOOR_FAULT]  = "fault",
};

//! Changes the state of the state machine to `state`.
//! This triggers all the signals and events that need to happen when entering the new
//! state. We design the state machine as such that it is not necessary to know the old state.
static void set_state(struct StateMachine * sm, enum State state)
{
  log_print(LSS_LOGIC, LL_VERBOSE, "switching state machine from %s to %s", state_names[sm->logic_state], state_names[state]);
  sm->logic_state = state;
  switch (state) {
  // base state. waits for either CLOSE or OPEN requests
  // and does nothing else.
  case STATE_IDLE:
  {
    // when entering idle state, make sure no buttons are currently pressed
    sm->setIo(sm, IO_TRIGGER_CLOSE, false);
    sm->setIo(sm, IO_TRIGGER_OPEN, false);
    sm->setTimeout(sm, 0);
    sm->unsafe_action = false;
    break;
  }

  // Initializes the open process. Presses the motor button, then waits
  // for the button timeout.
  case STATE_INIT_OPEN:
  {
    sm->setIo(sm, IO_TRIGGER_OPEN, true);
    sm->setTimeout(sm, TIMEOUT_BUTTON_PRESS);
    sm->signal(sm, SIGNAL_OPENING);
    break;
  }

  // Waits for the door to enter DOOR_CLOSED state or timeout. Will signal
  // error on timeout as this is a failure path.
  case STATE_WAIT_FOR_UNLOCKED:
  {
    sm->setIo(sm, IO_TRIGGER_OPEN, false);
    sm->setTimeout(sm, TIMEOUT_DISENGAGE_BOLT);
    break;
  }

  // Wait for the door to enter DOOR_OPEN state or timeout. On timeout
  // will go into locking sequence, as nobody has entered the space.
  case STATE_WAIT_FOR_OPEN:
  {
    // this state waits for either a door change event or
    // a timeout.
    sm->setTimeout(sm, TIMEOUT_USER_OPEN);
    break;
  }

  // Waits until the door enters DOOR_CLOSED state or timeouts. On timeout
  // will signal "door still open", otherwise will start locking the door.
  case STATE_WAIT_FOR_CLOSED:
  {
    // this state waits for either a door change event or
    // a timeout.
    sm->setTimeout(sm, TIMEOUT_USER_CLOSE);
    sm->signal(sm, SIGNAL_WAIT_FOR_DOOR_CLOSED);
    break;
  }

  // Presses the CLOSE button on the motor and starts locking the door.
  // will go into WAIT_FOR_LOCKED after a certain time.
  case STATE_INIT_LOCK:
  {
    // We have now closed door and press the button. This state
    // ensures the button will be pressed.
    sm->setIo(sm, IO_TRIGGER_CLOSE, true);
    sm->setTimeout(sm, TIMEOUT_BUTTON_PRESS);
    break;
  }

  // Waits until the door enters DOOR_LOCKED state. Will signal an error on timeout
  // as the door is still open then.
  case STATE_WAIT_FOR_LOCKED:
  {
    // We came from STATE_INIT_LOCK and are now waiting for either a
    // change in the door state or receiving a timeout.
    // We also release the "close" button .
    sm->setIo(sm, IO_TRIGGER_CLOSE, false);
    sm->setTimeout(sm, TIMEOUT_ENGAGE_BOLT);
    break;
  }
  }
}

void sm_init(
    struct StateMachine *  sm,
    enum DoorState         inital_state,
    StateMachineSignal     signal,
    StateMachineSetTimeout setTimeout,
    StateMachineSetIo      setIo,
    void *                 user_data)
{
  assert(sm != NULL);
  assert(signal != NULL);
  assert(setTimeout != NULL);
  assert(setIo != NULL);

  *sm = (struct StateMachine){
      .door_state  = inital_state,
      .logic_state = STATE_IDLE,

      .user_data  = user_data,
      .signal     = signal,
      .setTimeout = setTimeout,
      .setIo      = setIo,
  };
}

void sm_change_door_state(struct StateMachine * sm, enum DoorState new_state)
{
  assert(sm != NULL);
  if (sm->door_state == new_state)
    return;

  log_print(LSS_LOGIC, LL_VERBOSE, "SM: door changed status from %s to %s.", door_state_names[sm->door_state], door_state_names[new_state]);

  enum DoorState old_state = sm->door_state;
  sm->door_state           = new_state;

  switch (sm->logic_state) {
  // states that do not expect event changes:
  case STATE_INIT_OPEN:
  case STATE_INIT_LOCK:
  {
    log_print(LSS_LOGIC, LL_WARNING, "unexpected door change event in state %u!", sm->logic_state);
    // TODO: Handle this gracefully
    break;
  }

  case STATE_IDLE:
  {
    // in idle state, we track the state of the locking bolt.
    // when it changes the state, we signal a manual locking process.

    if (new_state == DOOR_LOCKED) {
      // every transition to LOCKED is a manual lock operation
      sm->signal(sm, SIGNAL_DOOR_MANUALLY_LOCKED);
    }
    if (old_state == DOOR_LOCKED) {
      // every transition from LOCKED is a manual unlock operation
      sm->signal(sm, SIGNAL_DOOR_MANUALLY_UNLOCKED);
    }
    break;
  }

  case STATE_WAIT_FOR_UNLOCKED:
  {
    if (new_state == DOOR_CLOSED) {
      sm->signal(sm, SIGNAL_UNLOCKED);
      if (sm->unsafe_action) {
        // unsafe unlock: we're done here
        set_state(sm, STATE_IDLE);
      }
      else {
        // wait until a user opens the door in 60 seconds.
        // will lock door again if the door won't be opened
        set_state(sm, STATE_WAIT_FOR_OPEN);
      }
    }
    else {
      log_print(LSS_LOGIC, LL_WARNING, "unexpected door change event in state STATE_WAIT_FOR_UNLOCKED!");
    }
    break;
  }

  case STATE_WAIT_FOR_OPEN:
  {
    if (new_state == DOOR_OPEN) {
      sm->signal(sm, SIGNAL_OPENED);
      set_state(sm, STATE_IDLE);
    }
    else {
      log_print(LSS_LOGIC, LL_WARNING, "unexpected door change event in state STATE_WAIT_FOR_UNLOCKED!");
    }
    break;
  }

  case STATE_WAIT_FOR_CLOSED:
  {
    if (new_state == DOOR_CLOSED) {
      sm->signal(sm, SIGNAL_LOCKING);
      set_state(sm, STATE_INIT_LOCK);
    }
    else {
      log_print(LSS_LOGIC, LL_WARNING, "unexpected door change event in state STATE_WAIT_FOR_UNLOCKED!");
    }
    break;
  }

  case STATE_WAIT_FOR_LOCKED:
  {
    if (new_state == DOOR_LOCKED) {
      sm->signal(sm, SIGNAL_LOCKED);
      set_state(sm, STATE_IDLE);
    }
    else {
      log_print(LSS_LOGIC, LL_WARNING, "unexpected door change event in state STATE_WAIT_FOR_UNLOCKED!");
    }
    break;
  }
  }
}

enum PortalError sm_send_event(struct StateMachine * sm, enum PortalEvent event)
{
  assert(sm != NULL);
  switch (event) {
  case EVENT_OPEN_SAFE:
  case EVENT_OPEN_UNSAFE:
  {
    if (sm->logic_state != STATE_IDLE)
      return SM_ERR_IN_PROGRESS;

    // store this for later use:
    // if this variable is set, the 60 second wait till door is opened
    // is skipped and the shack will be unlocked without checking.
    sm->unsafe_action = (event == EVENT_OPEN_UNSAFE);

    switch (sm->door_state) {
    case DOOR_LOCKED:
      // The door is currently locked, initiate unlock sequence
      set_state(sm, STATE_INIT_OPEN);
      break;

    case DOOR_OPEN:
    case DOOR_CLOSED:
      // Door is already open
      break;

    case DOOR_FAULT:
      return SM_ERR_UNEXPECTED;
    }

    break;
  }
  case EVENT_CLOSE:
  {
    if (sm->logic_state != STATE_IDLE)
      return SM_ERR_IN_PROGRESS;

    switch (sm->door_state) {
    case DOOR_OPEN:
      // Door is currently open, but we want it to be locked.
      // switch to the waiting state for the door to be closing
      set_state(sm, STATE_WAIT_FOR_CLOSED);
      break;

    case DOOR_CLOSED:
      // The door is already closed, no need to wait for the door to close.
      // Just begin locking the door
      set_state(sm, STATE_INIT_LOCK);
      break;

    case DOOR_LOCKED:
      break; // we're done already

    case DOOR_FAULT:
      return SM_ERR_UNEXPECTED;
    }
    break;
  }
  case EVENT_TIMEOUT:
  {
    switch (sm->logic_state) {
    case STATE_IDLE:
    {
      log_print(LSS_LOGIC, LL_WARNING, "unexpected EVENT_TIMEOUT in state %u", sm->logic_state);
      break;
    }

    case STATE_INIT_OPEN:
    {
      // button press timeout, we continue
      set_state(sm, STATE_WAIT_FOR_UNLOCKED);
      break;
    }

    case STATE_WAIT_FOR_UNLOCKED:
    {
      // The door motor didn't open the door in time, so we signal an error to the user and
      // return.
      sm->signal(sm, SIGNAL_ERROR_OPENING);
      set_state(sm, STATE_IDLE);
      break;
    }

    case STATE_WAIT_FOR_OPEN:
    {
      // The user didn't open the door in time, so
      // we signal this to the system and start locking the door again.
      sm->signal(sm, SIGNAL_NO_ENTRY);
      set_state(sm, STATE_INIT_LOCK);
      break;
    }

    case STATE_WAIT_FOR_CLOSED:
    {
      // The user didn't close the door in time, even though we asked nicely.
      // Signal failure and return to idle state.
      sm->signal(sm, SIGNAL_CLOSE_TIMEOUT);
      set_state(sm, STATE_IDLE);
      break;
    }

    case STATE_INIT_LOCK:
    {
      // button press timeout, we continue
      set_state(sm, STATE_WAIT_FOR_LOCKED);
      break;
    }

    case STATE_WAIT_FOR_LOCKED:
    {
      // The door motor failed to lock the door in time. Signal an error to the user.
      set_state(sm, STATE_IDLE);
      sm->signal(sm, SIGNAL_ERROR_LOCKING);
      break;
    }
    }
    break;
  }
  }
  return SM_SUCCESS;
}

enum DoorState sm_compute_state(bool locked, bool open)
{
  return (enum DoorState)((((int)open) << 1) | (((int)locked) << 0));
}

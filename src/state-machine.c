#include "state-machine.h"
#include <assert.h>

#define TIMEOUT_ENGAGE_BOLT 10000 // ms TODO: Figure out the right timing
#define TIMEOUT_DISENGAGE_BOLT 10000 // ms TODO: Figure out the right timing
#define TIMEOUT_USER_OPEN 10000 // ms
#define TIMEOUT_ENGAGE_BOLT 10000 // ms


enum
{
  STATE_IDLE,
};

void sm_init(
  struct StateMachine *sm,
  StateMachineSignal signal,
  StateMachineSetTimeout setTimeout,
  void *user_data)
{
  assert(sm != NULL);
  assert(signal != NULL);
  assert(setTimeout != NULL);

  *sm = (struct StateMachine) {
    .door_state = -1, // unobserved
    .logic_state = STATE_IDLE,

    .user_data = user_data,
    .signal = signal,
    .setTimeout = setTimeout,
  };
}

void sm_change_door_state(struct StateMachine *sm, enum DoorState new_state)
{
  assert(sm != NULL);
  if(sm->door_state == new_state)
    return;
  switch(sm->logic_state) {
    // process new door state here
  }
}

enum PortalError sm_send_event(struct StateMachine *sm, enum PortalEvent event)
{
  assert(sm != NULL);
  switch(event) {
    case EVENT_OPEN: {
      if(sm->logic_state != STATE_IDLE)
        return SM_ERR_IN_PROGRESS;
      // TODO: Implement open start sequence
      break;
    }
    case EVENT_CLOSE: {
      if(sm->logic_state != STATE_IDLE)
        return SM_ERR_IN_PROGRESS;
      // TODO: Implement open start sequence
      break;
    }
    case EVENT_TIMEOUT: {
      // TODO: Validate and handle events
      break;
    }
  }
  return SM_SUCCESS;
}


enum DoorState sm_compute_state(bool locked, bool open)
{
  return (enum DoorState) ((((int)open) << 1) | (((int)locked) << 0) );
}
#include "portal300.h"
#include <freertos/FreeRTOS.h>
#include <freertos/atomic.h>

static volatile uint32_t atomic_door_signal = 0;

void signal_door_open()
{
  // will either set the signal to 1 or will keep it at 1
  Atomic_CompareAndSwap_u32(&atomic_door_signal, 1, 0);
}

bool was_door_signalled()
{
  // Will return 1 if the value swap to 0 happened, otherwise 0.
  return Atomic_CompareAndSwap_u32(&atomic_door_signal, 0, 1);
}
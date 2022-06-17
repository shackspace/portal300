#ifndef DOOR_H
#define DOOR_H

#include <stdbool.h>

void signal_door_open(void);

bool was_door_signalled(void);

#endif
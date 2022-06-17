#ifndef PORTAL300_GPIO_H
#define PORTAL300_GPIO_H

#include <stdbool.h>

void io_init(void);

void io_trigger_door_unlock(void);

bool io_was_doorbell_triggered();

#endif
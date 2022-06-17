#ifndef PORTAL300_IO_H
#define PORTAL300_IO_H

#include <stdbool.h>

typedef void (*IoInputChangedCallback)(void);

void io_init(IoInputChangedCallback callback);

void io_set_open(bool state);
void io_set_close(bool state);

bool io_get_locked(void);
bool io_get_closed(void);
bool io_get_button(void);

#endif
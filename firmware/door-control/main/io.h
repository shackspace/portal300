#ifndef PORTAL300_IO_H
#define PORTAL300_IO_H

#include <stdbool.h>
#include <stdint.h>

// predefined beep patterns:
// will be played LSB to MSB until no more bits are set.
// each bit is 200ms long, each set bit enables the beeper.
#define IO_SHORT_BEEP           0b1
#define IO_LONG_BEEP            0b111
#define IO_SHORT_BEEP_BEEP      0b101
#define IO_SHORT_BEEP_BEEP_BEEP 0b10101
#define IO_LONG_BEEP_BEEP       0b11100111
#define IO_LONG_BEEP_BEEP_BEEP  0b1110011100111
#define IO_BEEP_ERROR           0b11101010111

typedef void (*IoInputChangedCallback)(void);

void io_init(IoInputChangedCallback callback);

void io_set_open(bool state);
void io_set_close(bool state);
void io_beep(uint32_t pattern); // beeps the bit pattern from LSB to MSB. each set bit beeps 200 ms

bool io_get_button(void);

#endif
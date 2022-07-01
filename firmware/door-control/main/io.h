#ifndef PORTAL300_IO_H
#define PORTAL300_IO_H

#include <stdbool.h>
#include <stdint.h>

#define IO_SHORT_BEEP           0x01              // 200 ms
#define IO_LONG_BEEP            0x07              // 600 ms
#define IO_SHORT_BEEP_BEEP      0x05              // 200 ms on, off, on
#define IO_SHORT_BEEP_BEEP_BEEP 0b10101           // 200 ms on, off, on
#define IO_LONG_BEEP_BEEP       0xE7              // 600 ms on, 400 ms off, 600 ms on
#define IO_LONG_BEEP_BEEP_BEEP  0b111000111000111 // 600 ms on/off for 3 times
#define IO_BEEP_ERROR           0b11101010111

typedef void (*IoInputChangedCallback)(void);

void io_init(IoInputChangedCallback callback);

void io_set_open(bool state);
void io_set_close(bool state);
void io_beep(uint32_t pattern); // beeps the bit pattern from LSB to MSB. each set bit beeps 200 ms

bool io_get_button(void);

#endif
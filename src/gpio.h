#ifndef PORTAL300_GPIO_H
#define PORTAL300_GPIO_H

#include <stdbool.h>
#include <stdint.h>

enum GpioDirection {
  GPIO_DIR_IN,       /* Input */
  GPIO_DIR_OUT,      /* Output, initialized to low */
  GPIO_DIR_OUT_LOW,  /* Output, initialized to low */
  GPIO_DIR_OUT_HIGH, /* Output, initialized to high */
};

enum GpioEdge {
  GPIO_EDGE_NONE,    /* No interrupt edge */
  GPIO_EDGE_RISING,  /* Rising edge 0 -> 1 */
  GPIO_EDGE_FALLING, /* Falling edge 1 -> 0 */
  GPIO_EDGE_BOTH     /* Both edges X -> !X */
};

enum GpioBias {
  GPIO_BIAS_DEFAULT,   /* Default line bias */
  GPIO_BIAS_PULL_UP,   /* Pull-up */
  GPIO_BIAS_PULL_DOWN, /* Pull-down */
  GPIO_BIAS_DISABLE,   /* Disable line bias */
};

enum GpioDrive {
  GPIO_DRIVE_DEFAULT,     /* Default line drive (push-pull) */
  GPIO_DRIVE_OPEN_DRAIN,  /* Open drain */
  GPIO_DRIVE_OPEN_SOURCE, /* Open source */
};

struct GpioHandle {
  int fd;
};

struct GpioPin {
  struct GpioHandle *io;
  uint32_t index;
  int fd;
  bool inverted;
};

struct GpioConfig {
  enum GpioDirection direction;
  enum GpioEdge edge;
  enum GpioBias bias;
  enum GpioDrive drive;
  bool inverted;
  char label[31];
  uint32_t debounce_us;
};

bool gpio_validate_config(struct GpioConfig config);

bool gpio_open(struct GpioHandle *io, char const *chip);

void gpio_close(struct GpioHandle *io);

bool gpio_open_pin(struct GpioHandle *io, struct GpioPin *pin,
                   struct GpioConfig config, uint32_t line);

void gpio_close_pin(struct GpioPin *pin);

bool gpio_write(struct GpioPin pin, bool value);
bool gpio_read(struct GpioPin pin, bool *status);
bool gpio_get_event(struct GpioPin pin, enum GpioEdge *status);

int gpio_fd(struct GpioPin pin);

#endif

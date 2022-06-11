#define _XOPEN_SOURCE   600 /* for POLLRDNORM */

#include "gpio.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <errno.h>

#include <sys/ioctl.h>

#include <linux/gpio.h>
#include <assert.h>

bool gpio_open(struct GpioHandle *io, char const *chip)
{
    assert(io != NULL);
    assert(chip != NULL);

    int fd = open(chip, O_RDWR);
    if(fd < 0) {
        perror("failed to open gpiochip");
        return false;
    }

    /* Get chip info for number of lines */
    struct gpiochip_info chip_info = {0};
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0) {
        perror("failed to invoke GPIO_GET_CHIPINFO_IOCTL");
        goto failure;
    }

    fprintf(stderr, "opened gpio chip %s / %s with %u io lines\n",
        chip_info.name,
        chip_info.label,
        chip_info.lines
    );

    // /* Loop through every line */
    // struct gpioline_info line_info = {0};
    // unsigned int line;
    // for (line = 0; line < chip_info.lines; line++) {
    //     line_info.line_offset = line;

    //     /* Get the line info */
    //     if (ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &line_info) < 0) {
    //         int errsv = errno;
    //         close(fd);
    //         abort();
    //     }

    //     printf("line[%u] = { line_offset=%u name='%s' consumer='%s' flags=%u }\n", line, line_info.line_offset, line_info.name, line_info.consumer, line_info.flags);
    // }

    *io = (struct GpioHandle) {
        .fd = fd,
    };

    return true;
failure:
    close(fd);
    return false;
}

void gpio_close(struct GpioHandle *io)
{
    assert(io != NULL);
    close(io->fd);
    io->fd = -1;
}

bool gpio_validate_config(struct GpioConfig config)
{
    if (config.direction != GPIO_DIR_IN && config.direction != GPIO_DIR_OUT && config.direction != GPIO_DIR_OUT_LOW && config.direction != GPIO_DIR_OUT_HIGH) {
        fprintf(stderr, "Invalid GPIO direction (can be in, out, low, high)\n");
        return false;
    }

    if (config.edge != GPIO_EDGE_NONE && config.edge != GPIO_EDGE_RISING && config.edge != GPIO_EDGE_FALLING && config.edge != GPIO_EDGE_BOTH) {
        fprintf(stderr, "Invalid GPIO interrupt edge (can be none, rising, falling, both)\n");
        return false;
    }

    if (config.direction != GPIO_DIR_IN && config.edge != GPIO_EDGE_NONE) {
        fprintf(stderr, "Invalid GPIO edge for output GPIO\n");
        return false;
    }

    if (config.bias != GPIO_BIAS_DEFAULT && config.bias != GPIO_BIAS_PULL_UP && config.bias != GPIO_BIAS_PULL_DOWN && config.bias != GPIO_BIAS_DISABLE) {
        fprintf(stderr, "Invalid GPIO line bias (can be default, pull_up, pull_down, disable)\n");
        return false;
    }

    if (config.drive != GPIO_DRIVE_DEFAULT && config.drive != GPIO_DRIVE_OPEN_DRAIN && config.drive != GPIO_DRIVE_OPEN_SOURCE) {
        fprintf(stderr, "Invalid GPIO line drive (can be default, open_drain, open_source)\n");
        return false;
    }

    if (config.direction == GPIO_DIR_IN && config.drive != GPIO_DRIVE_DEFAULT) {
        fprintf(stderr, "Invalid GPIO line drive for input GPIO\n");
        return false;
    }

    return true;
}

static void copy_string(char * dest, size_t dest_size, char const * source, size_t source_size)
{
    memset(dest, 0, dest_size);
    strncpy(dest, source, (dest_size - 1) < source_size ? dest_size - 1 : source_size);
    dest[dest_size] = 0;
}

bool gpio_open_pin(struct GpioHandle * io, struct GpioPin * pin, struct GpioConfig config, uint32_t line)
{
    assert(io != NULL);
    assert(pin != NULL);

    if(!gpio_validate_config(config)) {
        return false;
    }

    *pin = (struct GpioPin) {
        .io = io,
        .index = line,
        .fd = -1,
        .inverted = config.inverted,
    };

    uint64_t flags = 0;

    if (config.bias == GPIO_BIAS_PULL_UP)
        flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    else if (config.bias == GPIO_BIAS_PULL_DOWN)
        flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    else if (config.bias == GPIO_BIAS_DISABLE)
        flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;

    if (config.drive == GPIO_DRIVE_OPEN_DRAIN)
        flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    else if (config.drive == GPIO_DRIVE_OPEN_SOURCE)
        flags |= GPIO_V2_LINE_FLAG_OPEN_SOURCE;
    
    if (config.inverted)
        flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;

    struct gpio_v2_line_request request = {0};
    request.offsets[0] = line;
    request.num_lines = 1;
    request.event_buffer_size = 0; // default
    request.config.flags = flags;
    copy_string(request.consumer, sizeof request.consumer, config.label, sizeof config.label);

    if(config.debounce_us > 0) {
        request.config.attrs[0] = (struct gpio_v2_line_config_attribute) {
            .attr = {
                .id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE,
                .padding = 0,
                .debounce_period_us = config.debounce_us,
            },
            .mask = ~0ULL,
        };
        request.config.num_attrs = 1;
    }

    if(config.direction == GPIO_DIR_IN)
    {
        request.config.flags |= GPIO_V2_LINE_FLAG_INPUT;
        switch(config.edge) {
            case GPIO_EDGE_NONE: break;
            case GPIO_EDGE_FALLING: request.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING; break;
            case GPIO_EDGE_RISING:  request.config.flags |= GPIO_V2_LINE_FLAG_EDGE_FALLING; break;
            case GPIO_EDGE_BOTH:    request.config.flags |= (GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING); break;
        }
    }
    else 
    {
        request.config.flags |= GPIO_V2_LINE_FLAG_OUTPUT;

        // TODO: Handle initial value
        // bool initial_value = (config.direction == GPIO_DIR_OUT_HIGH);
        // initial_value ^= config.inverted;

        // request.default_values[0] = initial_value;
    }

    if (ioctl(io->fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        perror("failed to open gpio");
        return false;
    }

    pin->fd = request.fd;

    return true;
}

void gpio_close_pin(struct GpioPin *pin)
{
    assert(pin != NULL);
    if(close(pin->fd) != -1) {
        perror("failed to close gpio pin handle");
    }
    pin->fd = -1;
}

bool gpio_write(struct GpioPin pin, bool value)
{
    assert(pin.io != NULL);
    assert(pin.fd != -1);

    struct gpio_v2_line_values data = {
        .mask = 1ULL,
        .bits = value ^ pin.inverted,
    };

    if (ioctl(pin.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &data) < 0) {
        perror("failed to write gpio");
        return false;
    }

    return true;
}

bool gpio_read(struct GpioPin pin, bool * status)
{
    assert(pin.io != NULL);
    assert(pin.fd != -1);
    assert(status != NULL);

    struct gpio_v2_line_values data = {
        .mask = ~0ULL,
        .bits = 0ULL,
    };
    if (ioctl(pin.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &data) < 0) {
        perror("failed to read gpio");
        return false;
    }

    *status = ((data.bits & data.mask) != 0) ^ pin.inverted;

    return true;
}


bool gpio_get_event(struct GpioPin pin, enum GpioEdge * edge)
{
    assert(pin.io != NULL);
    assert(pin.fd != -1);
    assert(edge != NULL);

    struct gpio_v2_line_event event_data = {0};
    if (read(pin.fd, &event_data, sizeof(event_data)) < (ssize_t)sizeof(event_data)) {
        perror("failed to read event from gpio");
        return false;
    }

    switch(event_data.id) {
        case GPIO_V2_LINE_EVENT_RISING_EDGE:  *edge = GPIO_EDGE_RISING;  break;
        case GPIO_V2_LINE_EVENT_FALLING_EDGE: *edge = GPIO_EDGE_FALLING; break;
        default:                              *edge = GPIO_EDGE_NONE;    break;
    }
    
    return true;
}

int gpio_fd(struct GpioPin pin)
{
    assert(pin.io != NULL);
    return pin.fd;
}
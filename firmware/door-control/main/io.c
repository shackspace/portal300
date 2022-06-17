#include "io.h"
#include "hal/gpio_types.h"

#include <esp_err.h>
#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/atomic.h>

#define PIN_OUT_OPEN   GPIO_NUM_15
#define PIN_OUT_CLOSE  GPIO_NUM_14
#define PIN_OUT_SIGNAL GPIO_NUM_12

#define PIN_IN_CLOSED GPIO_NUM_39
#define PIN_IN_LOCKED GPIO_NUM_36
#define PIN_IN_BUTTON GPIO_NUM_35

static const bool output_active_high = false;

static const gpio_config_t output_config = {
    .pin_bit_mask = (1ULL << PIN_OUT_OPEN) | (1ULL << PIN_OUT_CLOSE),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};

static const gpio_config_t input_config = {
    .pin_bit_mask = (1ULL << PIN_IN_CLOSED) | (1ULL << PIN_IN_LOCKED) | (1ULL << PIN_IN_BUTTON),
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_ANYEDGE,
};

static IoInputChangedCallback callback = NULL;

static void handle_gpio_interrupt(void * arg)
{
  (void)arg;
  if (callback != NULL) {
    callback();
  }
}

void io_init(IoInputChangedCallback callback)
{
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));

  ESP_ERROR_CHECK(gpio_config(&output_config));
  ESP_ERROR_CHECK(gpio_config(&input_config));

  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_CLOSED, handle_gpio_interrupt, NULL));
  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_LOCKED, handle_gpio_interrupt, NULL));
  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_BUTTON, handle_gpio_interrupt, NULL));

  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_CLOSE, !output_active_high));
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_OPEN, !output_active_high));
}

void io_set_open(bool state)
{
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_OPEN, state ^ output_active_high));
}

void io_set_close(bool state)
{
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_OPEN, state ^ output_active_high));
}

bool io_get_locked(void)
{
  return gpio_get_level(PIN_IN_CLOSED);
}

bool io_get_closed(void)
{
  return gpio_get_level(PIN_IN_LOCKED);
}

bool io_get_button(void)
{
  return gpio_get_level(PIN_IN_BUTTON);
}
#include "io.h"
#include "esp_log.h"
#include "hal/gpio_types.h"

#include <esp_err.h>
#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/atomic.h>

#include "door_config.h"

#define TAG "IO"

static const gpio_config_t beeper_output_config = {
    .pin_bit_mask = (1ULL << PIN_OUT_SIGNAL),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};

#if PIN_CFG_OPENDRAIN
static const bool          control_active_high   = false;
static const gpio_config_t control_output_config = {
    .pin_bit_mask = (1ULL << PIN_OUT_CLOSE) | (1ULL << PIN_OUT_OPEN),
    .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
#else
static const bool          control_active_high   = true;
static const gpio_config_t control_output_config = {
    .pin_bit_mask = (1ULL << PIN_OUT_CLOSE) | (1ULL << PIN_OUT_OPEN),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
#endif

static const gpio_config_t input_config = {
    .pin_bit_mask = (1ULL << PIN_IN_BUTTON) | (1ULL << PIN_IN_DOOR_CLOSED) | (1ULL << PIN_IN_DOOR_LOCKED),
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE, // We use pins without internal pull up/down
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_ANYEDGE,
};

static IoInputChangedCallback interrupt_callback = NULL;

static void handle_gpio_interrupt(void * arg)
{
  (void)arg;
  if (interrupt_callback != NULL) {
    interrupt_callback();
  }
}

void io_init(IoInputChangedCallback callback)
{
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));

  ESP_ERROR_CHECK(gpio_config(&control_output_config));
  ESP_ERROR_CHECK(gpio_config(&beeper_output_config));
  ESP_ERROR_CHECK(gpio_config(&input_config));

  interrupt_callback = callback;
  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_BUTTON, handle_gpio_interrupt, NULL));

  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_CLOSE, !control_active_high));
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_OPEN, !control_active_high));
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_SIGNAL, 0));
}

void io_set_open(bool state)
{
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_OPEN, state ^ (!control_active_high)));
}

void io_set_close(bool state)
{
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_CLOSE, state ^ (!control_active_high)));
}

void io_beep(uint32_t pattern)
{
  while (pattern != 0) {
    ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_SIGNAL, (pattern & 1)));
    vTaskDelay(200 / portTICK_PERIOD_MS);
    pattern >>= 1;
  }
  ESP_ERROR_CHECK(gpio_set_level(PIN_OUT_SIGNAL, 0));
}

bool io_get_button(void)
{
  return gpio_get_level(PIN_IN_BUTTON);
}

bool io_get_door_closed(void)
{
  return !gpio_get_level(PIN_IN_DOOR_CLOSED);
}

bool io_get_door_locked(void)
{
  return !gpio_get_level(PIN_IN_DOOR_LOCKED);
}

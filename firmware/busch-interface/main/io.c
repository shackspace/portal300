#include "io.h"
#include "esp_intr_alloc.h"

#include <driver/gpio.h>
#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <freertos/atomic.h>
#include <freertos/task.h>

#define PIN_BUTTON   GPIO_NUM_14
#define PIN_DOORBELL GPIO_NUM_15

static const bool button_active_high = true;

static const gpio_config_t button_config = {
    .pin_bit_mask = (1ULL << PIN_BUTTON),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};

static const gpio_config_t doorbell_config = {
    .pin_bit_mask = (1ULL << PIN_DOORBELL),
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_NEGEDGE,
};

static volatile uint32_t atomic_doorbell_signal = 0;

static void handle_gpio_interrupt(void * arg)
{
  (void)arg;

  // TODO: Debounce this event here!

  if (gpio_get_level(PIN_DOORBELL) == false) {
    // will either set the signal to 1 or will keep it at 1
    Atomic_CompareAndSwap_u32(&atomic_doorbell_signal, 1, 0);
  }
}

void io_init()
{
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));

  ESP_ERROR_CHECK(gpio_config(&button_config));
  ESP_ERROR_CHECK(gpio_config(&doorbell_config));

  ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_DOORBELL, handle_gpio_interrupt, NULL));

  ESP_ERROR_CHECK(gpio_set_level(PIN_BUTTON, !button_active_high));
}

void io_trigger_door_unlock(void)
{
  ESP_ERROR_CHECK(gpio_set_level(PIN_BUTTON, button_active_high));
  vTaskDelay(100 / portTICK_PERIOD_MS); // sleep for 100ms
  ESP_ERROR_CHECK(gpio_set_level(PIN_BUTTON, !button_active_high));
}

bool io_was_doorbell_triggered()
{
  // Will return 1 if the value swap to 0 happened, otherwise 0.
  return Atomic_CompareAndSwap_u32(&atomic_doorbell_signal, 0, 1);
}

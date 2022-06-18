#include "door.h"
#include "io.h"
#include "portal300.h"
#include <portal300.h>
#include <portal300/ethernet.h>
#include <portal300/mqtt.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/////////////////////////////////////////////////////////////////////
// Configuration:

// The door we are attached to
#define CURRENT_DOOR DOOR_B

#define CURRENT_DEVICE BUSCH_INTERFACE

/////////////////////////////////////////////////////////////////////

#define STATUS_TOPIC_INNER(_Name) PORTAL300_TOPIC_STATUS_##_Name
#define STATUS_TOPIC(_Name)       STATUS_TOPIC_INNER(_Name)

#define TAG "application logic"

static const char client_crt[] =
#include "certs/client.crt.h"
    ;
static const char client_key[] =
#include "certs/client.key.h"
    ;
static const char ca_crt[] =
#include "certs/ca.crt.h"
    ;

static void on_mqtt_connect(void);
static void on_mqtt_data_received(struct MqttEvent const * event);

static const struct MqttConfig mqtt_config = {
    .host_name = "mqtt.portal.shackspace.de",
    .client_id = "portal.interface.busch",

    .device_status_topic = STATUS_TOPIC(CURRENT_DEVICE),

    .client_crt = client_crt,
    .client_key = client_key,
    .ca_crt     = ca_crt,

    .on_connect = on_mqtt_connect,
    .on_data    = on_mqtt_data_received,
};

void app_main(void)
{
  io_init();
  ethernet_init();
  mqtt_init(&mqtt_config);

  while (1) {
    if (io_was_doorbell_triggered()) {
      if (is_mqtt_connected()) {
        ESP_LOGI(TAG, "The doorbell was rang, sending MQTT message.");
        mqtt_pub(PORTAL300_TOPIC_EVENT_DOORBELL, DOOR_NAME(CURRENT_DOOR));
      }
      else {
        ESP_LOGI(TAG, "The doorbell was rang, but there's nobody here to listen.");
      }
    }

    if (was_door_signalled()) {
      ESP_LOGI(TAG, "MQTT message to unlock front door received. Opening door...");
      io_trigger_door_unlock();
    }

    vTaskDelay(100);
  }
}

static void on_mqtt_connect(void)
{
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR);
}

static void on_mqtt_data_received(struct MqttEvent const * event)
{
  if (mqtt_event_has_topic(event, PORTAL300_TOPIC_ACTION_OPEN_DOOR)) {
    if (mqtt_event_has_data(event, DOOR_NAME(CURRENT_DOOR))) {
      signal_door_open();
    }
  }
  else {
    ESP_LOGW(TAG, "Unhandled MQTT topic(%.*s) received '%.*s'\n", (int)event->topic_len, (char const *)event->topic, (int)event->data_len, (char const *)event->data);
  }
}
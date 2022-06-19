#include <portal300/mqtt.h>
#include <portal300/ethernet.h>
#include <portal300.h>
#include "freertos/portmacro.h"
#include "freertos/projdefs.h"
#include "io.h"
#include "portal300.h"
#include "state-machine.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

/////////////////////////////////////////////////////////////////////
// Configuration:

// The door we are attached to
#define CURRENT_DOOR         DOOR_B2
#define CURRENT_DEVICE       DOOR_CONTROL_B2
#define BUTTON_DEBOUNCE_TIME 1000 // ms, can be pretty high for less button mashing

/////////////////////////////////////////////////////////////////////

#define MERGE_TOPIC_INNER(_Space, _Name) _Space##_Name

#define SYSTEM_STATUS_TOPIC(_Name) MERGE_TOPIC_INNER(PORTAL300_TOPIC_STATUS_, _Name)

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
    .client_id = "portal.control." DOOR_NAME(CURRENT_DOOR),

    .device_status_topic = SYSTEM_STATUS_TOPIC(CURRENT_DEVICE),

    .client_crt = client_crt,
    .client_key = client_key,
    .ca_crt     = ca_crt,

    .on_connect = on_mqtt_connect,
    .on_data    = on_mqtt_data_received,
};

static TaskHandle_t timeout_task = NULL;

static void statemachine_signal(struct StateMachine * sm,
                                enum PortalSignal     signal);
static void statemachine_setTimeout(struct StateMachine * sm, uint32_t ms);
static void statemachine_setIo(struct StateMachine * sm, enum PortalIo io, bool active);
static void io_changed_level(void);

static const EventBits_t  EVENT_SM_TIMEOUT_BIT    = (1 << 0);
static const EventBits_t  EVENT_SM_IO_CHANGED_BIT = (1 << 1);
static const EventBits_t  EVENT_SM_CLOSE_REQUEST  = (1 << 2);
static const EventBits_t  EVENT_SM_OPEN_REQUEST   = (1 << 3);
static EventGroupHandle_t event_group             = NULL;

static enum DoorState get_door_state()
{
  return sm_compute_state(
      io_get_locked(),
      !io_get_closed());
}

static enum PortalError log_sm_error(enum PortalError err)
{
  if (err != SM_SUCCESS) {
    char const * msg;
    switch (err) {
    case SM_SUCCESS: __builtin_unreachable();
    case SM_ERR_IN_PROGRESS: msg = "in progress"; break;
    case SM_ERR_UNEXPECTED: msg = "unexpected"; break;
    default: msg = "Unknown"; break;
    }
    ESP_LOGE(TAG, "StateMachine signalled error: %s", msg);
  }
  return err;
}

static void publish_door_status(enum DoorState state)
{
  char const * state_msg = "invalid";
  switch (state) {
  case DOOR_OPEN: state_msg = PORTAL300_STATUS_DOOR_OPENED; break;
  case DOOR_LOCKED: state_msg = PORTAL300_STATUS_DOOR_LOCKED; break;
  case DOOR_CLOSED: state_msg = PORTAL300_STATUS_DOOR_CLOSED; break;
  case DOOR_FAULT: state_msg = "fault"; break;
  }
  mqtt_pub(PORTAL300_TOPIC_STATUS_DOOR(CURRENT_DOOR), state_msg);
}

void app_main(void)
{
  event_group = xEventGroupCreate();

  io_init(io_changed_level);

  ethernet_init();
  mqtt_init(&mqtt_config);

  enum DoorState door_state = get_door_state();

  struct StateMachine core_logic;
  sm_init(&core_logic,
          door_state,
          statemachine_signal,
          statemachine_setTimeout,
          statemachine_setIo,
          NULL);

  TickType_t last_button_press_time = xTaskGetTickCount();
  bool       button_pressed         = false;

  while (1) {
    EventBits_t const current_state = xEventGroupWaitBits(
        event_group,
        EVENT_SM_TIMEOUT_BIT | EVENT_SM_IO_CHANGED_BIT | EVENT_SM_CLOSE_REQUEST | EVENT_SM_OPEN_REQUEST,
        pdTRUE,                  // clear on return
        pdFALSE,                 // return as soon as one bit was set
        100 / portTICK_PERIOD_MS // wait some time
    );

    if (current_state != 0) {
      ESP_LOGI(TAG, "Events happened: %02X", current_state);
    }

    // always forward pin changes
    if (current_state & EVENT_SM_IO_CHANGED_BIT) {
      enum DoorState new_state = get_door_state();

      if (new_state != door_state) {
        door_state = new_state;
        publish_door_status(door_state);

        char const * state = "unknown";
        switch (door_state) {
        case DOOR_CLOSED: state = "closed"; break;
        case DOOR_LOCKED: state = "locked"; break;
        case DOOR_OPEN: state = "open"; break;
        case DOOR_FAULT: state = "fault"; break;
        }
        ESP_LOGI(TAG, "forwarding io pin change to door status locked=%u, closed=%u, state=%s", io_get_locked(), io_get_closed(), state);
        sm_change_door_state(&core_logic, door_state);
      }

      bool btn_state = io_get_button();
      if (btn_state != button_pressed) {
        button_pressed = btn_state;
        if (button_pressed) {

          TickType_t current_time = xTaskGetTickCount();

          if (current_time - last_button_press_time >= BUTTON_DEBOUNCE_TIME / portTICK_PERIOD_MS) {
            last_button_press_time = current_time;

            mqtt_pub(PORTAL300_TOPIC_EVENT_CLOSE_REQUEST, DOOR_NAME(CURRENT_DOOR));
          }
        }
      }
    }

    if (current_state & EVENT_SM_TIMEOUT_BIT) {
      ESP_LOGI(TAG, "forwarding timeout to state machine.");
      log_sm_error(sm_send_event(&core_logic, EVENT_TIMEOUT));
    }
    if (current_state & EVENT_SM_CLOSE_REQUEST) {
      ESP_LOGI(TAG, "forwarding open request to state machine.");
      log_sm_error(sm_send_event(&core_logic, EVENT_CLOSE));
    }
    if (current_state & EVENT_SM_OPEN_REQUEST) {
      ESP_LOGI(TAG, "forwarding open request to state machine.");
      log_sm_error(sm_send_event(&core_logic, EVENT_OPEN));
    }
  }
}

static void on_mqtt_connect(void)
{
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR);
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_CLOSE_DOOR);

  enum DoorState door_state = get_door_state();
  publish_door_status(door_state);
}

static void on_mqtt_data_received(struct MqttEvent const * event)
{
  if (mqtt_event_has_topic(event, PORTAL300_TOPIC_ACTION_OPEN_DOOR)) {
    if (mqtt_event_has_data(event, DOOR_NAME(CURRENT_DOOR))) {
      ESP_LOGI(TAG, "Received open command");
      xEventGroupSetBits(event_group, EVENT_SM_OPEN_REQUEST);
    }
  }
  else if (mqtt_event_has_topic(event, PORTAL300_TOPIC_ACTION_CLOSE_DOOR)) {
    if (mqtt_event_has_data(event, DOOR_NAME(CURRENT_DOOR))) {
      ESP_LOGI(TAG, "Received close command");
      xEventGroupSetBits(event_group, EVENT_SM_CLOSE_REQUEST);
    }
  }
  else {
    ESP_LOGW(TAG, "Unhandled MQTT topic(%.*s) received '%.*s'\n", (int)event->topic_len, (char const *)event->topic, (int)event->data_len, (char const *)event->data);
  }
}
static void statemachine_signal(struct StateMachine * sm, enum PortalSignal signal)
{
  switch (signal) {
  case SIGNAL_OPENING: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_OPENING"); break;
  case SIGNAL_LOCKING: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_LOCKING"); break;
  case SIGNAL_UNLOCKED: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_UNLOCKED"); break;
  case SIGNAL_OPENED: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_OPENED"); break;
  case SIGNAL_NO_ENTRY: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_NO_ENTRY"); break;
  case SIGNAL_LOCKED: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_LOCKED"); break;
  case SIGNAL_ERROR_LOCKING: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_ERROR_LOCKING"); break;
  case SIGNAL_ERROR_OPENING: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_ERROR_OPENING"); break;
  case SIGNAL_CLOSE_TIMEOUT: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_CLOSE_TIMEOUT"); break;
  case SIGNAL_WAIT_FOR_DOOR_CLOSED: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_WAIT_FOR_DOOR_CLOSED"); break;
  case SIGNAL_DOOR_MANUALLY_UNLOCKED: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_DOOR_MANUALLY_UNLOCKED"); break;
  case SIGNAL_DOOR_MANUALLY_LOCKED: ESP_LOGI(TAG, "StateMachine signal: SIGNAL_DOOR_MANUALLY_LOCKED"); break;
  }
}

static void io_changed_level(void)
{
  xEventGroupSetBits(event_group, EVENT_SM_IO_CHANGED_BIT);
}

static void statemachine_triggerTimeout(void * param)
{
  uint32_t ms = (uint32_t)param;
  ESP_LOGI(TAG, "Waiting for %u ms until timeout", ms);
  vTaskDelay(ms / portTICK_PERIOD_MS);
  ESP_LOGI(TAG, "%u ms elapsed, triggering timeout", ms);
  xEventGroupSetBits(event_group, EVENT_SM_TIMEOUT_BIT);
  TaskHandle_t self = timeout_task;
  timeout_task      = NULL;
  vTaskDelete(self);
}

static void statemachine_setTimeout(struct StateMachine * sm, uint32_t ms)
{
  if (ms == 0) {
    // cancel timeout
    if (timeout_task != NULL) {
      vTaskDelete(timeout_task);
    }
    timeout_task = NULL;
  }
  else {
    // prime timer
    xTaskCreate(statemachine_triggerTimeout, "State Machine Delay", 4096, (void *)ms, tskIDLE_PRIORITY, &timeout_task);
  }
}

static void statemachine_setIo(struct StateMachine * sm, enum PortalIo io, bool active)
{
  ESP_LOGI(TAG, "change pin %s to %u", (io == IO_TRIGGER_CLOSE) ? "close" : "open", active);
  (void)sm;
  switch (io) {
  case IO_TRIGGER_CLOSE: io_set_close(active); break;
  case IO_TRIGGER_OPEN: io_set_open(active); break;
  }
}
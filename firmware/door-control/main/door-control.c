#include <portal300/mqtt.h>
#include <portal300/ethernet.h>
#include <portal300.h>
#include "esp_system.h"
#include "io.h"
#include "portal300.h"
#include "state-machine.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <assert.h>

/////////////////////////////////////////////////////////////////////
// Configuration:

#include "door_config.h"

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

static void statemachine_signal(struct StateMachine * sm, enum PortalSignal signal);
static void statemachine_setTimeout(struct StateMachine * sm, uint32_t ms);
static void statemachine_setIo(struct StateMachine * sm, enum PortalIo io, bool active);
static void io_changed_level(void);

static const EventBits_t  EVENT_SM_TIMEOUT_BIT         = (1 << 0);
static const EventBits_t  EVENT_SM_IO_CHANGED_BIT      = (1 << 1);
static const EventBits_t  EVENT_SM_CLOSE_REQUEST       = (1 << 2);
static const EventBits_t  EVENT_SM_SAFE_OPEN_REQUEST   = (1 << 3);
static const EventBits_t  EVENT_SM_UNSAFE_OPEN_REQUEST = (1 << 4);
static EventGroupHandle_t event_group                  = NULL;

static char const * door_state_to_string(enum DoorState door_state)
{
  switch (door_state) {
  case DOOR_CLOSED: return "closed"; break;
  case DOOR_LOCKED: return "locked"; break;
  case DOOR_OPEN: return "open"; break;
  case DOOR_FAULT: return "fault"; break;
  }
  return "unknown";
}

static enum DoorState classify_sensor_data(void)
{
  if (io_get_door_closed()) {
    // single locked: debug/sensor/magnetometer/raw -24.03	21.03	-57.48
    // double locked: debug/sensor/magnetometer/raw -54.07	30.04	-90.75
    // if (vec->x < -30 && vec->y > 20 && vec->z < -60) {
    if (io_get_door_locked()) {
      return DOOR_LOCKED;
    }
    else {
      return DOOR_CLOSED;
    }
  }
  else {
    return DOOR_OPEN;
  }
}

#define SMOOTH_DOOR_STATE_SIZE 8
static size_t  smooth_door_state_i                       = 0;
enum DoorState smooth_door_state[SMOOTH_DOOR_STATE_SIZE] = {DOOR_OPEN, DOOR_CLOSED, DOOR_LOCKED, DOOR_FAULT};

static void push_door_state(enum DoorState state)
{
  smooth_door_state[smooth_door_state_i] = state;
  smooth_door_state_i += 1;
  if (smooth_door_state_i >= SMOOTH_DOOR_STATE_SIZE) {
    smooth_door_state_i = 0;
  }
}

static enum DoorState get_smoothed_door_state(bool * ok)
{
  assert(ok != NULL);
  *ok                   = false;
  enum DoorState filter = smooth_door_state[0];
  for (size_t i = 1; i < SMOOTH_DOOR_STATE_SIZE; i++) {
    if (smooth_door_state[i] != filter)
      return DOOR_FAULT;
  }
  *ok = true;
  return filter;
}

static enum DoorState get_door_state(bool * value_sane)
{
  assert(value_sane != NULL);

  char buffer[256];

  // and classify the sliding average instead of the current reading,
  // so we have a stable sensor output.
  const enum DoorState fresh_state = classify_sensor_data();

  push_door_state(fresh_state);

  enum DoorState result_state = get_smoothed_door_state(value_sane);

#ifdef DEBUG_BUILD
  snprintf(buffer, sizeof buffer, "ok: %d\tclosed: %d\tlocked: %d\tclassification: %s", *value_sane, io_get_door_closed(), io_get_door_locked(), door_state_to_string(result_state));
  mqtt_pub("debug/sensor/doors", buffer);
#endif

  return result_state;
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
  if (!mqtt_pub(PORTAL300_TOPIC_STATUS_DOOR(CURRENT_DOOR), state_msg)) {
    abort();
  }

#ifdef DEBUG_BUILD
  char buffer[] = "Closed: X, Locked: Y";
  buffer[8]     = io_get_door_closed() ? '1' : '0';
  buffer[19]    = io_get_door_locked() ? '1' : '0';
  mqtt_pub("debug/sensor/door_button", buffer);
#endif
}

void app_main(void)
{
  event_group = xEventGroupCreate();

  io_init(io_changed_level);

  ethernet_init("door_control_" CURRENT_DOOR); // CURRENT_DOOR is a string literal alias
  mqtt_init(&mqtt_config);

  enum DoorState door_state;
  {
    int timeout = 100;
    while (true) {
      bool system_stable;
      door_state = get_door_state(&system_stable);
      if (system_stable)
        break;
      vTaskDelay(10 / portTICK_PERIOD_MS); // 100 Hz sampling rate for now
      if (timeout == 0) {
        ESP_LOGE(TAG, "System is not in a magnetic stable state. Rebooting...");
        esp_restart();
      }
      timeout -= 1;
    }
  }

  struct StateMachine core_logic;
  sm_init(&core_logic,
          door_state,
          statemachine_signal,
          statemachine_setTimeout,
          statemachine_setIo,
          NULL);

  TickType_t last_button_press_time = xTaskGetTickCount();
  bool       button_pressed         = false;

  io_beep(IO_SHORT_BEEP_BEEP);

  int force_refresh_timeout = 0;

  while (1) {
    EventBits_t const current_state = xEventGroupWaitBits(
        event_group,
        EVENT_SM_TIMEOUT_BIT | EVENT_SM_IO_CHANGED_BIT | EVENT_SM_CLOSE_REQUEST | EVENT_SM_SAFE_OPEN_REQUEST | EVENT_SM_UNSAFE_OPEN_REQUEST,
        pdTRUE,                  // clear on return
        pdFALSE,                 // return as soon as one bit was set
        100 / portTICK_PERIOD_MS // wait some time
    );

    if (current_state != 0) {
      ESP_LOGI(TAG, "Events happened: %02X", current_state);
    }

    // React to button press
    if (current_state & EVENT_SM_IO_CHANGED_BIT) {
      bool btn_state = io_get_button();
      if (btn_state != button_pressed) {
        button_pressed = btn_state;
        if (button_pressed) {

          TickType_t current_time = xTaskGetTickCount();

          if (current_time - last_button_press_time >= BUTTON_DEBOUNCE_TIME / portTICK_PERIOD_MS) {
            last_button_press_time = current_time;

            if (!mqtt_pub(PORTAL300_TOPIC_EVENT_BUTTON, DOOR_NAME(CURRENT_DOOR))) {
              abort();
            }
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
    if (current_state & EVENT_SM_SAFE_OPEN_REQUEST) {
      ESP_LOGI(TAG, "forwarding safe-open request to state machine.");
      log_sm_error(sm_send_event(&core_logic, EVENT_OPEN_SAFE));
    }
    if (current_state & EVENT_SM_UNSAFE_OPEN_REQUEST) {
      ESP_LOGI(TAG, "forwarding unsafe-open request to state machine.");
      log_sm_error(sm_send_event(&core_logic, EVENT_OPEN_UNSAFE));
    }

    // Re-compute door changes with measurements:
    {
      bool           system_stable;
      enum DoorState new_state = get_door_state(&system_stable);

      // Check if we have to force-send a door status update
      bool force_update = (force_refresh_timeout <= 0);
      if (!force_update) {
        // if not, we decrement the loop counter
        force_refresh_timeout -= 1;
      }

      // Only send door status updates when the magnetic flux is stable
      // and the state changed (or we have a periodic forced update)
      if (system_stable && ((new_state != door_state) || force_update)) {
        door_state = new_state;
        publish_door_status(door_state);

        char const * state = door_state_to_string(door_state);
        ESP_LOGI(TAG, "forwarding door state change to state machine (state=%s)", state);
        sm_change_door_state(&core_logic, door_state);

        force_refresh_timeout = FORCED_STATUS_UPDATE_PERIOD;
      }
      else if (!system_stable) {
        ESP_LOGW(TAG, "measurement unstable, no door status update");
      }
    }
  }
}

static void on_mqtt_connect(void)
{
  if (!mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE)) {
    abort();
  }
  if (!mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE)) {
    abort();
  }
  if (!mqtt_subscribe(PORTAL300_TOPIC_ACTION_LOCK_DOOR)) {
    abort();
  }
}

static void on_mqtt_data_received(struct MqttEvent const * event)
{
  if (mqtt_event_has_topic(event, PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE)) {
    if (mqtt_event_has_data(event, DOOR_NAME(CURRENT_DOOR))) {
      ESP_LOGI(TAG, "Received safe open command");
      xEventGroupSetBits(event_group, EVENT_SM_SAFE_OPEN_REQUEST);
    }
  }
  if (mqtt_event_has_topic(event, PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE)) {
    if (mqtt_event_has_data(event, DOOR_NAME(CURRENT_DOOR))) {
      ESP_LOGI(TAG, "Received unsafe open command");
      xEventGroupSetBits(event_group, EVENT_SM_UNSAFE_OPEN_REQUEST);
    }
  }
  else if (mqtt_event_has_topic(event, PORTAL300_TOPIC_ACTION_LOCK_DOOR)) {
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
  case SIGNAL_OPENING:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_OPENING");
    io_beep(IO_SHORT_BEEP);
    break;

  case SIGNAL_LOCKING:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_LOCKING");
    io_beep(IO_SHORT_BEEP);
    break;

  case SIGNAL_UNLOCKED:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_UNLOCKED");
    io_beep(IO_SHORT_BEEP_BEEP);
    break;

  case SIGNAL_OPENED:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_OPENED");
    io_beep(IO_SHORT_BEEP);
    break;

  case SIGNAL_NO_ENTRY:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_NO_ENTRY");
    io_beep(IO_LONG_BEEP_BEEP_BEEP);
    break;

  case SIGNAL_LOCKED:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_LOCKED");
    io_beep(IO_SHORT_BEEP_BEEP);
    break;

  case SIGNAL_ERROR_LOCKING:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_ERROR_LOCKING");
    io_beep(IO_BEEP_ERROR);
    break;

  case SIGNAL_ERROR_OPENING:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_ERROR_OPENING");
    io_beep(IO_BEEP_ERROR);
    break;

  case SIGNAL_CLOSE_TIMEOUT:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_CLOSE_TIMEOUT");
    break;

  case SIGNAL_WAIT_FOR_DOOR_CLOSED:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_WAIT_FOR_DOOR_CLOSED");
    io_beep(IO_SHORT_BEEP_BEEP_BEEP);
    break;

  case SIGNAL_DOOR_MANUALLY_UNLOCKED:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_DOOR_MANUALLY_UNLOCKED");
    io_beep(IO_SHORT_BEEP);
    break;

  case SIGNAL_DOOR_MANUALLY_LOCKED:
    ESP_LOGI(TAG, "StateMachine signal: SIGNAL_DOOR_MANUALLY_LOCKED");
    io_beep(IO_SHORT_BEEP);
    break;
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
  timeout_task = NULL;
  vTaskDelete(NULL);
}

static void statemachine_setTimeout(struct StateMachine * sm, uint32_t ms)
{
  // cancel timeout in all cases:
  // - when ms==0, we have to stop the current timeout
  // - when ms!=0, we still have to cancel a potentially active timeout, so
  //   we don't get a race condition because the new timeout will be started,
  //   but the old timeout is also active.
  if (timeout_task != NULL) {
    ESP_LOGI(TAG, "Killing currently active timeout");
    vTaskDelete(timeout_task);
  }
  timeout_task = NULL;

  if (ms != 0) {
    // prime timer
    ESP_LOGI(TAG, "Starting new timeout with %u ms.", ms);
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
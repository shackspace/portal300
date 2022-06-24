#include <portal300/mqtt.h>
#include <portal300/ethernet.h>
#include <portal300.h>
#include "io.h"
#include "portal300.h"
#include "state-machine.h"
#include "mlx90393.h"

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

static const EventBits_t  EVENT_SM_TIMEOUT_BIT         = (1 << 0);
static const EventBits_t  EVENT_SM_IO_CHANGED_BIT      = (1 << 1);
static const EventBits_t  EVENT_SM_CLOSE_REQUEST       = (1 << 2);
static const EventBits_t  EVENT_SM_SAFE_OPEN_REQUEST   = (1 << 3);
static const EventBits_t  EVENT_SM_UNSAFE_OPEN_REQUEST = (1 << 4);
static EventGroupHandle_t event_group                  = NULL;

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
  struct MLX90393 mlx;

  if (!mlx90393_init(&mlx, MLX90393_DEFAULT_ADDR))
    abort();

  mlx90393_setGain(&mlx, MLX90393_GAIN_2_5X);
  // You can check the gain too
  printf("Gain set to: ");
  switch (mlx90393_getGain(&mlx)) {
  case MLX90393_GAIN_1X: printf("1 x\n"); break;
  case MLX90393_GAIN_1_33X: printf("1.33 x\n"); break;
  case MLX90393_GAIN_1_67X: printf("1.67 x\n"); break;
  case MLX90393_GAIN_2X: printf("2 x\n"); break;
  case MLX90393_GAIN_2_5X: printf("2.5 x\n"); break;
  case MLX90393_GAIN_3X: printf("3 x\n"); break;
  case MLX90393_GAIN_4X: printf("4 x\n"); break;
  case MLX90393_GAIN_5X: printf("5 x\n"); break;
  }

  // Set resolution, per axis
  mlx90393_setResolution(&mlx, MLX90393_X, MLX90393_RES_19);
  mlx90393_setResolution(&mlx, MLX90393_Y, MLX90393_RES_19);
  mlx90393_setResolution(&mlx, MLX90393_Z, MLX90393_RES_16);

  // Set oversampling
  mlx90393_setOversampling(&mlx, MLX90393_OSR_2);

  // Set digital filtering
  mlx90393_setFilter(&mlx, MLX90393_FILTER_6);

  while (true) {
    float x, y, z;
    if (mlx90393_readData(&mlx, &x, &y, &z)) {
      printf("x=%.2f\ty=%.2f\tz=%.2f\n", x, y, z);
    }
    else {
      printf("failed to read sensor\n");
    }
  }
}

void app_main_wirklich(void)
{
  event_group = xEventGroupCreate();

  io_init(io_changed_level);

  ethernet_init("door_control_" CURRENT_DOOR); // CURRENT_DOOR is a string literal alias
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

  io_beep(IO_SHORT_BEEP_BEEP);

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

            mqtt_pub(PORTAL300_TOPIC_EVENT_BUTTON, DOOR_NAME(CURRENT_DOOR));
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
  }
}

static void on_mqtt_connect(void)
{
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE);
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE);
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_LOCK_DOOR);

  enum DoorState door_state = get_door_state();
  publish_door_status(door_state);
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
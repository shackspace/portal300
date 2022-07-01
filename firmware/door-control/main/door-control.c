#include <portal300/mqtt.h>
#include <portal300/ethernet.h>
#include <portal300.h>
#include "esp_system.h"
#include "io.h"
#include "portal300.h"
#include "state-machine.h"
#include "mlx90393.h"

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

// The door we are attached to
#define CURRENT_DOOR                DOOR_B2
#define CURRENT_DEVICE              DOOR_CONTROL_B2
#define BUTTON_DEBOUNCE_TIME        1000 // ms, can be pretty high for less button mashing
#define FORCED_STATUS_UPDATE_PERIOD 100  // number of loops, roughly every ten seconds

// #define DEBUG_BUILD

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

static struct MLX90393 mlx;

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

struct Vector3
{
  float x, y, z;
};

struct SensorClassification
{
  struct Vector3 location;
  float          radius2;
  enum DoorState door_state;
};

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

static float euclidean_distance2(struct Vector3 const * a, struct Vector3 const * b)
{
  float dx = a->x - b->x;
  float dy = a->y - b->y;
  float dz = a->z - b->z;

  return dx * dx + dy * dy + dz * dz;
}

//! Acceptable standard derivation of the sensor data.
//! If data has too much noise in it, we assume the magnetic field is
//! not stable and we can reject it.
//!
//! Adjust this to increase/decrease sensitivity to movement
//!
//! Measurements:
//! - In a static environment, the stddev is usually less than 2.
//! - In a changing environment, the stddev is typically way larger than 10.
static const float acceptable_stddev = 10.0;

static const struct SensorClassification well_known_vectors[] = {
    // generated data:
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = 6.171983, .y = -22.461975, .z = 6.576705}, .radius2 = 10.483105},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = 0.505900, .y = -22.259615, .z = 8.296766}, .radius2 = 20.121052},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -4.249563, .y = -23.069056, .z = 10.927447}, .radius2 = 29.852276},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -9.713285, .y = -24.991476, .z = 12.445148}, .radius2 = 55.054486},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -14.569928, .y = -27.015078, .z = 11.838068}, .radius2 = 98.320034},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -19.325390, .y = -29.342220, .z = 7.568555}, .radius2 = 187.681367},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -22.866692, .y = -37.841347, .z = 0.000000}, .radius2 = 409.496213},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -22.866692, .y = -37.841347, .z = -10.016829}, .radius2 = 555.778533},
    (struct SensorClassification){.door_state = DOOR_OPEN, .location = {.x = -22.866692, .y = -37.841347, .z = -20.539557}, .radius2 = 733.463518},
    (struct SensorClassification){.door_state = DOOR_CLOSED, .location = {.x = 20.851877, .y = -13.566074, .z = 10.409240}, .radius2 = 34.790504},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -17.264450, .y = -0.243910, .z = -37.188171}, .radius2 = 332.412282},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -27.281277, .y = 3.499753, .z = -44.958797}, .radius2 = 525.025754},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -35.578041, .y = 7.648136, .z = -53.215088}, .radius2 = 731.612207},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -43.975986, .y = 10.582358, .z = -61.570679}, .radius2 = 1002.024397},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -23.153131, .y = 2.164176, .z = -40.830650}, .radius2 = 424.629290},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -32.502167, .y = 6.049490, .z = -49.329777}, .radius2 = 638.108918},
    (struct SensorClassification){.door_state = DOOR_LOCKED, .location = {.x = -40.394211, .y = 9.206306, .z = -57.586067}, .radius2 = 875.646447},
};

#define NUM_SAMPLES 32

static struct Vector3 sliding_window[NUM_SAMPLES];
static size_t         sliding_window_index = 0;

static void push_sensor_data(struct Vector3 const * vec)
{
  assert(vec != NULL);

  sliding_window[sliding_window_index] = *vec;
  sliding_window_index += 1;
  if (sliding_window_index >= NUM_SAMPLES)
    sliding_window_index = 0;
}

struct SensorStats
{
  struct Vector3 avg;
  float          stddev;
};

static void compute_sensor_stats(struct SensorStats * stats)
{
  assert(stats != NULL);

  struct Vector3 avg_vec = {0, 0, 0};

  for (size_t i = 0; i < NUM_SAMPLES; i++) {
    avg_vec.x += sliding_window[i].x;
    avg_vec.y += sliding_window[i].y;
    avg_vec.z += sliding_window[i].z;
  }

  avg_vec.x /= (float)NUM_SAMPLES;
  avg_vec.y /= (float)NUM_SAMPLES;
  avg_vec.z /= (float)NUM_SAMPLES;

  float variance_sum = 0.0;
  for (size_t i = 0; i < NUM_SAMPLES; i++) {
    float dx = sliding_window[i].x - avg_vec.x;
    float dy = sliding_window[i].y - avg_vec.y;
    float dz = sliding_window[i].z - avg_vec.z;

    variance_sum += dx * dx + dy * dy + dz * dz;
  }

  stats->avg    = avg_vec;
  stats->stddev = sqrtf(variance_sum / (float)NUM_SAMPLES);
}

static enum DoorState classify_sensor_data(struct Vector3 const * vec)
{
  assert(vec != NULL);

  size_t len = sizeof(well_known_vectors) / sizeof(well_known_vectors[0]);

  float min_dist = INFINITY;

  enum DoorState classification = DOOR_FAULT; // this is the default assumption

  for (size_t i = 0; i < len; i++) {
    struct SensorClassification class = well_known_vectors[i];
    float dist2                       = euclidean_distance2(&class.location, vec);

    // printf("%zu => %.2f\n", i, dist);

    if (dist2 <= class.radius2 && dist2 < min_dist) {
      min_dist       = dist2;
      classification = class.door_state;
    }
  }

  return classification;
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

  struct Vector3 sensor_data;
  if (mlx90393_readData(&mlx, &sensor_data.x, &sensor_data.y, &sensor_data.z)) {

#ifdef DEBUG_BUILD
    enum DoorState raw_state = classify_sensor_data(&sensor_data);

    // Send sensor data data over MQTT
    char buffer[256];
    snprintf(buffer, sizeof buffer, "%4.2f\t%4.2f\t%4.2f\t%s", sensor_data.x, sensor_data.y, sensor_data.z, door_state_to_string(raw_state));
    mqtt_pub("debug/sensor/magnetometer/raw", buffer);

#endif

    // Insert the current sample into the sliding window
    push_sensor_data(&sensor_data);

    struct SensorStats stats;
    compute_sensor_stats(&stats);

    // Only accept the value if the change in the magnetic field is low
    bool noise_level_ok = (stats.stddev <= acceptable_stddev);

    // and classify the sliding average instead of the current reading,
    // so we have a stable sensor output.
    enum DoorState fresh_state = classify_sensor_data(&stats.avg);
    push_door_state(fresh_state);

#ifdef DEBUG_BUILD
    snprintf(buffer, sizeof buffer, "stddev: %4.2f\tclassification: %s\tok: %d\tsmoothed: %4.2f\t%4.2f\t%4.2f", stats.stddev, door_state_to_string(fresh_state), noise_level_ok, stats.avg.x, stats.avg.y, stats.avg.z);
    mqtt_pub("debug/sensor/magnetometer/classification", buffer);
#endif

    bool           door_state_stable = false;
    enum DoorState result_state      = get_smoothed_door_state(&door_state_stable);

    // Final classification of "ok" is chosen by this logic:
    // - The door state must be stable for at least a short amount of time
    // - The noise level must be low (so we're locked into position) or the door is in open state, which means we can swing the door back and forth without going to fault state
    *value_sane = door_state_stable && (noise_level_ok || (fresh_state == DOOR_OPEN));

#ifdef DEBUG_BUILD
    snprintf(buffer, sizeof buffer, "stable: %d\tnoise: %d\tok: %d\tclassification: %s", door_state_stable, noise_level_ok, *value_sane, door_state_to_string(result_state));
    mqtt_pub("debug/sensor/magnetometer/final", buffer);
#endif

    return result_state;
  }
  else {
    *value_sane = false;
  }

  return DOOR_FAULT;
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

static void initialize_door_sensors()
{
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
}

void app_main(void)
{
  event_group = xEventGroupCreate();

  initialize_door_sensors();

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
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR_SAFE);
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_OPEN_DOOR_UNSAFE);
  mqtt_subscribe(PORTAL300_TOPIC_ACTION_LOCK_DOOR);
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
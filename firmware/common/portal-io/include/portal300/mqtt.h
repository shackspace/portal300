#ifndef RIIF_MQTT
#define RIIF_MQTT

#include <stdbool.h>
#include <stddef.h>

struct MqttEvent {
  char const *topic;
  char const *data;

  size_t topic_len;
  size_t data_len;
};

typedef void (*MqttEventHandler)(struct MqttEvent const *event);
typedef void (*MqttSubscribeHandler)(void);

struct MqttConfig {
  char const *host_name;
  char const *client_id;

  const char *client_crt;
  const char *client_key;
  const char *ca_crt;

  MqttSubscribeHandler on_connect;
  MqttEventHandler on_data;
};

void mqtt_init(struct MqttConfig const *config);

bool mqtt_pub(char const *topic, char const *data);

bool is_mqtt_connected(void);

// only allowed to be called in `on_connect`.
bool mqtt_subscribe(char const *topic);

bool mqtt_event_has_topic(struct MqttEvent const *event, char const *topic);
bool mqtt_event_has_data(struct MqttEvent const *event, char const *data);

#endif

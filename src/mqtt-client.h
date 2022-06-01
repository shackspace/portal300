#ifndef PORTAL300_MQTT_CLIENT_H
#define PORTAL300_MQTT_CLIENT_H

#include <mqtt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct MqttClientSsl {
  struct ssl_st *ssl;
  struct ssl_ctx_st *ctx;
};

struct MqttClient {
  // stored configuration
  char *cfg_host_name;
  int cfg_port;

  // runtime status
  bool connected;
  int socket;
  struct MqttClientSsl ssl;
  struct mqtt_client client;
  uint8_t sendbuf[2048];
  uint8_t recvbuf[1024];
};

//! Initializes the MQTT library
bool mqtt_client_init();

//! Creates a new MQTT client that can be used to connect to the portal MQT
//! broker. Does not automatically connect!
struct MqttClient *mqtt_client_create(char const *host_name, int port);

//! Destroys a previously allocated MQTT client.
void mqtt_client_destroy(struct MqttClient *client);

bool mqtt_client_connect(struct MqttClient *client);

bool mqtt_client_sync(struct MqttClient *client);

bool mqtt_client_publish(struct MqttClient *client, char const *topic,
                         char const *message, int qos);

int mqtt_client_get_socket_fd(struct MqttClient *client);

#endif // PORTAL300_MQTT_CLIENT_H

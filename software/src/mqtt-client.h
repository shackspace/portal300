#ifndef PORTAL300_MQTT_CLIENT_H
#define PORTAL300_MQTT_CLIENT_H

#include <mqtt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct MqttClient
{
  // stored configuration
  char *              cfg_host_name;
  int                 cfg_port;
  struct ssl_ctx_st * ctx;
  char *              lw_topic;
  char *              lw_data;

  // runtime status
  bool               connected;
  int                socket;
  struct ssl_st *    ssl;
  struct mqtt_client client;
  uint8_t            sendbuf[2048];
  uint8_t            recvbuf[1024];
};

//! Initializes the MQTT library
bool mqtt_client_init(void);

//! Creates a new MQTT client that can be used to connect to the portal MQT
//! broker. Does not automatically connect!
//!
//! - `host_name` is the host name or ip address of the mqtt server.
//! - `port` is the port number of the mqtt server. usually `1883` or `8883`.
//! - `ca_cert` is the path to the CA certificate of the servers certificate.
//! - `client_key` is the path to the private key of the client certificate.
//! - `client_cert` is the path to the client certificate.
struct MqttClient * mqtt_client_create(
    char const * host_name,
    int          port,
    char const * ca_cert,
    char const * client_key,
    char const * client_cert,
    char const * last_will_topic,
    char const * last_will_message);

//! Destroys a previously allocated MQTT client.
void mqtt_client_destroy(struct MqttClient * client);

bool mqtt_client_connect(struct MqttClient * client);
void mqtt_client_disconnect(struct MqttClient * client);

bool mqtt_client_is_connected(struct MqttClient * client);

bool mqtt_client_sync(struct MqttClient * client);

bool mqtt_client_subscribe(struct MqttClient * client, char const * topic);

bool mqtt_client_publish(
    struct MqttClient * client,
    char const *        topic,
    char const *        message,
    int                 qos);

int mqtt_client_get_socket_fd(struct MqttClient * client);

#endif // PORTAL300_MQTT_CLIENT_H

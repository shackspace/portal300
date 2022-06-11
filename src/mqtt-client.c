#include "mqtt-client.h"

#include "portal_mqtt_pal.h"

#include <openssl/bio.h>
#include <openssl/ossl_typ.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>


bool mqtt_client_init()
{
  // TODO: Add proper error handling here
  OpenSSL_add_all_algorithms();
  ERR_load_BIO_strings();
  ERR_load_crypto_strings();
  SSL_load_error_strings();

  if(SSL_library_init() < 0) {
    fprintf(stderr, "failed to initialize SSL library\n");
    return false;
  }

  return true;
}

int mqtt_client_get_socket_fd(struct MqttClient *client) {
  if (client == NULL)
    return -1;
  if (!client->connected)
    return -1;
  return client->socket;
}

struct MqttClient *mqtt_client_create(
  char const * host_name, 
  int port,
  char const * ca_cert,
  char const * client_key,
  char const * client_cert
  )
{
  assert(host_name != NULL);
  assert(port > 0 && port <= 65535);

  struct MqttClient * client = malloc(sizeof(struct MqttClient));
  if(client == NULL) {
    return NULL;
  }

  memset(client, 0xAA, sizeof(struct MqttClient));

  *client = (struct MqttClient) {
    .cfg_host_name = strdup(host_name),
    .cfg_port = port,
    .ctx = NULL,

    .connected = false,
    .socket = -1,
    .ssl = NULL,
  };

  if(client->cfg_host_name == NULL) {
    goto _error_deinit_memory;
  }


  client->ctx = SSL_CTX_new(TLS_client_method());
  if(client->ctx == NULL) {
    goto _error_deinit_hostname;
  }

  if(SSL_CTX_load_verify_locations(client->ctx, ca_cert, NULL) != 1) {
    fprintf(stderr, "failed to load ca\n");
    goto _error_deinit_ctx;
  }
  if(SSL_CTX_use_certificate_file(client->ctx, client_cert, SSL_FILETYPE_PEM) != 1) {
    fprintf(stderr, "failed to load cert\n");
    goto _error_deinit_ctx;
  }
  if(SSL_CTX_use_PrivateKey_file(client->ctx, client_key, SSL_FILETYPE_PEM) != 1) {
    fprintf(stderr, "failed to load key\n");
    goto _error_deinit_ctx;
  }
  if (SSL_CTX_check_private_key(client->ctx) != 1) {
    fprintf(stderr, "key and cert do not match!\n");
    goto _error_deinit_ctx;
  }

  // enable proper non-blocking handling of our socket
  SSL_CTX_set_mode(client->ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
  
  // please verify the host certificate
  SSL_CTX_set_verify(client->ctx, SSL_VERIFY_PEER, NULL);

  // and please do only allow certificates directly signed by the CA
  SSL_CTX_set_verify_depth(client->ctx, 1);

  return client;

_error_deinit_ctx:
  SSL_CTX_free(client->ctx);

_error_deinit_hostname:
  free(client->cfg_host_name);

_error_deinit_memory:
  free(client);

  return NULL;
}

void mqtt_client_destroy(struct MqttClient *client)
{
  assert(client != NULL);
  if(client->connected) {
    mqtt_client_disconnect(client);
  }
  SSL_CTX_free(client->ctx);
  free(client->cfg_host_name);
  free(client);
}
 
static void publish_callback(void** unused, struct mqtt_response_publish *published) 
{
  (void)unused;
  (void)published;
  fprintf(stderr, "publish callback was triggered!\n");
  fprintf(stderr, "published data:\n"
                  "  topic: %.*s\n"
                  "  data:  %.*s\n",
                  (int)published->topic_name_size,
                  (char const *)published->topic_name,
                  (int)published->application_message_size,
                  (char const *)published->application_message);
}

static int connect_socket_to(char const * host_name, int port)
{
  assert(host_name != NULL);
  assert(port >= 0 && port <= 65535);
  
  struct addrinfo hints = {
  .ai_family = AF_UNSPEC, // IPv4 or IPv6
  .ai_socktype = SOCK_STREAM,
  .ai_flags = 0,
  .ai_protocol = 0, // allow any protocol
  };

  char port_name[64];
  snprintf(port_name, sizeof port_name, "%d", port);


  struct addrinfo *result;
  int error = getaddrinfo(host_name, port_name, &hints, &result);
  if (error != 0) {
    fprintf(stderr, "failed to query address info: %s\n", gai_strerror(error));
    return -1;
  }

  int sock = -1;
  for (struct addrinfo * iter = result; iter != NULL; iter = iter->ai_next) {
    int sockfd = socket(iter->ai_family, iter->ai_socktype, iter->ai_protocol);
    if (sockfd == -1) {
      continue;
    }

    if (connect(sockfd, iter->ai_addr, iter->ai_addrlen) != -1) {
      sock = sockfd;
      break;
    }

    if(close(sockfd) == -1) {
      perror("failed to destroy socket.");
    }
   }

   freeaddrinfo(result);

   return sock;
}

bool mqtt_client_connect(struct MqttClient *client)
{
  assert(client != NULL);

  SSL * const ssl = SSL_new(client->ctx);
  if(ssl == NULL) {
    return false;
  }

  int sockfd = connect_socket_to(client->cfg_host_name, client->cfg_port); 
  if(sockfd == -1) {
    perror("failed to create mqtt socket");
    goto _error_deinit_ssl;
  }

  SSL_set_fd(ssl, sockfd);

  int ssl_err;
  if((ssl_err = SSL_connect(ssl)) != 1 ) {
    fprintf(stderr, "failed to perform SSL handshake: %d\n", SSL_get_error(ssl, ssl_err));
    goto _error_deinit_socket;
  }

  if (SSL_get_verify_result(ssl) != X509_V_OK) {
    fprintf(stderr, "host verification of handshake failed\n");
    goto _error_deinit_socket;
  }


  int socket_flags = fcntl(sockfd, F_GETFL);
  if(socket_flags == -1) {
    perror("failed to get socket flags");
    goto _error_deinit_socket;
  }

  if(fcntl(sockfd, F_SETFL, socket_flags | O_NONBLOCK) == -1) {
    perror("failed to set socket flags");
    goto _error_deinit_socket;
  }


  // Now perform MQTT handshake

  enum MQTTErrors err;

  err = mqtt_init(&client->client, ssl, client->sendbuf, sizeof(client->sendbuf), client->recvbuf, sizeof(client->recvbuf), publish_callback);
  if(err != MQTT_OK) {
    fprintf(stderr, "failed to initialize mqtt client\n");
    goto _error_deinit_ssl;
  }

  err = mqtt_connect(&client->client, "portal client", NULL, NULL, 0, NULL, NULL, 0, 400);
  if (err != MQTT_OK) {
    fprintf(stderr, "failed to connect to mqtt server: %s\n", mqtt_error_str(err));
    goto _error_deinit_mqtt;
  }

  /* check that we don't have any errors */
  if (client->client.error != MQTT_OK) {
    fprintf(stderr, "failed to connect to mqtt server: %s\n", mqtt_error_str(client->client.error));
    goto _error_deinit_mqtt;
  }

  client->ssl = ssl;
  client->socket = sockfd;
  client->connected = true;

  return true;

  // ERROR HANDLING:
_error_deinit_mqtt:
  // nothing special here

_error_deinit_socket:
  if(close(sockfd) == -1) {
    perror("failed to close mqtt socket");
  }
_error_deinit_ssl:
  SSL_free(ssl);

  return false;
}

void mqtt_client_disconnect(struct MqttClient *client)
{
  assert(client != NULL);
  if(!client->connected) {
    return;
  }

  if(client->ssl != NULL) {
    SSL_free(client->ssl);
  }
  if(close(client->socket) == -1) {
    perror("failed to close MQTT socket handle");
  }

  client->ssl = NULL;
  client->socket = -1;
  client->connected = false;
}

bool mqtt_client_is_connected(struct MqttClient *client)
{
  assert(client != NULL);
  return client->connected;
}

bool mqtt_client_sync(struct MqttClient *client)
{
  assert(client != NULL);
  if(client->connected == false) {
    return false;
  }

  enum MQTTErrors err = mqtt_sync(&client->client);

    switch(err) {
      case MQTT_OK: return true;

      case MQTT_ERROR_CONNECTION_CLOSED:
        mqtt_client_disconnect(client);
        return false;

      default:
        fprintf(stderr, "mqtt_sync() failed: %s\n", mqtt_error_str(err));
        break;
  }

  return false;
}

bool mqtt_client_subscribe(struct MqttClient *client, char const * topic)
{
  assert(client != NULL);
  assert(topic != NULL);
  assert(client->connected);

  enum MQTTErrors err = mqtt_subscribe(&client->client, topic, 2);
  
  if(err != MQTT_OK) {
    fprintf(stderr, "failed to subscribe to mqtt topic: %s\n", mqtt_error_str(err));
    return false;
  }

    /* check for errors */
  if (client->client.error != MQTT_OK) {
    fprintf(stderr, "failed to subscribe to mqtt topic: %s\n", mqtt_error_str(client->client.error));
    return false;
  }

  return true;
}

bool mqtt_client_publish(struct MqttClient *client, char const * topic, char const * message, int qos)
{
  assert(client != NULL);
  assert(topic != NULL);
  assert(message != NULL);
  assert(qos >= 0 && qos <= 2);
  assert(client->connected);

  static const uint8_t flags[3] = {
    MQTT_PUBLISH_QOS_0,
    MQTT_PUBLISH_QOS_1,
    MQTT_PUBLISH_QOS_2,
  };

  enum MQTTErrors err = mqtt_publish(
    &client->client, 
    topic, 
    message, strlen(message), 
    flags[qos]
  );
  if(err != MQTT_OK) {
    fprintf(stderr, "failed to publish mqtt message: %s\n", mqtt_error_str(err));
    return false;
  }

    /* check for errors */
  if (client->client.error != MQTT_OK) {
    fprintf(stderr, "failed to publish mqtt message: %s\n", mqtt_error_str(client->client.error));
    return false;
  }

  return true;
}



/**
 * @brief Sends all the bytes in a buffer.
 * @ingroup pal
 * 
 * @param[in] fd The file-descriptor (or handle) of the socket.
 * @param[in] buf A pointer to the first byte in the buffer to send.
 * @param[in] len The number of bytes to send (starting at \p buf).
 * @param[in] flags Flags which are passed to the underlying socket.
 * 
 * @returns The number of bytes sent if successful, an \ref MQTTErrors otherwise.
 *
 * Note about the error handling:
 * - On an error, if some bytes have been processed already,
 *   this function should return the number of bytes successfully
 *   processed. (partial success)
 * - Otherwise, if the error is an equivalent of EAGAIN, return 0.
 * - Otherwise, return MQTT_ERROR_SOCKET_ERROR.
 */
ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void *buf, size_t buffer_size,
                         int flags)
{
  (void)flags;
  
  char const * const buffer = (const char*)buf;

  size_t offset = 0;
  while(offset < buffer_size) {
    int rv = SSL_write(fd, buffer + offset, buffer_size - offset);
    if(rv <= 0) {
      int error_code = SSL_get_error(fd, rv);
      switch (error_code) {
        case SSL_ERROR_WANT_READ:
          return offset;
        case SSL_ERROR_ZERO_RETURN: // end of stream
          return MQTT_ERROR_CONNECTION_CLOSED;
        default: 
          fprintf(stderr, "SSL_write() failed: %d\n", error_code);
          return MQTT_ERROR_SOCKET_ERROR;
      }
    }
    offset += (size_t)rv;
  }
  return offset;
}

/**
 * @brief Non-blocking receive all the byte available.
 * @ingroup pal
 * 
 * @param[in] fd The file-descriptor (or handle) of the socket.
 * @param[in] buf A pointer to the receive buffer.
 * @param[in] bufsz The max number of bytes that can be put into \p buf.
 * @param[in] flags Flags which are passed to the underlying socket.
 * 
 * @returns The number of bytes received if successful, an \ref MQTTErrors otherwise.
 *
 * Note about the error handling:
 * - On an error, if some bytes have been processed already,
 *   this function should return the number of bytes successfully
 *   processed. (partial success)
 * - Otherwise, if the error is an equivalent of EAGAIN, return 0.
 * - Otherwise, return MQTT_ERROR_SOCKET_ERROR.
 */
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void *buffer_erased, size_t buffer_size,
                         int flags)
{
  (void)flags;
  
  char * const buffer = buffer_erased;

  size_t offset = 0;
  while(offset < buffer_size) {
    int rv = SSL_read(fd, buffer + offset, buffer_size - offset);
    if(rv <= 0) {
      int error_code = SSL_get_error(fd, rv);
      switch (error_code) {
        case SSL_ERROR_WANT_READ:
          return offset;
        case SSL_ERROR_ZERO_RETURN: // end of stream
          return MQTT_ERROR_CONNECTION_CLOSED;
        default: 
          fprintf(stderr, "SSL_read() failed: %d\n", error_code);
          return MQTT_ERROR_SOCKET_ERROR;
      }
    }
    offset += (size_t)rv;
  }
  return offset;
}

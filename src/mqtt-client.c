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

struct MqttClient *mqtt_client_create(char const * host_name, int port)
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
    .connected = false,
  };

  if(client->cfg_host_name == NULL) {
    free(client);
    return NULL;
  }

  return client;
}

void mqtt_client_destroy(struct MqttClient *client)
{
  assert(client != NULL);
  if(client->connected) {
    if(client->ssl.ssl != NULL) {
      SSL_free(client->ssl.ssl);
    }
    if(client->ssl.ctx != NULL) {
      SSL_CTX_free(client->ssl.ctx);
    }
    if(close(client->socket) == -1) {
      perror("failed to close MQTT socket handle");
    }
  }
  free(client->cfg_host_name);
  free(client);
}


static void publish_callback(void** unused, struct mqtt_response_publish *published) 
{
  (void)unused;
  (void)published;
  fprintf(stderr, "publish callback was triggered!\n");
}



static void* client_refresher(void* client)
{
  while(1) 
  {
    // fprintf(stderr, "sync\n");
    mqtt_sync((struct mqtt_client*) client);
    usleep(100000U);
  }
  return NULL;
}

bool mqtt_client_connect(struct MqttClient *client)
{
  (void)client;

  SSL_CTX * const ctx = SSL_CTX_new(TLS_client_method());
  if(ctx == NULL) {
    return false;
  }

//#define PREFIX "/home/felix/projects/shackspace/portal300/code/"
#define PREFIX ""

  if(SSL_CTX_load_verify_locations(ctx, PREFIX "debug/ca.crt", NULL) != 1) {
    fprintf(stderr, "failed to load ca\n");
    goto _error_deinit_ctx;
  }
  if(SSL_CTX_use_certificate_file(ctx, PREFIX "debug/client.crt", SSL_FILETYPE_PEM) != 1) {
    fprintf(stderr, "failed to load cert\n");
    goto _error_deinit_ctx;
  }
  if(SSL_CTX_use_PrivateKey_file(ctx, PREFIX "debug/client.key", SSL_FILETYPE_PEM) != 1) {
    fprintf(stderr, "failed to load key\n");
    goto _error_deinit_ctx;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    fprintf(stderr, "key and cert do not match!\n");
    goto _error_deinit_ctx;
  }

  // automatically continue incomplete reads/writes during handshake
  SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
  
  // please verify the host certificate
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

  // and please do only allow certificates directly signed by the CA
  SSL_CTX_set_verify_depth(ctx, 1);

  // TODO: Load CA 

  SSL * const ssl = SSL_new(ctx);
  if(ssl == NULL) {
    goto _error_deinit_ctx;
  }

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if(sockfd == -1) {
    perror("failed to create mqtt socket");
    goto _error_deinit_ssl;
  }

  struct sockaddr_in dest_addr = {
    .sin_family = AF_INET,
    .sin_port = htons(8883),
    .sin_addr.s_addr = inet_addr("127.0.0.1"),
  };

  /* ---------------------------------------------------------- *
   * Try to make the host connect here                          *
   * ---------------------------------------------------------- */
  if (connect(sockfd, (struct sockaddr *) &dest_addr, sizeof(struct sockaddr)) == -1 ) {
    goto _error_deinit_socket;
  }

  SSL_set_fd(ssl, sockfd);
  
  if(SSL_connect(ssl) != 1 ) {
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

  fprintf(stderr, "initialized\n");

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

  fprintf(stderr, "connected\n");

  // TODO: solve this more elegantly via a exposed socket FD for poll

  /* start a thread to refresh the client (handle egress and ingree client traffic) */
  pthread_t client_daemon;
  if(pthread_create(&client_daemon, NULL, client_refresher, &client->client)) {
      // fprintf(stderr, "Failed to start client daemon.\n");
      // exit_example(EXIT_FAILURE, sockfd, NULL);
  }

  client->ssl = (struct MqttClientSsl) {
    .ssl = ssl,
    .ctx = ctx,
  };
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

_error_deinit_ctx:
  SSL_CTX_free(ctx);

  return false;
}

bool mqtt_client_sync(struct MqttClient *client)
{
  assert(client != NULL);

  mqtt_sync(&client->client);

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
        default: 
          fprintf(stderr, "SSL_read() failed: %d\n", error_code);
          return MQTT_ERROR_SOCKET_ERROR;
      }
    }
    offset += (size_t)rv;
  }
  return offset;
}
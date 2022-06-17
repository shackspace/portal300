#ifndef PORTAL300_MQTT_PAL_H
#define PORTAL300_MQTT_PAL_H

#include <stddef.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define MQTT_PAL_HTONS(s) htons(s)
#define MQTT_PAL_NTOHS(s) ntohs(s)

#define MQTT_PAL_TIME() time(NULL)

typedef time_t          mqtt_pal_time_t;
typedef pthread_mutex_t mqtt_pal_mutex_t;

#define MQTT_PAL_MUTEX_INIT(mtx_ptr)   pthread_mutex_init(mtx_ptr, NULL)
#define MQTT_PAL_MUTEX_LOCK(mtx_ptr)   pthread_mutex_lock(mtx_ptr)
#define MQTT_PAL_MUTEX_UNLOCK(mtx_ptr) pthread_mutex_unlock(mtx_ptr)

typedef struct ssl_st * mqtt_pal_socket_handle;

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle fd, const void * buf, size_t len, int flags);
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle fd, void * buf, size_t bufsz, int flags);

#endif // PORTAL300_MQTT_PAL_H
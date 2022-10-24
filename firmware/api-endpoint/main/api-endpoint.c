#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "hal/gpio_types.h"
#include "nvs_flash.h"
#include "esp_sntp.h"

#include <time.h>
#include <sys/time.h>

#include "esp_http_client.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "config.h"
#include <stdint.h>
#include <stdio.h>

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char * const SYSTEM_TAG = "system";
static const char * const WIFI_TAG   = "wifi station";
static const char * const HTTP_TAG   = "http client";

static void wifi_init_sta(void);
static void http_native_request(void);

static void time_synchronized(struct timeval * tv);

extern const uint8_t root_cert_pem_start[] asm("_binary_letsencrypt_root_pem_start");
extern const uint8_t root_cert_pem_end[] asm("_binary_letsencrypt_root_pem_end");

static volatile bool last_time_sync = false;

// void sntp_sync_time(struct timeval * tv)
// {
//   if (settimeofday(tv, NULL) == -1)
//     perror("lol wtf");
//   ESP_LOGI(SYSTEM_TAG, "Time is synchronized from custom code");
//   sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
//   last_time_sync = true;
// }

void app_main(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_netif_create_default_wifi_sta();

  ESP_ERROR_CHECK(esp_tls_set_global_ca_store(root_cert_pem_start, root_cert_pem_end - root_cert_pem_start));

  ESP_LOGI(SYSTEM_TAG, "connecting to wifi...");
  wifi_init_sta();

  ESP_LOGI(SYSTEM_TAG, "Connected to AP, begin providing shack information");

  {
    // printf("TZ=%s\n", getenv("TZ") || "null");
    setenv("TZ", "CET-1MET", 1);
    tzset();
  }

  {
    sntp_set_time_sync_notification_cb(time_synchronized);
    sntp_servermode_dhcp(1);
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    struct timeval tv;
    sntp_sync_time(&tv);
  }

  {
    time_t    now;
    char      strftime_buf[64];
    struct tm timeinfo;

    time(&now);

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(SYSTEM_TAG, "Time after boot is: %s", strftime_buf);
  }

  bool is_open = false;

  uint32_t next_update_msg = 0;

  TickType_t next_wakeup = xTaskGetTickCount();
  while (true) {

    // tick with 50ms period time and check stdin. process messages from there.
    xTaskDelayUntil(&next_wakeup, pdMS_TO_TICKS(50));

    if (last_time_sync) {
      last_time_sync = false;

      time_t    now;
      char      strftime_buf[128];
      struct tm timeinfo;

      time(&now);
      localtime_r(&now, &timeinfo);

      strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
      ESP_LOGI("SNTP", "Time was synchronized to %s", strftime_buf);
    }

    uint8_t byte;
    while (fread(&byte, 1, 1, stdin) == 1) {
      switch (byte) {
      case PORTAL_SIGNAL_OPEN: is_open = true; break;
      case PORTAL_SIGNAL_CLOSED: is_open = false; break;
      default:
        printf("received unhandled %02X\n", byte);
        break;
      }
    }

    if (xTaskGetTickCount() >= next_update_msg) {

      next_update_msg += pdMS_TO_TICKS(PORTAL_API_UPDATE_PERIOD * 1000);
      if (is_open) {
        ESP_LOGI(SYSTEM_TAG, "Periodic updater tick: Sending 'open' ping");
        http_native_request();
      }
      else {
        ESP_LOGI(SYSTEM_TAG, "Periodic updater tick: Shack not open, not sending ping.");
      }
    }
  }
}

static void time_synchronized(struct timeval * tv)
{
  (void)tv;
  // printf("secs=%ld\n", tv->tv_sec);

  settimeofday(tv, NULL);

  last_time_sync = true;
}

static void event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
  static int s_retry_num = 0;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < SHACK_MAXIMUM_RECONNECT_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
    }
    else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(WIFI_TAG, "connect to the AP fail");
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t * event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static wifi_config_t shack_wifi_config = {
    .sta = {
        .ssid               = SHACK_WLAN_SSID,
        .password           = SHACK_WLAN_PASSWORD,
        .threshold.authmode = WIFI_AUTH_WPA2_PSK, // at least wpa2psk plz
    },
};

static void wifi_init_sta(void)
{

  s_wifi_event_group = xEventGroupCreate();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_got_ip));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &shack_wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE,
                                         pdFALSE,
                                         portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s", shack_wifi_config.sta.ssid, shack_wifi_config.sta.password);
  }
  else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s, password:%s", shack_wifi_config.sta.ssid, shack_wifi_config.sta.password);
    abort(); // just reboot with a panic
  }
  else {
    ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    abort(); // just reboot with a panic
  }
}

static void http_native_request(void)
{
  esp_http_client_config_t config = {
      .url                 = PORTAL_API_ENDPOINT,
      .use_global_ca_store = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);

  // GET Request
  ESP_ERROR_CHECK(esp_http_client_set_method(client, HTTP_METHOD_GET));
  ESP_ERROR_CHECK(esp_http_client_open(client, 0));

  int content_length = esp_http_client_fetch_headers(client);
  if (content_length < 0) {
    ESP_LOGE(HTTP_TAG, "HTTP client fetch headers failed");
    abort(); // just reboot
  }
  else {
    content_length  = esp_http_client_get_content_length(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200 || content_length != 2) { // assume "ok"
      ESP_LOGE(HTTP_TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
    }

    // we ignore all data here, we're happy with just pushing the message successfully to the server
  }

  ESP_ERROR_CHECK(esp_http_client_close(client));

  ESP_ERROR_CHECK(esp_http_client_cleanup(client));
}

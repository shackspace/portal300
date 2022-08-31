#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
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

#include "esp_http_client.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "config.h"

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

extern const uint8_t root_cert_pem_start[] asm("_binary_letsencrypt_root_pem_start");
extern const uint8_t root_cert_pem_end[] asm("_binary_letsencrypt_root_pem_end");

void app_main(void)
{
  static const gpio_config_t io_conf = {
      .intr_type    = GPIO_INTR_DISABLE,
      .mode         = GPIO_MODE_INPUT,
      .pin_bit_mask = (1 << PORTAL_OPEN_STATE_PIN),
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .pull_up_en   = GPIO_PULLUP_ENABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));

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

  bool is_open = false;

  TickType_t next_wakeup = xTaskGetTickCount();
  while (true) {
    xTaskDelayUntil(&next_wakeup, pdMS_TO_TICKS(PORTAL_API_UPDATE_PERIOD * 1000));

    {
      bool const new_open = (gpio_get_level(PORTAL_OPEN_STATE_PIN) == 0);
      if (new_open != is_open) {
        ESP_LOGI(SYSTEM_TAG, "shack state changed. shack is now %s", new_open ? "open" : "closed");
      }
      is_open = new_open;
    }

    if (is_open) {
      http_native_request();
    }
  }
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
  }
  else {
    ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
  }
}

static esp_err_t _http_event_handler(esp_http_client_event_t * evt)
{
  static char * output_buffer; // Buffer to store response of http request from event handler
  static int    output_len;    // Stores number of bytes read
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
    if (!esp_http_client_is_chunked_response(evt->client)) {
      // If user_data buffer is configured, copy the response into the buffer
      if (evt->user_data) {
        memcpy(evt->user_data + output_len, evt->data, evt->data_len);
      }
      else {
        if (output_buffer == NULL) {
          output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
          output_len    = 0;
          if (output_buffer == NULL) {
            ESP_LOGE(HTTP_TAG, "Failed to allocate memory for output buffer");
            return ESP_FAIL;
          }
        }
        memcpy(output_buffer + output_len, evt->data, evt->data_len);
      }
      output_len += evt->data_len;
    }

    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_FINISH");
    if (output_buffer != NULL) {
      // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
      // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(HTTP_TAG, "HTTP_EVENT_DISCONNECTED");
    int       mbedtls_err = 0;
    esp_err_t err         = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
    if (err != 0) {
      ESP_LOGI(HTTP_TAG, "Last esp error code: 0x%x", err);
      ESP_LOGI(HTTP_TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
    }
    if (output_buffer != NULL) {
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;
    // case HTTP_EVENT_REDIRECT:
    //   ESP_LOGD(HTTP_TAG, "HTTP_EVENT_REDIRECT");
    //   esp_http_client_set_header(evt->client, "From", "user@example.com");
    //   esp_http_client_set_header(evt->client, "Accept", "text/html");
    //   break;
  }
  return ESP_OK;
}

static void http_native_request(void)
{
  int                      content_length = 0;
  esp_http_client_config_t config         = {
      .url                 = PORTAL_API_ENDPOINT,
      .use_global_ca_store = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);

  // GET Request
  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(HTTP_TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
  }
  else {
    content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
      ESP_LOGE(HTTP_TAG, "HTTP client fetch headers failed");
    }
    else {
      ESP_LOGI(HTTP_TAG, "HTTP GET Status = %d, content_length = %d", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));

      // int data_read = esp_http_client_read_response(client, output_buffer, MAX_HTTP_OUTPUT_BUFFER);
      // if (data_read >= 0) {
      //
      //   // ESP_LOG_BUFFER_HEX(TAG, output_buffer, data_read);
      // }
      // else {
      //   ESP_LOGE(TAG, "Failed to read response");
      // }
    }
  }
  esp_http_client_close(client);

  esp_http_client_cleanup(client);
}

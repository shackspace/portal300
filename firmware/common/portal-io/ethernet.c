#include "include/portal300/ethernet.h"

// https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_netif.html
// https://github.com/ldijkman/WT32-ETH01-LAN-8720-RJ45-

#define CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET
#define CONFIG_EXAMPLE_ETH_PHY_LAN87XX

#define CONFIG_EXAMPLE_ETH_MDC_GPIO   23
#define CONFIG_EXAMPLE_ETH_MDIO_GPIO  18
#define CONFIG_EXAMPLE_ETH_POWER_GPIO 16

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#if CONFIG_ETH_USE_SPI_ETHERNET
#include "driver/spi_master.h"
#endif // CONFIG_ETH_USE_SPI_ETHERNET
#include "esp_eth_mac.h"

// #include <ethernet/include/eth_phy/phy_lan8720.h>

static const char * TAG = "eth_example";

/** Event handler for Ethernet events */
static void eth_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
  uint8_t mac_addr[6] = {0};
  /* we can get the ethernet driver handle from event data */
  esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

  switch (event_id) {
  case ETHERNET_EVENT_CONNECTED:
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
    ESP_LOGI(TAG, "Ethernet Link Up");
    ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    break;
  case ETHERNET_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Ethernet Link Down");
    break;
  case ETHERNET_EVENT_START:
    ESP_LOGI(TAG, "Ethernet Started");
    break;
  case ETHERNET_EVENT_STOP:
    ESP_LOGI(TAG, "Ethernet Stopped");
    break;
  default:
    break;
  }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
  ip_event_got_ip_t *         event   = (ip_event_got_ip_t *)event_data;
  const esp_netif_ip_info_t * ip_info = &event->ip_info;

  ESP_LOGI(TAG, "Ethernet Got IP Address");
  ESP_LOGI(TAG, "~~~~~~~~~~~");
  ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
  ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
  ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
  ESP_LOGI(TAG, "~~~~~~~~~~~");
}

void ethernet_init(char const * host_name)
{
  // Initialize TCP/IP network interface (should be called only once in application)
  ESP_ERROR_CHECK(esp_netif_init());
  // Create default event loop that running in background
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Create new default instance of esp-netif for Ethernet
  esp_netif_config_t cfg       = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *      eth_netif = esp_netif_new(&cfg);

  if (host_name != NULL) {
    ESP_ERROR_CHECK(esp_netif_set_hostname(eth_netif, host_name));
  }

  // Init MAC and PHY configs to default

  gpio_config_t gpio_cfg = {
      .pin_bit_mask = 1 << CONFIG_EXAMPLE_ETH_POWER_GPIO,
      .mode         = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
  ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_ETH_POWER_GPIO, 1));

  eth_mac_config_t esp32_emac_config             = ETH_MAC_DEFAULT_CONFIG();
  esp32_emac_config.smi_mdc_gpio_num             = CONFIG_EXAMPLE_ETH_MDC_GPIO;
  esp32_emac_config.smi_mdio_gpio_num            = CONFIG_EXAMPLE_ETH_MDIO_GPIO;
  esp32_emac_config.sw_reset_timeout_ms          = 1000; //
  esp32_emac_config.clock_config.rmii.clock_gpio = 0;
  esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;

  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.phy_addr         = 1;  // always the first PHY
  phy_config.reset_gpio_num   = -1; // no physical reset

  esp_eth_mac_t * mac = esp_eth_mac_new_esp32(&esp32_emac_config);
  esp_eth_phy_t * phy = esp_eth_phy_new_lan87xx(&phy_config);

  esp_eth_config_t config     = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth_handle = NULL;
  ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
  /* attach Ethernet driver to TCP/IP stack */
  ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

  // Register user defined event handers
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

  /* start Ethernet driver state machine */
  ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}
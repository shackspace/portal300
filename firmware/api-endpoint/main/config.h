#ifndef PORTAL_API_ENDPOINT_CONFIG_H
#define PORTAL_API_ENDPOINT_CONFIG_H

#include "api-token.h" // NOT IN GIT! Contains only a string literal with the API token.

// WLAN Configuration:
#define SHACK_WLAN_SSID               "shack"
#define SHACK_WLAN_PASSWORD           "welcome2shack"
#define SHACK_MAXIMUM_RECONNECT_RETRY 5

// Hardware Configuration:
#define PORTAL_OPEN_STATE_PIN 0 // we use the RTS signal to not only pass "boot" request, but also pass the information of being open

// API Configuration:
#define PORTAL_API_UPDATE_PERIOD (2 * 60) // seconds
#define PORTAL_API_ENDPOINT      "https://api2.shackspace.de/v1/space/notify-open?auth_token=" PORTAL_API_TOKEN

#endif
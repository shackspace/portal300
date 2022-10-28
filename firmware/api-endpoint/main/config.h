#ifndef PORTAL_API_ENDPOINT_CONFIG_H
#define PORTAL_API_ENDPOINT_CONFIG_H

#include "api-token.h" // NOT IN GIT! Contains only a string literal with the API token.

// WLAN Configuration:
#define SHACK_WLAN_SSID               "shack"
#define SHACK_WLAN_PASSWORD           "welcome2shack"
#define SHACK_MAXIMUM_RECONNECT_RETRY 5

// API Configuration:
#define PORTAL_API_UPDATE_PERIOD (1 * 60) // seconds
#define PORTAL_API_ENDPOINT      "https://api.shackspace.de/v1/space/notify-open?auth_token=" PORTAL_API_TOKEN

// byte message definitions:
#define PORTAL_SIGNAL_OPEN   0x12 // DC2, C-R
#define PORTAL_SIGNAL_CLOSED 0x14 // DC4, C-T

#endif
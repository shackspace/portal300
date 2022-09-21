#!/bin/bash

set -e
root="$(realpath "$(dirname "$0")")"

cat > "$root/main/door_config.h" <<EOF
#define CURRENT_DOOR                DOOR_C2
#define CURRENT_DEVICE              DOOR_CONTROL_C2
#define BUTTON_DEBOUNCE_TIME        1000 // ms, can be pretty high for less button mashing
#define FORCED_STATUS_UPDATE_PERIOD 100  // number of loops, roughly every ten seconds
// #define DEBUG_BUILD

#define PIN_OUT_OPEN   GPIO_NUM_2
#define PIN_OUT_CLOSE  GPIO_NUM_4
#define PIN_OUT_SIGNAL GPIO_NUM_12

#define PIN_CFG_OPENDRAIN 1

#define PIN_IN_BUTTON GPIO_NUM_35

#define PIN_IN_DOOR_CLOSED GPIO_NUM_14
#define PIN_IN_DOOR_LOCKED GPIO_NUM_15
EOF
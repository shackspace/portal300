#!/bin/bash

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

exec mosquitto_pub \
  -d \
  -L "mqtts://mqtt.portal.shackspace.de/shackspace/portal/action/door/open" \
  -m "door.B" \
  --cafile "${SCRIPT_ROOT}/ca.crt" \
  --key    "${SCRIPT_ROOT}/client.key" \
  --cert   "${SCRIPT_ROOT}/client.crt" 
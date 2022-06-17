#!/bin/bash

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

exec mosquitto_sub \
  -d \
  -v \
  -L "mqtts://mqtt.portal.shackspace.de/#" \
  --cafile "${SCRIPT_ROOT}/ca.crt" \
  --key    "${SCRIPT_ROOT}/client.key" \
  --cert   "${SCRIPT_ROOT}/client.crt" 
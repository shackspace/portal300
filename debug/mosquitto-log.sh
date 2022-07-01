#!/bin/bash

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

TOPIC="#"

if [ "$1" != "" ]; then
  TOPIC="$1"
fi

exec mosquitto_sub \
  -v \
  -L "mqtts://mqtt.portal.shackspace.de/${TOPIC}" \
  --cafile "${SCRIPT_ROOT}/ca.crt" \
  --key    "${SCRIPT_ROOT}/client.key" \
  --cert   "${SCRIPT_ROOT}/client.crt" 
  
#!/bin/bash

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

TOPIC="$1"
DATA="$2"

if [ -z "${TOPIC}" ]; then
  echo "mosquitto-test.sh <topic> <data>"
  exit 1
fi

if [ -z "${DATA}" ]; then
  echo "mosquitto-test.sh <topic> <data>"
  exit 1
fi

exec mosquitto_pub \
  -L "mqtts://mqtt.portal.shackspace.de/${TOPIC}" \
  -m "${DATA}" \
  --cafile "${SCRIPT_ROOT}/ca.crt" \
  --key    "${SCRIPT_ROOT}/client.key" \
  --cert   "${SCRIPT_ROOT}/client.crt" 
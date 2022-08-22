#!/bin/bash

PATH=/bin:/usr/bin:/usr/local/bin

function run() {
  /opt/portal300/import-keys.sh \
    "${DEVNAME}" \
    /etc/mosquitto/ca_certificates/shack-portal.crt \
  2>&1 | logger --tag keyimport
}
run "$@" &

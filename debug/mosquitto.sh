#!/bin/bash

set -e

SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

cd "${SCRIPT_ROOT}"

mosquitto -c "${SCRIPT_ROOT}/mosquitto.conf" -v
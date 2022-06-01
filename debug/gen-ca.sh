#!/bin/bash

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

if [ -z "$1" ]; then
  echo "gen-selfsigned-cert.sh <basename>"
  echo " generates <basename>.key and <basename>.crt"
  exit 1
fi

# Generate key pair for CA
openssl genrsa -out "$1.key" 2048

openssl req -new -x509 -config "${SCRIPT_ROOT}/ca-info.cfg" -days 356 -key "$1.key" -out "$1.crt"
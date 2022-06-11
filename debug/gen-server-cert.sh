#!/bin/bash

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

if [ -z "$1" ] || [ -z "$2" ]; then
  echo "gen-selfsigned-cert.sh <ca> <basename>"
  echo " generates <basename>.key and <basename>.crt and signs them with the <ca>"
  exit 1
fi

# generate server key
openssl genrsa -out "$2.key" 2048

openssl req -new -out "$2.csr" -key "$2.key" -config "${SCRIPT_ROOT}/server-info.cfg"

openssl x509 -req -in "$2.csr" -CA "$1.crt" -CAkey "$1.key" -CAcreateserial -out "$2.crt" -days 365

rm "$2.csr"

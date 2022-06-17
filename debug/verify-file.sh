#!/bin/bash

set -e

openssl x509 \
  -pubkey \
  -noout \
  -in /etc/mosquitto/ca_certificates/shack-portal.crt \
> /tmp/shack-portal.pub

openssl dgst \
  -sha256 \
  -verify /tmp/shack-portal.pub \
  -signature /mnt/keymembers.json.sig \
  /mnt/keymembers.json

echo "Verified OK"
#!/bin/bash

ROOT="$(dirname $(realpath $0))"

set -e

DEVICE="$1"
CA_CERT="$2"
ACTIONS="close open-front open-back status"

if [ -z "${DEVICE}" ]; then
  echo "Usage: import-keys.sh <device> <ca-cert>"
  exit 1
fi

if [ ! -f "${CA_CERT}" ]; then
  echo "Usage: import-keys.sh <device> <ca-cert>"
  exit 1
fi

# extract key from ca certificate
openssl x509 \
  -pubkey \
  -noout \
  -in "${CA_CERT}" \
> /tmp/shack-portal.pub

echo "Pulling files from usb stick"

mount "${DEVICE}" /mnt

if [ ! -f "/mnt/keymembers.json" ]; then
  echo "Missing keymembers.json!"
  umount /mnt
fi

if [ ! -f "/mnt/keymembers.json.sig" ]; then
  echo "Missing keymembers.json.sig!"
  umount /mnt
fi

cp /mnt/keymembers.json /tmp/keymembers.json
cp /mnt/keymembers.json.sig /tmp/keymembers.json.sig

umount /mnt

echo "Retrieved files, validating..."

if openssl dgst \
  -sha256 \
  -verify /tmp/shack-portal.pub \
  -signature /tmp/keymembers.json.sig \
  /tmp/keymembers.json; then
  echo "Files are correct, we can now generate authorized_keys files..."
else
  echo "Signature verification failed"
  exit 1
fi

rm /tmp/shack-portal.pub /tmp/keymembers.json.sig

for action in ${ACTIONS}; do
  echo "rendering file for ${action}..."
  id "${action}" > /dev/zero # verify we have a user for this action

  jq --raw-output -f "${ROOT}/generate_authorized_keys.jq" --arg action "${action}" \
     < /tmp/keymembers.json \
     > "/home/${action}/.ssh/authorized_keys.new"

  chown "${action}:${action}" "/home/${action}/.ssh/authorized_keys.new"
done

for action in ${ACTIONS}; do
  echo "deploying file for ${action}..."
  mv "/home/${action}/.ssh/authorized_keys"{.new,}
done

rm /tmp/keymembers.json

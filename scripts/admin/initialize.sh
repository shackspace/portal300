#!/bin/bash
# Initializes a fresh admin setup.

ROOT="$(dirname $(realpath $0))"
set -e
source "${ROOT}/data/common-inc.sh"

mkdir -p "${ROOT}/ca"

function print_security_panic()
{
  echo "$(basename "$1") already exists. Not generating a new CA."
  echo ""
  echo "WARNING: DO NOT DELETE THIS FILE IF YOU DO NOT UNDERSTAND THE IMPLICATIONS."
  echo "         PORTAL SYSTEM WILL NOT BE OPERABLE ANYMORE AFTER THAT AND MUST BE"
  echo "         REFLASHED!"
  exit 1
}


if [ -e "${KEY_FILE}" ] && [ -e "${CRT_FILE}" ]; then
  echo ""
  echo "CA is already fully initialized!"
  echo ""
  echo "Your CA files are:"
  echo "Key:         ${KEY_FILE}"
  echo "Certificate: ${CRT_FILE}"
  echo ""
  echo "Run ./key-export.sh <folder> to generate a new key deployment."
  exit 0
fi

if [ -e "${KEY_FILE}" ]; then
  print_security_panic "${KEY_FILE}"
  exit 1
fi

if [ -e "${CRT_FILE}" ]; then
  print_security_panic "${CRT_FILE}"
  exit 1
fi

openssl genrsa -out "${KEY_FILE}" 2048

openssl req -new -x509 -config "${ROOT}/data/ca-descriptor.cfg" -days 365 -key "${KEY_FILE}" -out "${CRT_FILE}"

echo "Done. You can now run ./key-export.sh <folder>"

#!/bin/bash
# key-export.sh <target-folder>

ROOT="$(dirname $(realpath $0))"
set -e
source "${ROOT}/data/common-inc.sh"

# sanitize files
if [ ! -f "${ROOT}/data/key-export.sql" ]; then 
  echo "data/key-export.sql missing!"
  exit 1
fi

if [ ! -f "${KEY_FILE}" ]; then
  echo "${KEY_FILE} not found. Is the CA initialized?"
  exit 1
fi

if [ ! -f "${CRT_FILE}" ]; then
  echo "${CRT_FILE} not found. Is the CA initialized?"
  exit 1
fi

# sanitize arguments
TARGET="$1"
if [ -z "${TARGET}" ]; then
  echo "usage: $0 <target-folder>"
  exit 1
fi

LISTFILE="${TARGET}/keymembers.json"

echo "Fetching keys from byro..."
psql \
  -h localhost \
  -U postgres \
  -d byro \
  -f "${ROOT}/data/key-export.sql" \
  --tuples-only \
  --no-align \
  --field-separator=STRING \
> "${LISTFILE}"

echo "Signing keys..."
openssl dgst -sha256 -sign "${KEY_FILE}" -out "${LISTFILE}.sig" "${LISTFILE}"

echo "Key export done. Copy the files in ${TARGET} to an USB stick, and plug it into the portal machine."

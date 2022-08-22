#!/bin/bash
# key-export.sh <target-folder>

ROOT="$(dirname $(realpath $0))"
set -e
source "${ROOT}/data/common-inc.sh"

TARGET="$1"

if [ -z "${TARGET}" ]; then
  echo "usage: $0 <target-folder>"
  exit 1
fi

LISTFILE="${TARGET}/keymembers.json"
SIGFILE="${TARGET}/keymembers.json.sig"

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
openssl dgst -sha256 -sign "${KEY_FILE}" -out "${SIGFILE}.sig" "${LISTFILE}"

echo "Finished."
#!/bin/bash

set -e

KEY="$1"
FILE="$2"

openssl dgst -sha256 -sign "${KEY}" -out "${FILE}.sig" "${FILE}"

# openssl base64 -in /tmp/sign.sha256 -out <signature>
#!/bin/bash

set -e

KEY="$1"
FILE="$2"

openssl dgst -sha256 -sign "$1" -out "$2.sig" "$2"

# openssl base64 -in /tmp/sign.sha256 -out <signature>
#!/bin/bash
#
# Usage:
# ./merge-datasets.sh
#
# Merges the three datasets open.ply, closed.ply and locked.ply into a large dataset
# that can be viewed with the Blender plugin "Point Cloud Visualizer".
#
# Also re-encodes the color values in the PLY files so:
# -   open is #FF0000
# - closed is #00FF00
# - locked is #0000FF
# 
# This way, the points can be easily classified visually.
#

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"

tail -n +11 "${SCRIPT_ROOT}/open.ply"   | awk '{ print $1 " " $2 " " $3 " 255 0 0" }' > "/tmp/merged.ply"
tail -n +11 "${SCRIPT_ROOT}/closed.ply" | awk '{ print $1 " " $2 " " $3 " 0 255 0" }' >> "/tmp/merged.ply"
tail -n +11 "${SCRIPT_ROOT}/locked.ply" | awk '{ print $1 " " $2 " " $3 " 0 0 255" }' >> "/tmp/merged.ply"

echo -n 'ply
format ascii 1.0
element vertex ' > "${SCRIPT_ROOT}/merged.ply"
wc -l /tmp/merged.ply | cut -f 1 -d " " >> "${SCRIPT_ROOT}/merged.ply"
echo 'property float x
property float y
property float z
property uint8 red
property uint8 green
property uint8 blue
end_header' >> "${SCRIPT_ROOT}/merged.ply"

cat /tmp/merged.ply >> "${SCRIPT_ROOT}/merged.ply"

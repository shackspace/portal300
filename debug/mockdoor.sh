#!/bin/bash

# mockdoor.sh <door name>
# Mocks a door implementation with <door name> either being "B" or "C".
# Reacts to MQTT door events and responds immediatly with the changed effect

set -e
SCRIPT_ROOT="$(realpath "$(dirname "$0")")"
door="${1,,}"

if [ -z "${door}" ]; then
  echo "usage: mockdoor.sh <door name>"
  exit 1
fi

function pub()
{
  mosquitto_pub \
  -L "mqtts://mqtt.portal.shackspace.de/$1" \
  -m "$2" \
  --cafile "${SCRIPT_ROOT}/ca.crt" \
  --key    "${SCRIPT_ROOT}/client.key" \
  --cert   "${SCRIPT_ROOT}/client.crt" 
}

pub "shackspace/portal/status/door-control-${door}2" "online"
pub "shackspace/portal/status/door-${door}2" "locked"

state="locked"
counter=0

function sleep_random()
{
  sleep $(( 3 + 2 * $RANDOM / 10000 ))
}

echo "mock door ${door} ready."

mosquitto_sub \
  -F '%t\t%p' \
  -L "mqtts://mqtt.portal.shackspace.de/#" \
  --cafile "${SCRIPT_ROOT}/ca.crt" \
  --key    "${SCRIPT_ROOT}/client.key" \
  --cert   "${SCRIPT_ROOT}/client.crt" \
  | while read -r line; do
    topic="$(echo "${line}" | cut -f 1)"
    data="$(echo "${line}" | cut -f 2)"
    
    # echo "received message:"
    # echo "  topic: ${topic}"
    # echo "  data:  ${data}"

    case ${topic} in

    shackspace/portal/action/door/open-safe)
      if [ "${data}" == "door.${door}2" ]; then

        if [ "${state}" == "locked" ]; then
          echo "opening door ${door} safely..."

          sleep_random # simulate motor activity

          # play sequence "unlock and enter"
          pub "shackspace/portal/status/door-${door}2" "closed"
          echo "door is now closed"

          sleep_random # simulate user walking
          pub "shackspace/portal/status/door-${door}2" "opened"
          echo "door is now open"

          sleep 1
          
          pub "shackspace/portal/status/door-${door}2" "closed"
          echo "door is now closed"
        else
          echo "door ${door} is already open"
        fi
        state="closed"
      fi
      ;;

    shackspace/portal/action/door/open-unsafe)
      if [ "${data}" == "door.${door}2" ]; then

        if [ "${state}" == "locked" ]; then
          echo "open door ${door} unsafely..."

          sleep_random # simulate motor activity

          # play sequence "unlock, don't enter"
          pub "shackspace/portal/status/door-${door}2" "closed"
        else
          echo "door ${door} is already open"
        fi
        state="closed"
      fi
      ;;

    shackspace/portal/action/door/lock)
      if [ "${data}" == "door.${door}2" ]; then
        if [ "${state}" != "locked" ]; then
          echo "lock door ${door}"

          if [ "${state}" == "opened" ]; then
            sleep_random # simulate user activity 
            pub "shackspace/portal/status/door-${door}2" "closed"
          fi
          sleep_random # simulate motor activity
          pub "shackspace/portal/status/door-${door}2" "locked"
        else
          echo "door ${door} is already locked"
        fi
        state="locked"
      fi
      ;;

      *)
        # echo "unhandled topic '${topic}'"
        ;;
    esac

    if [ "${counter}" -gt 10 ]; then
      counter="0"

      pub "shackspace/portal/status/door-${door}2" "${state}"
      echo "door ${door} is now ${state}"
    else
      counter="$(( ${counter} + 1 ))"
    fi

  done
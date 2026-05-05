#!/bin/sh
set -eu

: "${CERTBOT_MODE:?CERTBOT_MODE is required}"
: "${MQTT_DOMAIN:?MQTT_DOMAIN is required}"

if [ "${CERTBOT_MODE}" = "init" ]; then
  : "${CERTBOT_EMAIL:?CERTBOT_EMAIL is required}"

  exec certbot certonly \
    --standalone \
    --non-interactive \
    --agree-tos \
    --email "${CERTBOT_EMAIL}" \
    -d "${MQTT_DOMAIN}" \
    ${CERTBOT_ARGS:-}
fi

if [ "${CERTBOT_MODE}" = "renew" ]; then
  trap 'exit 0' TERM INT

  while :; do
    certbot renew --standalone --non-interactive ${CERTBOT_ARGS:-}
    sleep 12h &
    wait "$!"
  done
fi

echo "Unsupported CERTBOT_MODE: ${CERTBOT_MODE}" >&2
exit 1

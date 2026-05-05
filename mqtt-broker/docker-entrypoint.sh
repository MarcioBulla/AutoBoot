#!/bin/sh
set -eu

: "${MQTT_DOMAIN:?MQTT_DOMAIN is required}"
: "${MQTT_USER:?MQTT_USER is required}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required}"

CERT_DIR="/etc/letsencrypt/live/${MQTT_DOMAIN}"
PASSWD_FILE="/mosquitto/config/mosquitto.passwd"
CONFIG_FILE="/mosquitto/config/mosquitto.conf"
MOSQUITTO_CERT_DIR="/mosquitto/config/certs"
FULLCHAIN_FILE="${MOSQUITTO_CERT_DIR}/fullchain.pem"
PRIVKEY_FILE="${MOSQUITTO_CERT_DIR}/privkey.pem"

if [ ! -f "${CERT_DIR}/fullchain.pem" ] || [ ! -f "${CERT_DIR}/privkey.pem" ]; then
  echo "Missing certificate files in ${CERT_DIR}" >&2
  exit 1
fi

mkdir -p /mosquitto/config /mosquitto/data "${MOSQUITTO_CERT_DIR}"

cp "${CERT_DIR}/fullchain.pem" "${FULLCHAIN_FILE}"
cp "${CERT_DIR}/privkey.pem" "${PRIVKEY_FILE}"
chown -R mosquitto:mosquitto "${MOSQUITTO_CERT_DIR}"
chmod 640 "${FULLCHAIN_FILE}" "${PRIVKEY_FILE}"

rm -f "${PASSWD_FILE}"
mosquitto_passwd -b -c "${PASSWD_FILE}" "${MQTT_USER}" "${MQTT_PASSWORD}"
chown mosquitto:mosquitto "${PASSWD_FILE}" /mosquitto/data
chmod 640 "${PASSWD_FILE}"

cat > "${CONFIG_FILE}" <<EOF
persistence true
persistence_location /mosquitto/data/

log_dest stdout
log_type error
log_type warning
log_type notice
log_type information
connection_messages true
user root

listener 8883
protocol mqtt
certfile ${FULLCHAIN_FILE}
keyfile ${PRIVKEY_FILE}
listener_allow_anonymous false
password_file ${PASSWD_FILE}

listener 443
protocol websockets
certfile ${FULLCHAIN_FILE}
keyfile ${PRIVKEY_FILE}
listener_allow_anonymous false
password_file ${PASSWD_FILE}
EOF

chown mosquitto:mosquitto "${CONFIG_FILE}"

exec mosquitto -c "${CONFIG_FILE}"

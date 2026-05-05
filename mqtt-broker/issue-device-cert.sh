#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "Usage: $0 <base_dir> <device_id> [days]" >&2
  exit 1
fi

BASE_DIR="$1"
DEVICE_ID="$2"
DAYS="${3:-825}"
PKI_DIR="${BASE_DIR}/pki"
CA_DIR="${PKI_DIR}/ca"
OPENSSL_CONF="${CA_DIR}/openssl.cnf"
DEVICE_DIR="${PKI_DIR}/devices/${DEVICE_ID}"

if [ ! -f "${OPENSSL_CONF}" ] || [ ! -f "${CA_DIR}/private/ca.key" ] || [ ! -f "${CA_DIR}/certs/ca.crt" ]; then
  echo "Client CA not initialized. Run mqtt-broker/init-client-ca.sh first." >&2
  exit 1
fi

mkdir -p "${DEVICE_DIR}"
chmod 700 "${DEVICE_DIR}"

openssl genrsa -out "${DEVICE_DIR}/${DEVICE_ID}.key" 2048
chmod 600 "${DEVICE_DIR}/${DEVICE_ID}.key"

openssl req \
  -new \
  -key "${DEVICE_DIR}/${DEVICE_ID}.key" \
  -out "${CA_DIR}/csr/${DEVICE_ID}.csr" \
  -subj "/CN=${DEVICE_ID}"

openssl ca \
  -batch \
  -config "${OPENSSL_CONF}" \
  -extensions client_cert \
  -days "${DAYS}" \
  -notext \
  -in "${CA_DIR}/csr/${DEVICE_ID}.csr" \
  -out "${DEVICE_DIR}/${DEVICE_ID}.crt"

cp "${CA_DIR}/certs/ca.crt" "${DEVICE_DIR}/ca.crt"
chmod 644 "${DEVICE_DIR}/${DEVICE_ID}.crt" "${DEVICE_DIR}/ca.crt"
rm -f "${CA_DIR}/csr/${DEVICE_ID}.csr"

echo "Issued certificate for ${DEVICE_ID} in ${DEVICE_DIR}"

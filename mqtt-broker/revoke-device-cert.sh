#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <base_dir> <device_id>" >&2
  exit 1
fi

BASE_DIR="$1"
DEVICE_ID="$2"
PKI_DIR="${BASE_DIR}/pki"
CA_DIR="${PKI_DIR}/ca"
OPENSSL_CONF="${CA_DIR}/openssl.cnf"
DEVICE_DIR="${PKI_DIR}/devices/${DEVICE_ID}"
DEVICE_CERT="${DEVICE_DIR}/${DEVICE_ID}.crt"

if [ ! -f "${DEVICE_CERT}" ]; then
  echo "Device certificate not found: ${DEVICE_CERT}" >&2
  exit 1
fi

openssl ca \
  -config "${OPENSSL_CONF}" \
  -revoke "${DEVICE_CERT}"

openssl ca \
  -config "${OPENSSL_CONF}" \
  -gencrl \
  -out "${CA_DIR}/crl/client-ca.crl.pem"

echo "Revoked certificate for ${DEVICE_ID}"

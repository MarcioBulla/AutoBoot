#!/bin/sh
set -eu

BASE_DIR="${1:-./runtime}"
CA_CN="${2:-AutoBoot Device CA}"
PKI_DIR="${BASE_DIR}/pki"
CA_DIR="${PKI_DIR}/ca"
OPENSSL_CONF="${CA_DIR}/openssl.cnf"

mkdir -p \
  "${CA_DIR}/certs" \
  "${CA_DIR}/crl" \
  "${CA_DIR}/csr" \
  "${CA_DIR}/newcerts" \
  "${CA_DIR}/private" \
  "${PKI_DIR}/devices"

chmod 700 "${CA_DIR}/private"

if [ ! -f "${CA_DIR}/index.txt" ]; then
  : > "${CA_DIR}/index.txt"
fi

if [ ! -f "${CA_DIR}/serial" ]; then
  printf '1000\n' > "${CA_DIR}/serial"
fi

if [ ! -f "${CA_DIR}/crlnumber" ]; then
  printf '1000\n' > "${CA_DIR}/crlnumber"
fi

cat > "${OPENSSL_CONF}" <<EOF
[ ca ]
default_ca = CA_default

[ CA_default ]
dir               = ${CA_DIR}
certs             = \$dir/certs
crl_dir           = \$dir/crl
database          = \$dir/index.txt
new_certs_dir     = \$dir/newcerts
certificate       = \$dir/certs/ca.crt
serial            = \$dir/serial
crlnumber         = \$dir/crlnumber
crl               = \$dir/crl/client-ca.crl.pem
private_key       = \$dir/private/ca.key
default_md        = sha256
default_days      = 825
default_crl_days  = 30
policy            = policy_loose
copy_extensions   = none
unique_subject    = no

[ policy_loose ]
commonName = supplied

[ req ]
default_bits       = 4096
prompt             = no
default_md         = sha256
distinguished_name = req_distinguished_name
x509_extensions    = v3_ca

[ req_distinguished_name ]
CN = ${CA_CN}

[ v3_ca ]
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints = critical, CA:true
keyUsage = critical, cRLSign, keyCertSign

[ client_cert ]
basicConstraints = CA:false
nsCertType = client
nsComment = "AutoBoot MQTT device certificate"
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = clientAuth
EOF

if [ ! -f "${CA_DIR}/private/ca.key" ]; then
  openssl genrsa -out "${CA_DIR}/private/ca.key" 4096
  chmod 600 "${CA_DIR}/private/ca.key"
fi

if [ ! -f "${CA_DIR}/certs/ca.crt" ]; then
  openssl req \
    -config "${OPENSSL_CONF}" \
    -key "${CA_DIR}/private/ca.key" \
    -new -x509 -days 3650 -sha256 \
    -out "${CA_DIR}/certs/ca.crt"
fi

if [ ! -f "${CA_DIR}/crl/client-ca.crl.pem" ]; then
  openssl ca \
    -config "${OPENSSL_CONF}" \
    -gencrl \
    -out "${CA_DIR}/crl/client-ca.crl.pem"
fi

echo "Client CA initialized in ${CA_DIR}"

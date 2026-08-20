#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-config/tls}"
mkdir -p "${OUT_DIR}"

CA_KEY="${OUT_DIR}/ca.key"
CA_CERT="${OUT_DIR}/ca.crt"
SERVER_KEY="${OUT_DIR}/server.key"
SERVER_CSR="${OUT_DIR}/server.csr"
SERVER_CERT="${OUT_DIR}/server.crt"
EXT_FILE="${OUT_DIR}/server.ext"

openssl req \
  -x509 \
  -newkey rsa:3072 \
  -sha256 \
  -nodes \
  -days 3650 \
  -keyout "${CA_KEY}" \
  -out "${CA_CERT}" \
  -subj "/CN=Chatroom Dev CA"

openssl req \
  -new \
  -newkey rsa:3072 \
  -sha256 \
  -nodes \
  -keyout "${SERVER_KEY}" \
  -out "${SERVER_CSR}" \
  -subj "/CN=localhost"

cat > "${EXT_FILE}" <<'EOF'
subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
EOF

openssl x509 \
  -req \
  -in "${SERVER_CSR}" \
  -CA "${CA_CERT}" \
  -CAkey "${CA_KEY}" \
  -CAcreateserial \
  -out "${SERVER_CERT}" \
  -days 825 \
  -sha256 \
  -extfile "${EXT_FILE}"

chmod 600 "${CA_KEY}" "${SERVER_KEY}"

echo "Generated:"
echo "  CA:     ${CA_CERT}"
echo "  Server: ${SERVER_CERT}"
echo "  Key:    ${SERVER_KEY}"
echo
echo "Do not commit/share the generated private keys."

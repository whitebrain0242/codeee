#!/usr/bin/env bash
set -euo pipefail

TEST_BIN="$1"
OUT_DIR="$2"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

"$(cd "$(dirname "$0")/.." && pwd)/scripts/generate_dev_tls_cert.sh" \
  "127.0.0.1" "${OUT_DIR}" >/dev/null

"${TEST_BIN}" \
  "${OUT_DIR}/server.crt" \
  "${OUT_DIR}/server.key" \
  "${OUT_DIR}/ca.crt"

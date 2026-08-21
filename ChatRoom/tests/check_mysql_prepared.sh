#!/usr/bin/env bash
set -euo pipefail

SOURCE="$1"

required=(
  "mysql_stmt_prepare"
  "mysql_stmt_bind_param"
  "mysql_stmt_execute"
  "mysql_stmt_bind_result"
)

for pattern in "${required[@]}"; do
  if ! grep -q "${pattern}" "${SOURCE}"; then
    echo "missing prepared-statement API: ${pattern}" >&2
    exit 1
  fi
done

forbidden=(
  "mysql_real_escape_string"
  "mysql_query("
  "mysql_real_query("
  "CLIENT_MULTI_STATEMENTS"
  "std::to_string("
)

for pattern in "${forbidden[@]}"; do
  if grep -Fq "${pattern}" "${SOURCE}"; then
    echo "forbidden dynamic SQL pattern found: ${pattern}" >&2
    exit 1
  fi
done

# The old implementation exposed an escape() helper and embedded values into
# SQL strings. The v8.3 database implementation must not contain that helper.
if grep -Eq '(^|[^A-Za-z0-9_])escape\(' "${SOURCE}"; then
  echo "legacy escape() SQL helper is still present" >&2
  exit 1
fi

echo "MySQL prepared-statement source contract passed"

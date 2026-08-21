#!/usr/bin/env bash
set -euo pipefail

ROOT="$1"
CPP="${ROOT}/src/mysql_database.cpp"
SQL="${ROOT}/sql/006_create_friend_blocks.sql"

grep -Fq "CREATE TABLE IF NOT EXISTS friend_blocks" "${SQL}"
grep -Fq "PRIMARY KEY (" "${SQL}"
grep -Fq "blocker_username" "${SQL}"
grep -Fq "blocked_username" "${SQL}"

for method in \
  "is_friend_blocked" \
  "add_friend_block" \
  "remove_friend_block" \
  "list_blocked_friends"; do
  grep -Fq "MySqlDatabase::${method}" "${CPP}"
done

# New blocking queries must remain prepared statements: friend values appear
# as '?' placeholders, not by runtime SQL concatenation.
grep -Fq '"WHERE blocker_username=? "' "${CPP}"
grep -Fq '"AND blocked_username=? "' "${CPP}"


# Offline direct-message/file delivery queries must suppress active blocks.
grep -Fq '"FROM friend_blocks b "' "${CPP}"
grep -Fq '"AND b.blocked_username=m.sender_username"' "${CPP}"
grep -Fq '"AND b.blocked_username=f.sender_username"' "${CPP}"

echo "friend-block persistence contract passed"

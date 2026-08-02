-- Run this file as a MySQL administrator.
-- Replace the example password before running.

CREATE DATABASE IF NOT EXISTS chatroom
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS
    'chatroom'@'127.0.0.1'
    IDENTIFIED BY 'replace_with_a_strong_password';

GRANT
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE,
    INDEX,
    ALTER
ON chatroom.*
TO 'chatroom'@'127.0.0.1';

FLUSH PRIVILEGES;

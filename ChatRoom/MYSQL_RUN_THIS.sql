-- ============================================================
-- Chatroom v7.1 MySQL initialization script
-- ============================================================
--
-- Usage:
--   1. Change the password in BOTH ALTER USER statements below.
--   2. Log in as a MySQL administrator.
--   3. Run:
--
--      SOURCE /absolute/path/to/MYSQL_RUN_THIS.sql;
--
--   Or from a Linux shell:
--
--      sudo mysql < MYSQL_RUN_THIS.sql
--
-- Keep the password here consistent with config/mysql.conf.
-- ============================================================

CREATE DATABASE IF NOT EXISTS chatroom
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS
    'chatroom'@'127.0.0.1'
    IDENTIFIED BY 'xiyou linux';

CREATE USER IF NOT EXISTS
    'chatroom'@'localhost'
    IDENTIFIED BY 'xiyou linux';

ALTER USER
    'chatroom'@'127.0.0.1'
    IDENTIFIED BY 'xiyou linux';

ALTER USER
    'chatroom'@'localhost'
    IDENTIFIED BY 'xiyou linux';

GRANT
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE,
    ALTER,
    INDEX
ON chatroom.*
TO 'chatroom'@'127.0.0.1';

GRANT
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE,
    ALTER,
    INDEX
ON chatroom.*
TO 'chatroom'@'localhost';

USE chatroom;

CREATE TABLE IF NOT EXISTS users (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

    username VARCHAR(20)
        CHARACTER SET ascii
        COLLATE ascii_bin
        NOT NULL,

    password_salt VARBINARY(16) NOT NULL,
    password_hash VARBINARY(32) NOT NULL,
    password_iterations INT UNSIGNED NOT NULL,

    created_at TIMESTAMP NOT NULL
        DEFAULT CURRENT_TIMESTAMP,

    updated_at TIMESTAMP NOT NULL
        DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,

    PRIMARY KEY (id),
    UNIQUE KEY uq_users_username (username)
) ENGINE=InnoDB;

FLUSH PRIVILEGES;

SHOW TABLES;
DESCRIBE users;

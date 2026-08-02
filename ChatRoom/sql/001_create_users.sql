CREATE DATABASE IF NOT EXISTS chatroom
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

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

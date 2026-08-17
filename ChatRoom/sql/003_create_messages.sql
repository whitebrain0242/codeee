CREATE TABLE IF NOT EXISTS messages (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    message_type TINYINT UNSIGNED NOT NULL,
    sender_username VARCHAR(20) NOT NULL,
    recipient_username VARCHAR(20) NULL,
    created_at_unix_ms BIGINT NOT NULL,
    payload LONGBLOB NOT NULL,
    PRIMARY KEY (id),
    INDEX idx_messages_public (message_type, id),
    INDEX idx_messages_sender_recipient (
        message_type,
        sender_username,
        recipient_username,
        id
    ),
    INDEX idx_messages_recipient_sender (
        message_type,
        recipient_username,
        sender_username,
        id
    ),
    CONSTRAINT fk_message_sender
        FOREIGN KEY (sender_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT fk_message_recipient
        FOREIGN KEY (recipient_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT chk_message_type CHECK (message_type IN (1, 2)),
    CONSTRAINT chk_message_recipient CHECK (
        (message_type = 1 AND recipient_username IS NULL) OR
        (message_type = 2 AND recipient_username IS NOT NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

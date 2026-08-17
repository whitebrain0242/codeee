CREATE TABLE IF NOT EXISTS file_transfers (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    transfer_token CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    scope_type TINYINT UNSIGNED NOT NULL,
    sender_username VARCHAR(20) NOT NULL,
    recipient_username VARCHAR(20) NULL,
    group_id BIGINT UNSIGNED NULL,
    file_name VARCHAR(255) NOT NULL,
    file_size BIGINT UNSIGNED NOT NULL,
    sha256_hex CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    stored_relative_path VARCHAR(512) NOT NULL,
    created_at_unix_ms BIGINT NOT NULL,
    metadata LONGBLOB NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_file_transfers_token (transfer_token),
    INDEX idx_file_transfers_sender (sender_username, id),
    INDEX idx_file_transfers_group (group_id, id),
    CONSTRAINT fk_file_transfer_sender
        FOREIGN KEY (sender_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT fk_file_transfer_recipient
        FOREIGN KEY (recipient_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT fk_file_transfer_group
        FOREIGN KEY (group_id) REFERENCES chat_groups(group_id)
        ON DELETE SET NULL,
    CONSTRAINT chk_file_transfer_scope
        CHECK (scope_type IN (1, 2))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS file_transfer_deliveries (
    transfer_id BIGINT UNSIGNED NOT NULL,
    recipient_username VARCHAR(20) NOT NULL,
    delivered_at_unix_ms BIGINT NULL,
    PRIMARY KEY (transfer_id, recipient_username),
    INDEX idx_file_delivery_pending (
        recipient_username,
        delivered_at_unix_ms,
        transfer_id
    ),
    CONSTRAINT fk_file_delivery_transfer
        FOREIGN KEY (transfer_id) REFERENCES file_transfers(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_file_delivery_recipient
        FOREIGN KEY (recipient_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

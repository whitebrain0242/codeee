CREATE TABLE IF NOT EXISTS private_message_deliveries (
    message_id BIGINT UNSIGNED NOT NULL,
    recipient_username VARCHAR(20) NOT NULL,
    delivered_at_unix_ms BIGINT NULL,
    PRIMARY KEY (message_id),
    INDEX idx_private_delivery_pending (
        recipient_username,
        delivered_at_unix_ms,
        message_id
    ),
    CONSTRAINT fk_private_delivery_message
        FOREIGN KEY (message_id) REFERENCES messages(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_private_delivery_recipient
        FOREIGN KEY (recipient_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS chat_groups (
    group_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    group_name VARCHAR(32) NOT NULL,
    owner_username VARCHAR(20) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id),
    UNIQUE KEY uq_chat_groups_name (group_name),
    CONSTRAINT fk_chat_group_owner
        FOREIGN KEY (owner_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS group_members (
    group_id BIGINT UNSIGNED NOT NULL,
    username VARCHAR(20) NOT NULL,
    member_role TINYINT UNSIGNED NOT NULL,
    joined_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, username),
    INDEX idx_group_members_username (username, group_id),
    CONSTRAINT fk_group_member_group
        FOREIGN KEY (group_id) REFERENCES chat_groups(group_id)
        ON DELETE CASCADE,
    CONSTRAINT fk_group_member_user
        FOREIGN KEY (username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT chk_group_member_role
        CHECK (member_role IN (1, 2, 3))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS group_join_requests (
    group_id BIGINT UNSIGNED NOT NULL,
    requester_username VARCHAR(20) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, requester_username),
    INDEX idx_group_join_request_user (requester_username, group_id),
    CONSTRAINT fk_group_join_request_group
        FOREIGN KEY (group_id) REFERENCES chat_groups(group_id)
        ON DELETE CASCADE,
    CONSTRAINT fk_group_join_request_user
        FOREIGN KEY (requester_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS group_messages (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    group_id BIGINT UNSIGNED NOT NULL,
    sender_username VARCHAR(20) NOT NULL,
    created_at_unix_ms BIGINT NOT NULL,
    payload LONGBLOB NOT NULL,
    PRIMARY KEY (id),
    INDEX idx_group_messages_history (group_id, id),
    CONSTRAINT fk_group_message_group
        FOREIGN KEY (group_id) REFERENCES chat_groups(group_id)
        ON DELETE CASCADE,
    CONSTRAINT fk_group_message_sender
        FOREIGN KEY (sender_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS group_message_deliveries (
    message_id BIGINT UNSIGNED NOT NULL,
    recipient_username VARCHAR(20) NOT NULL,
    delivered_at_unix_ms BIGINT NULL,
    PRIMARY KEY (message_id, recipient_username),
    INDEX idx_group_delivery_pending (
        recipient_username,
        delivered_at_unix_ms,
        message_id
    ),
    CONSTRAINT fk_group_delivery_message
        FOREIGN KEY (message_id) REFERENCES group_messages(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_group_delivery_recipient
        FOREIGN KEY (recipient_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

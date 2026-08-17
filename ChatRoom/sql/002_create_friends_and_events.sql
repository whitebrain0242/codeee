CREATE TABLE IF NOT EXISTS friend_requests (
    requester_username VARCHAR(20) NOT NULL,
    target_username VARCHAR(20) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (requester_username, target_username),
    CONSTRAINT fk_friend_request_requester
        FOREIGN KEY (requester_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT fk_friend_request_target
        FOREIGN KEY (target_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT chk_friend_request_not_self
        CHECK (requester_username <> target_username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS friendships (
    user_low VARCHAR(20) NOT NULL,
    user_high VARCHAR(20) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_low, user_high),
    CONSTRAINT fk_friendship_low
        FOREIGN KEY (user_low) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT fk_friendship_high
        FOREIGN KEY (user_high) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT chk_friendship_order CHECK (user_low < user_high)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS friend_events (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    event_type TINYINT UNSIGNED NOT NULL,
    actor_username VARCHAR(20) NOT NULL,
    target_username VARCHAR(20) NOT NULL,
    occurred_at_unix_ms BIGINT NOT NULL,
    payload BLOB NOT NULL,
    PRIMARY KEY (id),
    INDEX idx_friend_events_actor (actor_username, id),
    INDEX idx_friend_events_target (target_username, id),
    CONSTRAINT fk_friend_event_actor
        FOREIGN KEY (actor_username) REFERENCES users(username)
        ON DELETE CASCADE,
    CONSTRAINT fk_friend_event_target
        FOREIGN KEY (target_username) REFERENCES users(username)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS friend_blocks (
    blocker_username VARCHAR(20) NOT NULL,
    blocked_username VARCHAR(20) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (
        blocker_username,
        blocked_username
    ),

    INDEX idx_friend_blocks_blocked (
        blocked_username,
        blocker_username
    ),

    CONSTRAINT fk_friend_block_blocker
        FOREIGN KEY (blocker_username)
        REFERENCES users(username)
        ON DELETE CASCADE,

    CONSTRAINT fk_friend_block_blocked
        FOREIGN KEY (blocked_username)
        REFERENCES users(username)
        ON DELETE CASCADE,

    CONSTRAINT chk_friend_block_not_self
        CHECK (
            blocker_username <>
            blocked_username
        )
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_bin;

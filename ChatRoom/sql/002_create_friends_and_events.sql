GRANT
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE,
    ALTER,
    INDEX,
    REFERENCES
ON chatroom.*
TO 'chatroom'@'127.0.0.1';

GRANT
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE,
    ALTER,
    INDEX,
    REFERENCES
ON chatroom.*
TO 'chatroom'@'localhost';

USE chatroom;


CREATE TABLE IF NOT EXISTS friend_requests(
    sender_user_id BIGINT UNSIGNED NOT NULL,
    receiver_user_id BIGINT UNSIGNED NOT NULL,
    created_at TIMESTAMP(3) NOT NULL
        DEFAULT CURRENT_TIMESTAMP(3),-- 自动填上当前时间，精确到毫秒级别
    -- 唯一并且不为空，并且把这两个绑定在一起,保证发送好友请求只能一次
    PRIMARY KEY(
        sender_user_id,
        receiver_user_id
    ),
-- 创建一个符合索引，加速查询某个用户受到的好友请求，这个高频操作
    KEY idx_friend_requests_receiver(
        receiver_user_id,--同一个人受到的请求
        created_at--在接受者相同的情况下，索引按照时间进行自动排序
    ),
-- 强制要求sender ID必须是USER表中真实存在的ID,并且一旦注销，发出的所有请求都被自动清理干净
    CONSTRAINT fk_friend_requests_sender
        FOREIGN KEY (sender_user_id)-- 外键，受限于另一个表
        REFERENCES users(id)-- 只能引用USER表中的ID列，只有在USER表真实存在的才能填入SENDERID
        ON DELETE CASCADE,-- 级联删除：当USER中用户注销，会把用户的所有请求删除
    
    CONSTRAINT fk_friend_requests_receiver
        FOREIGN KEY (receiver_user_id)
        REFERENCES users(id)
        ON DELETE CASCADE,
    
    CONSTRAINT chk_friend_request_users
        CHECK(---  检查条件，这两个不相等必须成立，也就是不能给自己发好友请求
            sender_user_id<>receiver_user_id
        )
)ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friendships(-- 存储已经互相确认的好友对，并且强制排序
    user_id_low BIGINT UNSIGNED NOT NULL,-- 必须小的放在LOW,大的放在HIGH
    user_id_high BIGINT UNSIGNED NOT NULL,

    created_at TIMESTAMP(3) NOT NULL
        DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY(-- 保证同一对好友只能存在一对，不能再次加好友
        user_id_low,
        user_id_high
    ),

    KEY idx_friendships_high(
        user_id_high
    ),

    CONSTRAINT fk_friendships_low
        FOREIGN KEY(user_id_low)
        REFERENCES users(id)
        ON DELETE CASCADE,

    CONSTRAINT fk_friendships_high
        FOREIGN KEY(user_id_high)
        REFERENCES users(id)
        ON DELETE CASCADE,

    CONSTRAINT chk_friendship_order
    CHECK(
        user_id_low<user_id_high
    )
)ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friend_events(
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

    actor_user_id BIGINT UNSIGNED NOT NULL,
    target_user_id BIGINT UNSIGNED NOT NULL,

    payload BLOB NOT NULL,-- 内容正文，保存的是经过PROTOBUF序列化后的二进制字节流
    created_at TIMESTAMP(3) NOT NULL
        DEFAULT CURRENT_TIMESTAMP(3),
    
    PRIMARY KEY (id),
    KEY idx_friend_events_actor(
        actor_user_id,
        id
    ),
    KEY idx_friend_events_target(
        target_user_id,
        id
    ),

    CONSTRAINT fk_friend_events_actor
        FOREIGN KEY (actor_user_id)
        REFERENCES users(id)
        ON DELETE CASCADE,

    CONSTRAINT fk_friend_events_target
        FOREIGN KEY (target_user_id)
        REFERENCES users(id)
        ON DELETE CASCADE
)ENGINE=InnoDB;
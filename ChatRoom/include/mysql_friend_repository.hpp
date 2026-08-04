#pragma once

#include "database_config.hpp"
#include "friend_repository.hpp"

#include <mysql/mysql.h>
//使用mysql作为底层存储，具体实现好友系统的所有操作的SQL逻辑
namespace chat{
class MySqlFriendRepository final//这个类不能被继承
    : public IFriendRepository {//继承自抽象接口
public:
//构造函数接受数据库配置
    explicit MySqlFriendRepository(
        DatabaseConfig config
    );

    ~MySqlFriendRepository() override;

    MySqlFriendRepository(
        const MySqlFriendRepository&
    ) = delete;

    MySqlFriendRepository& operator=(
        const MySqlFriendRepository&
    ) = delete;
//建表
    bool initialize(
        std::string& error
    ) override;
//联合查询friendship和friendrequests表，填充friendstate结构体
    bool load_state(
        FriendState& state,
        std::string& error
    ) override;
//
    FriendMutationResult create_request(
        const std::string& sender_username,
        const std::string& receiver_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    FriendMutationResult accept_request(
        const std::string& requester_username,
        const std::string& accepter_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    FriendMutationResult reject_request(
        const std::string& requester_username,
        const std::string& rejecter_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    FriendMutationResult remove_friend(
        const std::string& actor_username,
        const std::string& target_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    bool load_recent_events(
        const std::string& username,
        std::size_t count,
        std::vector<StoredFriendEvent>& events,
        std::string& error
    ) override;

private:
    bool connect(std::string& error);//建立与mysql数据库的连接
    bool ensure_connected(std::string& error);
    bool create_schema(std::string& error);

    bool begin_transaction(std::string& error);//执行 START TRANSACTION，开启数据库事务
    bool commit_transaction(std::string& error);//执行 COMMIT，提交事务
    void rollback_transaction();//执行 ROLLBACK，回滚事务（出错时调用）

    //通用的“写事件日志”方法
    bool insert_event(
        const std::string& actor_username,
        const std::string& target_username,
        const std::string& protobuf_event,
        std::string& error
    );

    //释放 MySQL 连接句柄，清理资源
    void close_connection();

    
    DatabaseConfig config_;
    MYSQL* connection_ = nullptr;
};
}
#pragma once

#include "mysql_database.hpp"
#include "protocol.hpp"
#include "integration/redis_client.hpp"
#include "file_transfer_service.hpp"
#include "server/client_session.hpp"
#include "server/command_router.hpp"
#include "server/direct_message_policy.hpp"
#include "server/heartbeat_manager.hpp"
#include "server/online_user_registry.hpp"

#include "minimuduo/net/Callbacks.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace minimuduo::net {
class Buffer;
class TcpServer;
}

class ChatServer {
public:
    using TcpConnectionPtr = minimuduo::net::TcpConnectionPtr;

    ChatServer(
        minimuduo::net::TcpServer& tcp_server,
        MySqlDatabase& database,
        RedisClient& redis,
        std::string server_instance_id,//在redis中的ID
        unsigned int presence_ttl_seconds,//过期时间
        std::filesystem::path file_storage_root//文件系统路径
    );

    ~ChatServer();

    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;

private:
    static constexpr std::size_t kMaxInputBuffer = 8192;//单条TCP消息的最大缓冲区
    static constexpr std::size_t kMaxChatMessage = 1000;//单条聊天消息的最大字节数
    static constexpr std::size_t kDefaultHistoryCount = 20;//默认查询消息数量
    static constexpr std::size_t kMaxHistoryCount = 100;//查询的最大数量
    static constexpr std::size_t kOfflineDeliveryBatch = 100;//登录时推送的消息上线
    static constexpr std::size_t kOfflineFileDeliveryBatch = 10;//一次性最多推送文件数量
    static constexpr std::uint64_t kMaxFileSize = 20ULL * 1024ULL * 1024ULL;//20MB，上传的最大文件大小
    static constexpr std::size_t kMaxFileChunkBytes = 3072U;//单个网络包解码后的最大字节数

    minimuduo::net::TcpServer& tcp_server_;
    MySqlDatabase& database_;
    RedisClient& redis_;
    std::string server_instance_id_;//服务器ID
    unsigned int presence_ttl_seconds_;//redis在线状态的过期时间
    FileTransferService file_transfer_service_;//文件传输引擎
    //Redis在线状态刷新线程
    std::atomic<bool> stopping_{false};
    std::mutex presence_wait_mutex_;
    std::condition_variable presence_wait_cv_;
    std::thread presence_refresh_thread_;

    OnlineUserRegistry online_users_;//在线注册表
    DirectMessagePolicy direct_message_policy_;//私聊/传文件前的检测-是否好友-是否屏蔽
    HeartbeatManager heartbeat_manager_;//扫描所有连接，定时发送PING，超市强制关闭
    ServerCommandRouter command_router_;//命令路由器

    //为什么使用两个锁？这是因为，两个操作独立，如果共用一个锁的话，彼此之间需要等待，降低服务器的并发能力
    std::mutex friend_operation_mutex_;//好友操作互斥锁
    std::mutex group_operation_mutex_;//群组操作互斥锁

    void on_connection(const TcpConnectionPtr& connection);
    void on_message(
        const TcpConnectionPtr& connection,
        minimuduo::net::Buffer* buffer
    );

    void configure_command_routes();

    void handle_command(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const Command& command
    );

    void send_help(const TcpConnectionPtr& connection);

    void handle_register(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_login(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_logout(
        const TcpConnectionPtr& connection,
        ClientSession& session
    );

    void handle_public_message(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& message
    );

    void handle_private_message(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_who(
        const TcpConnectionPtr& connection,
        const ClientSession& session
    );

    void handle_add_friend(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_accept_friend(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_reject_friend(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_remove_friend(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );


    void handle_block_friend(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_unblock_friend(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_blocked_friends(
        const TcpConnectionPtr& connection,
        const ClientSession& session
    );

    void handle_friends(
        const TcpConnectionPtr& connection,
        const ClientSession& session
    );

    void handle_friend_requests(
        const TcpConnectionPtr& connection,
        const ClientSession& session
    );

    void handle_history_public(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_history_private(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_create_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_dissolve_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_apply_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_my_groups(
        const TcpConnectionPtr& connection,
        const ClientSession& session
    );

    void handle_leave_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_group_members(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_add_group_admin(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_remove_group_admin(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_group_requests(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_approve_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_reject_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_remove_group_member(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_group_message(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_history_group(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& arguments
    );

    void handle_pending(
        const TcpConnectionPtr& connection,
        const ClientSession& session
    );


    void handle_ping(
        const TcpConnectionPtr& connection,
        const std::string& arguments
    );

    void handle_pong(
        const TcpConnectionPtr& connection,
        const std::string& arguments
    );
    
    void handle_file_begin_private(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_begin_group(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_chunk(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_end(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_abort(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_resume_request(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_received(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void handle_file_receive_failed(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& arguments
    );

    void deliver_pending_files(
        const TcpConnectionPtr& connection,
        const std::string& username
    );

    void deliver_file_to_user(
        const StoredFileTransfer& transfer,
        const std::string& recipient,
        const TcpConnectionPtr& connection
    );

    void detach_active_upload(
        ClientSession& session
    );

    void pause_file_upload(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& token,
        const std::string& reason
    );

    void reject_file_upload(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const std::string& token,
        const std::string& reason
    );

    std::shared_ptr<ClientSession> session_of(
        const TcpConnectionPtr& connection
    ) const;

    bool require_login(
        const TcpConnectionPtr& connection,
        const ClientSession& session,
        const std::string& action
    ) const;

    bool extract_single_username(
        const TcpConnectionPtr& connection,
        const std::string& arguments,
        const std::string& usage,
        std::string& username
    ) const;

    bool extract_single_group_name(
        const TcpConnectionPtr& connection,
        const std::string& arguments,
        const std::string& usage,
        std::string& group_name
    ) const;

    bool extract_group_and_username(
        const TcpConnectionPtr& connection,
        const std::string& arguments,
        const std::string& usage,
        std::string& group_name,
        std::string& username
    ) const;

    bool find_online_user(
        const std::string& username,
        TcpConnectionPtr& connection
    );

    bool is_user_online(const std::string& username);

    bool register_online_user(
        const std::string& username,
        const TcpConnectionPtr& connection
    );

    void remove_online_user(
        const std::string& username,
        const TcpConnectionPtr& connection
    );

    void notify_user_if_online(
        const std::string& username,
        const std::string& message
    );

    void broadcast_to_logged_in(
        const std::string& message,
        const TcpConnectionPtr& except = {}
    );

    void notify_pending_requests(
        const TcpConnectionPtr& connection,
        const std::string& username
    );

    void deliver_pending_messages(
        const TcpConnectionPtr& connection,
        const std::string& username
    );

    void notify_group_managers(
        const std::string& group_name,
        const std::string& message,
        const std::string& except_username = {}
    );

    static bool is_valid_username(const std::string& username);
    static bool is_valid_password(const std::string& password);
    static bool is_valid_group_name(const std::string& group_name);

    static std::int64_t now_unix_ms();
    static std::string format_unix_ms(std::int64_t value);
    static std::string join_names(const std::vector<std::string>& names);
    static std::string group_role_name(GroupRole role);
    static bool is_group_manager(GroupRole role);

    void presence_refresh_loop();

    void refresh_all_presence_best_effort();

    bool claim_redis_presence(
        const std::string& username,
        const TcpConnectionPtr& connection
    );

    void remove_redis_presence_best_effort(
        const std::string& username
    );

    void adjust_redis_unread_best_effort(
        const std::string& username,
        const std::string& kind,
        std::int64_t delta
    );

    void send_redis_unread_summary_best_effort(
        const TcpConnectionPtr& connection,
        const std::string& username
    );

    void database_error(
        const TcpConnectionPtr& connection,
        const std::string& operation,
        const std::string& error
    ) const;
};


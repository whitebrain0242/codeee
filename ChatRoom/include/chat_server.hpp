#pragma once

#include "message_store.hpp"
#include "protocol.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
// v3新增：服务端类
namespace chat
{
    struct ClientSession
    {
        // v2新增内容: 昵称
        // v3新增：登陆
        int fd = -1;
        bool logged_in = false;
        std::string username;           // 新增昵称
        bool close_after_write = false; // 当用户要退出时-》先把输出缓冲区中的数据发完，然后再关闭连接---处理QUIT
        std::string in_buffer;
        std::string out_buffer;
    };
    // v3新增：暂时使用明文密码
    // v5新增：账号保存：好友列表，收到好友申请列表，发送好友申请列表
    struct UserAccount
    {
        std::string username;
        std::string password;

        //v5新增好友功能
        std::unordered_set<std::string>friends;//好友列表
        std::unordered_set<std::string>incoming_friend_requests;
        std::unordered_set<std::string>outgoing_friend_requests;
    };

    class ChatServer
    {
    public:
        explicit ChatServer(int port);
        ~ChatServer();

        ChatServer(const ChatServer &) = delete;
        ChatServer &operator=(const ChatServer &) = delete;

        bool initialize();
        int run();

    private:
        static constexpr int kMaxEvents = 1024;
        static constexpr std::size_t kReadBufferSize = 4096;
        static constexpr std::size_t kMaxInputBuffer = 8192;
        static constexpr std::size_t kMaxChatMessage = 1000;
        static constexpr std::size_t kDefaultHistoryCount = 20;
        static constexpr std::size_t kMaxHistoryQueryCount = 100;

        int port_;
        int listen_fd_ = -1;
        int epoll_fd_ = -1;

        // v3新增：所有用户users=online_users+未注册用户
        std::unordered_map<int, ClientSession> clients_;//所有建立了TCP连接的客户：fd+信息体
        std::unordered_map<std::string, UserAccount> users_;//保存已经注册过的信息
        std::unordered_map<std::string, int> online_users_;

        //存最近的消息
        InMemoryMessageStore message_store_;

        
        bool create_listen_socket();
        bool create_epoll();
        bool add_listen_socket_to_epoll();

        static bool set_non_blocking(int fd);//把fd设置成非阻塞模式
        

        
        void accept_new_clients();
        void close_client(int client_fd,bool announce_offline = true);
        void update_epoll_events(int client_fd);
        void queue_message(int client_fd, const std::string &message);

        void handle_client_read(int client_fd);
        void handle_client_write(int client_fd);
        
        //业务命令处理
        bool handle_command(int client_fd, const std::string &line);

        void send_help(int client_fd);
        void handle_register(int clinet_fd,const Command& command);
        void handle_login(int client_fd,const Command& command);
        void handle_logout(int client_fd);//等出但是保持tcp连接
        void handle_public_message(int client_fd,const Command& command);//广播消息
        void handle_private_message(int client_fd,const Command& command);//私聊特定用户
        void handle_who(int client_fd);
        void handle_add_friend(int client_fd,const Command& command);//发送好友申请
        void handle_accept_friend(int client_fd,const Command& command);//接受
        void handle_reject_friend(int client_fd,const Command& command);
        void handle_remove_friend(int client_fd,const Command& command);


        void send_friend_list(int client_fd);//发好友列表
        void send_friend_requests(int client_fd);//发送待处理的好友申请列表

        void handle_public_history(int client_fd,const Command& command);
        void handle_private_history(int client_fd,const Command&command);

        void broadcast_to_logged_in(const std::string &message, int exclude_fd=-1);//值发送给一登陆用户
        void notify_user_if_online(const std::string& username,const std::string& message);//目标是否在线，发欧式能够系统通知

        void send_private_message(int sender_fd,const std::string& target_username, const std::string& message);
        bool require_login( int client_fd, const std::string& action);
        //静态检验和工具函数
        static bool is_valid_username(const std::string &username);
        static bool is_valid_password(const std::string &password);

        bool are_friends(const std::string& left,const std::string& right)const;
        bool extract_single_username(int client_fd,const Command& command,const std::string& usage,std::string& username);//从原始字符串提取一个用户名
        
        static std::string join_sorted(const std::unordered_set<std::string>& values);//把set排序后拼接称字符串

        //v6新增，处理查询历史消息
       static std::string format_public_history_line(const ChatMessage& message);

       static std::string format_private_history_line(const ChatMessage& message);
        
    };

}
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
// v3新增：服务端类
namespace chat
{
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
        struct ClientSession
        {
            // v2新增内容: 昵称
            // v3新增：登陆
            int fd = -1;
            bool logged_in = false;
            std::string username; // 新增昵称
            bool has_nickname = false;
            bool close_after_write = false; // 当用户要退出时-》先把输出缓冲区中的数据发完，然后再关闭连接---处理QUIT
            std::string in_buffer;
            std::string out_buffer;
        };
        // v3新增：暂时使用明文密码
        struct UserAccount
        {
            std::string username;
            std::string password;
        };

        static constexpr int kMaxEvents = 1024;
        static constexpr std::size_t kReadBufferSize = 4096;
        static constexpr std::size_t kMaxInputBuffer = 8192;
        static constexpr std::size_t kMaxChatMessage = 1000;
        static constexpr std::size_t kMinUsernameLength = 3;
        static constexpr std::size_t kMaxUsernameLength = 20;
        static constexpr std::size_t kMinPasswordLength = 4;
        static constexpr std::size_t kMaxPasswordLength = 64;

        int port_;
        int listen_fd_ = -1;
        int epoll_fd_ = -1;

        // v3新增：所有用户users=online_users+未注册用户
        std::unordered_map<int, ClientSession> clients_;
        std::unordered_map<std::string, UserAccount> users_;
        std::unordered_map<std::string, int> online_users_;

        static int set_non_blocking(int fd);
        static bool is_valid_username(const std::string &username);
        static bool is_valid_password(const std::string &password);

        bool create_listen_socket();
        bool create_epoll_instance();

        void accept_new_clients();
        void handle_client_read(int client_fd);
        void handle_client_write(int client_fd);
        bool handle_command(int client_fd, const std::string &line);

        void update_epoll_events(int client_fd);
        void queue_message(int client_fd, const std::string &message);
        void broadcast_to_logged_in(
            int sender_fd,
            const std::string &message,
            bool include_sender);

        void send_help(int client_fd);
        void send_online_users(int client_fd);
        void logout_client(int client_fd, bool notify_self, bool notify_others);
        void close_client(int client_fd);
        void close_all();
    };

}
#include "chat_server.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace chat
{

    ChatServer::ChatServer(int port) : port_(port) {}; // 没看懂e
    ChatServer::~ChatServer()
    {
        close_all();
    }
    int ChatServer::set_non_blocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            return -1;
        }
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    // v2新增解析命令函数
    // 实现：首先判断长度是1到20之间，其次只能游数字字母下划线构成，其他组成不通过
    // 目的：判断昵称是否合规

    bool ChatServer::is_valid_username(const std::string &username)
    {
        if (username.size() < kMinUsernameLength || username.size() > kMaxUsernameLength)
            return false;
        for (char ch : username)
        {
            const unsigned char value = static_cast<unsigned char>(ch);
            if (!std::isalnum(value) && ch != '_')
                return false;
        }
        return true;
    }

    // v3新增密码检验函数
    // 实现：先看大小符合不符合，再看有没有空格或者控制字符
    bool ChatServer::is_valid_password(const std::string &password)
    {
        if (password.size() < kMinPasswordLength || password.size() > kMaxPasswordLength)
        {
            return false;
        }
        for (char ch : password)
        {
            const unsigned char value = static_cast<unsigned char>(ch);
            if (std::isspace(value) || std::iscntrl(value))
                return false;
        }
        return true;
    }

    bool ChatServer::create_listen_socket()
    {
        listen_fd_= socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == -1)
        {
            std::cerr << "socket failed: " << strerror(errno) << std::endl;
            return false;
        }

        int opt = 1;
        if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
        {
            std::cerr << "setsockopt failed: " << strerror(errno) << std::endl;
            close(listen_fd_);
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1)
        {
            std::cerr << "bind failed: " << strerror(errno) << std::endl;
            return false;
        }

        if (listen(listen_fd_, SOMAXCONN) == -1)
        {
            std::cerr << "listen failed: " << strerror(errno) << std::endl;
            return false;
        }

        if (set_non_blocking(listen_fd_) == -1)
        {
            std::cerr << "set_non_blocking failed: " << strerror(errno) << std::endl;
            return false;
        }

        return true;
    }
    // v3新增：创建一个epoll,并且把监听socket加进去
    bool ChatServer::create_epoll_instance()
    {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1)
        {
            std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
            return false;
        }

        epoll_event listen_event{};
        listen_event.data.fd = listen_fd_;
        listen_event.events = EPOLLIN;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listen_event) == -1)
        {
            std::cerr << "epoll_ctl ADD listen socket failed: "
                      << std::strerror(errno) << '\n';
            return false;
        }
        return true;
    }
    // v3新增函数：服务端启动前的检验
    bool ChatServer::initialize()
    {
        if (!create_listen_socket())
        {
            return false;
        }
        if (!create_epoll_instance())
        {
            return false;
        }
        std::cout << "chat_server v3 started on port " << port_ << '\n';
        return true;
    }

    void ChatServer::update_epoll_events(int client_fd)
    {
        const auto it = clients_.find(client_fd);
        if (it == clients_.end())
        {
            return;
        }

        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLRDHUP;

        if (!it->second.close_after_write)
        {
            event.events |= EPOLLIN;
        }

        if (!it->second.out_buffer.empty())
        {
            event.events |= EPOLLOUT;
        }

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event) == -1)
        {
            std::cerr << "epoll_ctl MOD failed, fd=" << client_fd
                      << ", error=" << std::strerror(errno) << '\n';
        }
    }

    void ChatServer::queue_message(int client_fd, const std::string& message)
    {
        auto it = clients_.find(client_fd);
        if (it == clients_.end())
        {
            return;
        }

        it->second.out_buffer += message;
        update_epoll_events(client_fd);
    }

    // v3：新增群发广播，把一条消息发给当前所有登陆用户
    // 实现：筛选一登陆用户，是否回显，放入发送队列
    void ChatServer::broadcast_to_logged_in(int sender_fd, const std::string &message, bool include_sender)
    {
        for (const auto &[client_fd, client] : clients_)
        {
            if (!client.logged_in)
                continue;
            if (!include_sender && client_fd == sender_fd)
                continue;
            queue_message(client_fd, message);
        }
    }

    void ChatServer::send_help(int client_fd)
    {
        queue_message(
            client_fd,
            "[system] commands:\n"
            "  HELP                           show this help\n"
            "  REGISTER <username> <password> create an account\n"
            "  LOGIN <username> <password>    log in\n"
            "  LOGOUT                         log out but keep TCP connected\n"
            "  SAY <message>                  send a public message\n"
            "  WHO                            list logged-in online users\n"
            "  QUIT                           leave the server\n");
    }

    // v3：把每一个命令拆出来
    // 实现：先找是哪一个客户端-》进而判断是否登陆了，创建一个数组接着预留空间，增加元素，排序，拼接恢复，输出
    void ChatServer::send_online_users(int client_fd)
    {

        const auto client_it = clients_.find(client_fd);
        if (client_it == clients_.end())
            return;


        if (!client_it->second.logged_in)
        {
            queue_message(client_fd, "[error] you must LOGIN befor using WHO .\n");
            return;
        }

        std::vector<std::string> usernames;
        usernames.reserve(online_users_.size());

        for (const auto &[username, fd] : online_users_)
        {
            (void)fd;
            usernames.push_back(username);
        }

        std::sort(usernames.begin(), usernames.end());

        std::string response =
            "[system] online users (" + std::to_string(usernames.size()) + "): ";

        for (std::size_t i = 0; i < usernames.size(); ++i)
        {
            if (i != 0)
            {
                response += ", ";
            }
            response += usernames[i];
        }
        response += "\n";

        queue_message(client_fd, response);
    }

    //v3新增函数：用户注销退出函数
    //实现：1，安全检查：检查fd检查是否登陆2.清理数据：onlineusers,loggedin,名字清除3.通知
    void ChatServer::logout_client(int client_fd,bool notify_self,bool notify_others){
        const auto it=clients_.find(client_fd);
        if(it==clients_.end())return;
        ClientSession& client =it->second;
        if(!client.logged_in){
            if(notify_self){
                queue_message(client_fd,"[error] you are not logged in .\n");
            }
            return;

        }
        const std::string username=client.username;

        online_users_.erase(username);
        client.logged_in=false;
        client.username.clear();
        if(notify_self){
            queue_message(client_fd,"[system] logout successful .\n");
        }
        if(notify_others){
            broadcast_to_logged_in(client_fd,"[system] "+username+" is offline.\n",false);
        }
        std::cout<<"user logged out, username=" << username
              << ", fd=" << client_fd << '\n';
    }

    bool ChatServer::handle_command(int client_fd,const std::string &line)
{
    auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end())
    {
        return false;
    }

    const ParsedCommand command = parse_command(line);

    if (command.name.empty())
    {
        queue_message(client_fd, "[error] empty command.\n");
        return true;
    }

    if (command.name == "HELP")
    {
        send_help(client_fd);
        return true;
    }

    //v3新增注册函数
    //实现：1.先取客户端信息2.检查是否一登陆3.检查参数是不是2个4.检查名字和密码是否可以使用5.检查名字是否已经被使用
    //     最后增加名字和密码，发送信息
    if (command.name == "REGISTER"){
        ClientSession& client=client_it->second;
        if(client.logged_in){
            queue_message(client_fd,"[error] LOGOUT before registering another account.\n");
            return true;
        }
        if(command.arguments.size()!=2){
            queue_message(client_fd,"[error] usage: REGISTER <username> <password>\n");
            return true;
        }

        const std::string& username = command.arguments[0];
        const std::string& password = command.arguments[1];
        if(!is_valid_username(username)){
            queue_message(
                client_fd,
                "[error] username must be 3-20 characters and contain "
                "only letters, digits, or underscore.\n"
            );
            return true;
        }
        if(!is_valid_password(password)){
            queue_message(
                client_fd,
                "[error] password must be 4-64 non-space characters.\n"
            );
            return true;
        }
        //看看是不是最后一个
        if(users_.find(username)!=users_.end()){
            queue_message(client_fd, "[error] username already exists.\n");
            return true;
        }
        users_.emplace(username,UserAccount{username,password});
        queue_message(
            client_fd,
            "[system] registration successful. "
            "Use LOGIN <username> <password> to log in.\n"
        );

        std::cout << "account registered, username=" << username << '\n';
        return true;
    }
    //v3新增登陆函数
    //实现：1.取客户端2.检查是否登陆3.检查参数4.身份验证
    if (command.name == "LOGIN"){
        ClientSession& client =client_it->second;

        if(client.logged_in){
            queue_message(
                client_fd,
                "[error] you are already logged in as " +
                client.username + ".\n"
            );
            return true;
        }
        if(command.arguments.size()!=2){
            queue_message(
                client_fd,
                "[error] usage: LOGIN <username> <password>\n"
            );
            return true;
        }
        const std::string& username = command.arguments[0];
        const std::string& password = command.arguments[1];

        const auto account_it=users_.find(username);

        //查看账号是否存在并且密码是否正确
        if(account_it==users_.end()||account_it->second.password!=password){
            queue_message(
                client_fd,
                "[error] invalid username or password.\n"
            );
            return true;
        }
        //在在线用户列表里面查找用户是否存在，防止双开
        if(online_users_.find(username)!=online_users_.end()){
            queue_message(
                client_fd,
                "[error] this account is already logged in.\n"
            );
            return true;
        }
        client.logged_in=true;
        client.username=username;
        online_users_[username]=client_fd;
        queue_message(
            client_fd,
            "[system] login successful. Welcome, " + username + ".\n"
        );

        broadcast_to_logged_in(
            client_fd,
            "[system] " + username + " is online.\n",
            false
        );

        std::cout << "user logged in, username=" << username
                  << ", fd=" << client_fd << '\n';
        return true;

    }
    //v3新增函数,账号退出
    if (command.name == "LOGOUT"){
        if(!command.arguments.empty()){
            queue_message(client_fd, "[error] usage: LOGOUT\n");
            return true;
        }
        logout_client(client_fd, true, true);
        return true;
    }

    //v3新增登录验证
    if (command.name == "SAY")
    {
        const ClientSession& client = client_it->second;

        if (!client.logged_in) {
            queue_message(
                client_fd,
                "[error] you must LOGIN before chatting.\n"
            );
            return true;
        }

        if (command.raw_arguments.empty()) {
            queue_message(client_fd, "[error] usage: SAY <message>\n");
            return true;
        }

        if (command.raw_arguments.size() > kMaxChatMessage) {
            queue_message(
                client_fd,
                "[error] message is too long; maximum is " +
                std::to_string(kMaxChatMessage) + " bytes.\n"
            );
            return true;
        }

        const std::string chat_message =
            "[" + client.username + "] " +
            command.raw_arguments + "\n";

        broadcast_to_logged_in(client_fd, chat_message, true);

        std::cout << "chat from username=" << client.username
                  << ": " << command.raw_arguments << '\n';
        return true;
    }

    if(command.name=="WHO"){
        if(!command.arguments.empty()){
            queue_message(client_fd, "[error] usage: WHO\n");
            return true;
        }
        send_online_users(client_fd);
        return true;
    }


    //v3更新退出时，退出账号
    if (command.name == "QUIT")
    {
        if(!command.arguments.empty()){
           queue_message(client_fd, "[error] usage: QUIT\n");
            return true;
        }
        if(client_it->second.logged_in){
            logout_client(client_fd,true,true);
        }
        auto current_it=clients_.find(client_fd);
        if(current_it==clients_.end()){
            return  false;
        }
        current_it->second.close_after_write=true;
        queue_message(client_fd, "[system] goodbye.\n");
        return false;
    }

    queue_message(client_fd,"[error] unknown command. Type HELP to see available commands.\n");
    return true;
    
}

void ChatServer::handle_client_read(int client_fd)
{
    char buffer[kReadBufferSize];

    while (true)
    {
        const ssize_t received =
            recv(client_fd, buffer, sizeof(buffer), 0);

        if (received > 0)
        {
            auto it = clients_.find(client_fd);
            if (it == clients_.end())
            {
                return;
            }

            ClientSession& client = it->second;
            client.in_buffer.append(
                buffer,
                static_cast<std::size_t>(received));

            if (client.in_buffer.size() > kMaxInputBuffer &&
                client.in_buffer.find('\n') == std::string::npos)
            {
                client.close_after_write = true;
                queue_message(
                    client_fd,
                    "[error] input line is too long; connection will close.\n");
                return;
            }

            while (true)
            {
                const std::size_t newline = client.in_buffer.find('\n');
                if (newline == std::string::npos)
                {
                    break;
                }

                std::string line = client.in_buffer.substr(0, newline);
                client.in_buffer.erase(0, newline + 1);

                if (!handle_command(client_fd, line))
                {
                    return;
                }

                if (clients_.find(client_fd) == clients_.end())
                {
                    return;
                }
            }
        }
        else if (received == 0)
        {
            close_client(client_fd);
            return;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "recv failed, fd=" << client_fd
                      << ", error=" << std::strerror(errno) << '\n';

            close_client(client_fd);
            return;
        }
    }
}



void ChatServer::handle_client_write(int client_fd)
{

    auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end()) {
        return;
    }

    ClientSession& client = client_it->second;
    while (!client.out_buffer.empty())
    {
        const ssize_t sent = send(
            client_fd,
            client.out_buffer.data(),
            client.out_buffer.size(),
            MSG_NOSIGNAL);

        if (sent > 0) {
            client.out_buffer.erase(
                0,
                static_cast<std::size_t>(sent)
            );
            continue;
        }

        if (sent == -1 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        if (sent == -1 && errno == EINTR) {
            continue;
        }

        std::cerr << "send failed, fd=" << client_fd
                  << ", error=" << std::strerror(errno) << '\n';
        close_client(client_fd);
        return;
    }

    if (client.out_buffer.empty() && client.close_after_write)
    {
        close_client(client_fd);
        return;
    }

    update_epoll_events(client_fd);
}

void ChatServer::accept_new_clients()
{
    while (true)
    {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);

        const int client_fd = accept(
            listen_fd_,
            reinterpret_cast<sockaddr *>(&client_address),
            &client_length);

        if (client_fd == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "accept failed: "
                      << std::strerror(errno) << '\n';
            return;
        }

        if (set_non_blocking(client_fd) == -1)
        {
            std::cerr << "set_non_blocking client failed: "
                      << std::strerror(errno) << '\n';
            close(client_fd);
            continue;
        }

        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLRDHUP;

        if (epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_ADD,
                client_fd,
                &event) == -1)
        {
            std::cerr << "epoll_ctl ADD client failed: "
                      << std::strerror(errno) << '\n';
            close(client_fd);
            continue;
        }

        clients_.emplace(
            client_fd,
            ClientSession{client_fd, false, "", false, false, "", ""}
        );

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(
            AF_INET,
            &client_address.sin_addr,
            ip,
            sizeof(ip));

        const std::string guest_name =
            "guest-" + std::to_string(client_fd);

        std::cout << "new TCP client, fd=" << client_fd
                  << ", ip=" << ip
                  << ", port=" << ntohs(client_address.sin_port)
                  << '\n';

        queue_message(
            client_fd,
            "[system] connected to chatroom_v3.\n"
            "[system] REGISTER an account, then LOGIN to chat.\n"
        );
        send_help(client_fd);
    }
}
//实现：1.先找到客户端存在与否2.把用户名字保存下来然后在在线人员表里面把他删除
    //3.epoll里面移除，关闭socket连接
void ChatServer::close_client(int client_fd)
{
    const auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end()) {
        return;
    }

    std::string disconnected_username;

    if (client_it->second.logged_in) {
        //保存下来，然后删除
        disconnected_username = client_it->second.username;
        online_users_.erase(disconnected_username);
    }

    //epoll里面删除
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
    clients_.erase(client_it);

    std::cout << "TCP client disconnected, fd=" << client_fd;

    if (!disconnected_username.empty()) {
        std::cout << ", username=" << disconnected_username;
    }

    std::cout << '\n';

    if (!disconnected_username.empty()) {
        broadcast_to_logged_in(
            client_fd,
            "[system] " + disconnected_username + " is offline.\n",
            false
        );
    }
}

int ChatServer::run(){

    epoll_event events[kMaxEvents];

    while (true)
    {
        const int ready =
            epoll_wait(epoll_fd_, events, kMaxEvents, -1);

        if (ready == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            std::cerr << "epoll_wait failed: "
                      << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < ready; ++i)
        {
            const int fd = events[i].data.fd;
            const uint32_t event_mask = events[i].events;

            if (fd == listen_fd_)
            {
                accept_new_clients();
                continue;
            }

            if (event_mask & (EPOLLERR | EPOLLHUP ))
            {
                close_client(fd);
                continue;
            }

            if (event_mask & EPOLLIN)
            {
                handle_client_read(fd);
            }

            if (clients_.find(fd) != clients_.end() &&
                (event_mask & EPOLLOUT))
            {
                handle_client_write(fd);
            }

            if (clients_.find(fd) != clients_.end() &&
                (event_mask & EPOLLRDHUP)) {
                close_client(fd);
            }
        }
    }
    return 0;
}

void ChatServer::close_all(){
    for(const auto &[fd,client]:clients_){
        (void)client;
        close(fd);
    }
    clients_.clear();
    online_users_.clear();
    if(listen_fd_!=-1){
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}
}


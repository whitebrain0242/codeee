#include "chat_server.hpp"

#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

namespace chat
{

    ChatServer::ChatServer(int port) : port_(port) {}; // 没看懂e
    ChatServer::~ChatServer()
    {
        for (const auto& [client_fd, session] : clients_) {
        (void)session;
        ::close(client_fd);
       }

        clients_.clear();

       if (listen_fd_ != -1) {
        ::close(listen_fd_);
       }

    if (epoll_fd_ != -1) {
        ::close(epoll_fd_);
    }
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
        epoll_event listen_event{};
        listen_event.data.fd = listen_fd_;
        listen_event.events = EPOLLIN;

        if(epoll_ctl(epoll_fd_,EPOLL_CTL_ADD,listen_fd_,&listen_event)==-1){
            std::cerr
            << "epoll_ctl add listen socket failed: "
            << std::strerror(errno)
            << '\n';

            return false;
        }
        running_=true;
        std::cout << "chat_server v5 started on port " << port_ << '\n';
        return true;
    }

    int ChatServer::run(){
    if (!running_) {
        std::cerr << "server is not initialized\n";
        return 1;
    }

    epoll_event events[kMaxEvents];

    while (running_) {
        const int ready_count =
            epoll_wait(
                epoll_fd_,
                events,
                kMaxEvents,
                -1
            );

        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr
                << "epoll_wait failed: "
                << std::strerror(errno)
                << '\n';

            return 1;
        }

        for (int index = 0; index < ready_count; ++index) {
            const int fd = events[index].data.fd;
            const std::uint32_t event_flags =
                events[index].events;

            if (fd == listen_fd_) {
                accept_new_clients();
                continue;
            }

            if (clients_.find(fd) == clients_.end()) {
                continue;
            }

            if (
                event_flags &
                (EPOLLERR | EPOLLHUP | EPOLLRDHUP)
            ) {
                close_client(fd);
                continue;
            }

            if (event_flags & EPOLLIN) {
                handle_client_read(fd);
            }

            if (
                clients_.find(fd) != clients_.end() &&
                (event_flags & EPOLLOUT)
            ) {
                handle_client_write(fd);
            }
        }
    }

    return 0;
}



    bool ChatServer::create_listen_socket()
    {
        listen_fd_= socket(AF_INET, SOCK_STREAM| SOCK_NONBLOCK, 0);
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

        return true;
    }

    bool ChatServer::set_non_blocking(int fd)
    {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            return false;
        }
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK)!=-1;
    }
//实现：accept+报错处理+加入epoll+加入clients
    void ChatServer::accept_new_clients()
{
    while (true)
    {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);

        const int client_fd = accept4(
            listen_fd_,
            reinterpret_cast<sockaddr *>(&client_address),
            &client_length, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client_fd == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }

            if (errno == ENOSYS) {
                const int fallback_fd =
                    accept(
                        listen_fd_,
                        reinterpret_cast<sockaddr*>(&client_address),
                        &client_length
                    );

                if (fallback_fd == -1) {
                    if (
                        errno == EAGAIN ||
                        errno == EWOULDBLOCK
                    ) {
                        break;
                    }

                    std::cerr
                        << "accept failed: "
                        << std::strerror(errno)
                        << '\n';

                    break;
                }

                if (!set_non_blocking(fallback_fd)) {
                    std::cerr
                        << "set_non_blocking failed for client\n";

                    ::close(fallback_fd);
                    continue;
                }

                epoll_event event{};
                event.data.fd = fallback_fd;
                event.events = EPOLLIN | EPOLLRDHUP;

                if (
                    epoll_ctl(
                        epoll_fd_,
                        EPOLL_CTL_ADD,
                        fallback_fd,
                        &event
                    ) == -1
                ) {
                    std::cerr
                        << "epoll_ctl add client failed: "
                        << std::strerror(errno)
                        << '\n';

                    ::close(fallback_fd);
                    continue;
                }

                ClientSession fallback_session;
                fallback_session.fd = fallback_fd;

                clients_.emplace(
                    fallback_fd,
                    std::move(fallback_session)
                );

                queue_message(
                    fallback_fd,
                    "[system] connected to chatroom v5.\n"
                    "[system] type HELP to see commands.\n"
                );

                continue;
            }

            std::cerr << "accept failed: "
                      << std::strerror(errno) << '\n';
            break;
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
            ::close(client_fd);
            continue;
        }

        ClientSession session;
        session.fd=client_fd;
        clients_.emplace(
            client_fd,
            std::move(session)
        );

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(
            AF_INET,
            &client_address.sin_addr,
            ip,
            sizeof(ip));


        std::cout << "new client connected, fd=" << client_fd
                  << ", ip=" << ip
                  << ", port=" << ntohs(client_address.sin_port)
                  << '\n';

        queue_message(
            client_fd,
            "[system] connected to chatroom_v5.\n"
            "[system] type HELP to see commands.\n"
        );
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

    const bool was_logged_in = client_it->second.logged_in;
    const std::string username=client_it->second.username;

    if (was_logged_in) {
        const auto online_it=online_users_.find(username);
        if(online_it!=online_users_.end()&&online_it->second==client_fd){
            online_users_.erase(online_it);
        }
    }
    epoll_ctl(epoll_fd_,EPOLL_CTL_DEL,client_fd,nullptr);
    ::close(client_fd);
    clients_.erase(client_it);


    std::cout << "client disconnected, fd=" << client_fd;

     if (was_logged_in){
        std::cout<<", user="<<username;
    }
    std::cout<<'\n';

     if (was_logged_in) {
        broadcast_system_message(username+" is offline.");
    }
}


    void ChatServer::update_epoll_events(int client_fd)
    {
        const auto it = clients_.find(client_fd);
        if (it == clients_.end())
        {
            return;
        }

        const ClientSession& client =it->second;

        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLRDHUP;

        if (!client.close_after_write)
        {
            event.events |= EPOLLIN;
        }

        if (!client.out_buffer.empty())
        {
            event.events |= EPOLLOUT;
        }

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event) == -1)
        {
            std::cerr << "epoll_ctl MOD failed, fd=" << client_fd
                      << ", error=" << std::strerror(errno) << '\n';
            close_client(client_fd);
        }
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

            if (client.in_buffer.size() > kMaxInputBuffer)
            {
                client.close_after_write = true;
                client.in_buffer.clear();
                update_epoll_events(client_fd);
                queue_message(
                    client_fd,
                    "[error] input line is too long; connection will close.\n");
                return;
            }

            while (true)
            {
                it=clients_.find(client_fd);
                if(it==clients_.end()){
                    return;
                }

                const std::size_t newline = it->second.in_buffer.find('\n');
                if (newline == std::string::npos)
                {
                    break;
                }

                std::string line = it->second.in_buffer.substr(0, newline);
               it->second.in_buffer.erase(0, newline + 1);
                if(!line.empty()&&line.back()=='\r')line.pop_back();

                const bool should_continue=handle_command(client_fd,line);
                if(!should_continue)return;
            }
            continue;
        }
        
        if (received == 0)
        {
            close_client(client_fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
                break;
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

    if (client_it->second.out_buffer.empty() && client_it->second.close_after_write)
    {
        close_client(client_fd);
        return;
    }

    update_epoll_events(client_fd);
}


    void ChatServer::queue_message(int client_fd, const std::string& message)
    {
        const auto it = clients_.find(client_fd);
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
        std::vector<int> recipients;

        for (const auto &[client_fd, client] : clients_)
        {
            if (!client.logged_in)
                continue;
            if (!include_sender && client_fd == sender_fd)
                continue;

            recipients.push_back(client_fd);
        }
        for(const int recipient_fd:recipients){
            queue_message(recipient_fd,message);
        }
    }
    //v5新增函数：广播系统消息
    void ChatServer::broadcast_system_message(const std::string& message,int except_fd){
        std::vector<int> recipients;
        
        for(const auto& [client_fd,client]:clients_){
            if(!client.logged_in)continue;
            if(client_fd==except_fd)continue;
            
            recipients.push_back(client_fd);
        } 
        for(const int recipient_fd:recipients){
            queue_message(recipient_fd,"[system] "+message+"\n");
        }
    }

    bool ChatServer::handle_command(int client_fd,const std::string &raw_line)
{
    
    const Command command = parse_command(raw_line);

    if (command.name.empty())
    {
        return true;
    }

    if (command.name == "HELP")
    {
        send_help(client_fd);
        return true;
    }

    //v5：提取register函数
    if (command.name == "REGISTER"){
        handle_register(client_fd,command.raw_arguments);
        return true;
    }
   
    if (command.name == "LOGIN"){
         handle_login(client_fd,command.raw_arguments);
        return true;

    }
    //v3新增函数,账号退出
    if (command.name == "LOGOUT"){
        handle_logout(client_fd);
        return true;
    }

    //v3新增登录验证
    if (command.name == "SAY")
    {
        handle_public_message(client_fd,command.raw_arguments);
        return true;
    }

    //v4新增函数：处理私聊功能
    if(command.name=="MSG"){
        handle_private_message(client_fd,command.raw_arguments);
        return true;
        
    }

    //v5新增好友添加，接受，拒绝，删除，显示好友，查看好友请求
    if (command.name == "ADD_FRIEND") {
        handle_add_friend(
            client_fd,
            command.raw_arguments
        );
        return true;
    }

    if (command.name == "ACCEPT_FRIEND") {
        handle_accept_friend(
            client_fd,
            command.raw_arguments
        );
        return true;
    }

    if (command.name == "REJECT_FRIEND") {
        handle_reject_friend(
            client_fd,
            command.raw_arguments
        );
        return true;
    }

    if (command.name == "REMOVE_FRIEND") {
        handle_remove_friend(
            client_fd,
            command.raw_arguments
        );
        return true;
    }

    if (command.name == "FRIENDS") {
        send_friend_list(client_fd);
        return true;
    }

    if (command.name == "FRIEND_REQUESTS") {
        send_friend_requests(client_fd);
        return true;
    }


    if(command.name=="WHO"){
        send_online_users(client_fd);
        return true;
    }


    //v3更新退出时，退出账号
    if (command.name == "QUIT")
    {
        auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()){
            return false;
        }
        if(client_it->second.logged_in){
            logout_session(client_fd,false,true);
        }
        client_it=clients_.find(client_fd);

        if(client_it==clients_.end()){
            return false;
        }
        client_it->second.close_after_write=true;
        queue_message(client_fd, "[system] goodbye.\n");

        update_epoll_events(client_fd);
        return true;
    }

    queue_message(client_fd,"[error] unknown command. Type HELP to see available commands.\n");
    return true;
    
}

    void ChatServer::send_help(int client_fd)
    {
        queue_message(
            client_fd,
        "[system] commands:\n"
        "  HELP\n"
        "      show this help\n"
        "  REGISTER <username> <password>\n"
        "      create an in-memory account\n"
        "  LOGIN <username> <password>\n"
        "      log in to an account\n"
        "  LOGOUT\n"
        "      log out without closing TCP connection\n"
        "  SAY <message>\n"
        "      send a public message\n"
        "  MSG <username> <message>\n"
        "      send a private message to a friend\n"
        "  ADD_FRIEND <username>\n"
        "      send a friend request\n"
        "  ACCEPT_FRIEND <username>\n"
        "      accept an incoming friend request\n"
        "  REJECT_FRIEND <username>\n"
        "      reject an incoming friend request\n"
        "  REMOVE_FRIEND <username>\n"
        "      remove an existing friend\n"
        "  FRIENDS\n"
        "      list friends and online state\n"
        "  FRIEND_REQUESTS\n"
        "      list incoming and outgoing requests\n"
        "  WHO\n"
        "      list logged-in users\n"
        "  QUIT\n"
        "      close the connection gracefully\n");
    }

    // v3：把每一个命令拆出来
    // 实现：先找是哪一个客户端-》进而判断是否登陆了，创建一个数组接着预留空间，增加元素，排序，拼接恢复，输出
    void ChatServer::send_online_users(int client_fd)
    {

        const auto client_it = clients_.find(client_fd);

        if (client_it == clients_.end()||!client_it->second.logged_in)
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

        std::ostringstream output;
        output<<"[system] online users (" + std::to_string(usernames.size()) + "): ";

        if(usernames.empty()){
            output<<"(none)";
        }else{
         for (std::size_t i = 0; i < usernames.size(); ++i)
         {
            if (i != 0)
            {
                output<< ", ";
            }
             output<< usernames[i];
         }
    }
        output<< "\n";

        queue_message(client_fd, output.str());
    }

    
    //v3新增注册函数
    //实现：1.先取客户端信息2.检查是否一登陆3.检查参数是不是2个4.检查名字和密码是否可以使用5.检查名字是否已经被使用
    //     最后增加名字和密码，发送信息
    void ChatServer::handle_register(int client_fd,const std::string& raw_arguments){
        auto client_it = clients_.find(client_fd);

        if (client_it == clients_.end()) {
            return;
        }
        if(client_it->second.logged_in){
            queue_message(client_fd,"[error] LOGOUT before registering another account.\n");
            return;
        }
        const std::vector<std::string> arguments =split_words(raw_arguments);

        if(arguments.size()!=2){
            queue_message(client_fd,"[error] usage: REGISTER <username> <password>\n");
            return;
        }

        const std::string& username = arguments[0];
        const std::string& password = arguments[1];

        if(!is_valid_username(username)){
            queue_message(
                client_fd,
                "[error] username must be 3-20 characters and contain "
                "only letters, digits, or underscore.\n"
            );
            return;
        }
        if(!is_valid_password(password)){
            queue_message(
                client_fd,
                "[error] password must be 4-64 non-space characters.\n"
            );
            return;
        }
        if(account_exists(username)){
            queue_message(client_fd,"[error] username already exists.\n");
            return;
        }
        UserAccount account;
        account.username=username;
        account.password=password;

        users_.emplace(username,std::move(account));
        queue_message(
            client_fd,
            "[system] registration successful. "
            "Use LOGIN <username> <password> to log in.\n"
        );
    }
    //v3新增登陆函数
    //实现：1.取客户端2.检查是否登陆3.检查参数4.身份验证
    void ChatServer::handle_login(int client_fd,const std::string& raw_arguments){
        auto client_it = clients_.find(client_fd);

        if (client_it == clients_.end()) {
            return;
        }
        if(client_it->second.logged_in){
            queue_message(client_fd,"[error] this connection is already logged in.\n");
            return;
        }
        const std::vector<std::string> arguments =split_words(raw_arguments);

        if(arguments.size()!=2){
            queue_message(client_fd,"[error] usage: LOGIN <username> <password>\n");
            return;
        }

        const std::string& username = arguments[0];
        const std::string& password = arguments[1];

        const auto account_it = users_.find(username);

        if(account_it==users_.end()||account_it->second.password!=password){
            queue_message(client_fd, "[error] invalid username or password.\n");
            return;
        }
        if(online_users_.find(username)!=online_users_.end()){
            queue_message(client_fd,"[error] this account is already logged in.\n");
            return;
        }
        client_it->second.logged_in=true;
        client_it->second.username=username;
        online_users_[username]=client_fd;
        queue_message(client_fd,"[system] login successful. Welcome, " +username + ".\n");
        broadcast_system_message(username+" is online.",client_fd);
        const auto& account=account_it->second;
        if(!account.incoming_friend_requests.empty()){
            queue_message(client_fd,"[system] you have " +std::to_string(account.incoming_friend_requests.size())+" pending friend request(s). ""Use FRIEND_REQUESTS to view them.\n");
        }
    }


    void ChatServer::handle_logout(int client_fd){
        const auto client_it = clients_.find(client_fd);

        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd, "[error] you are not logged in.\n");
            return;
        }
        logout_session(client_fd, true, true);
    }

    void ChatServer::handle_public_message(int client_fd,const std::string& message){
        const auto client_it = clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd, "[error] you must LOGIN before chatting.\n");
            return;
        }

        const std::string cleaned=trim(message);

        if (cleaned.empty()) {
            queue_message(client_fd, "[error] usage: SAY <message>\n");
            return ;
        }

        if (cleaned.size() > kMaxChatMessage) {
            queue_message(
                client_fd,
                "[error] message is too long; maximum is 1000 bytes.\n"
            );
            return ;
        }

        const std::string chat_message =
            "[" + client_it->second.username + "] " +
            cleaned + "\n";

        broadcast_to_logged_in(client_fd, chat_message, true);

    }

    void ChatServer::handle_private_message(int client_fd,const std::string& raw_arguments){
        //验证登陆状态
        const auto sender_it=clients_.find(client_fd);
        if(sender_it==clients_.end()||!sender_it->second.logged_in){
            queue_message(client_fd, "[error] you must LOGIN before sending.\n");
            return;
        }
        //解析命令参数
        std::string target_username;
        std::string message;
        if(!split_first_token(raw_arguments,target_username,message)||message.empty()){
            queue_message(client_fd,"[error] usage :MSG<username><message>\n");
            return;
        }
        //验证目标用户名合法性
        if(!is_valid_username(target_username)){
            queue_message(client_fd,"[error] invalid target username.\n");
            return;
        }
        //检查消息长度上限
        if(message.size()>kMaxChatMessage){
            queue_message(client_fd,"[error] private message is too long; maximum is 1000 bytes.\n");
            return;
        }

        //禁止给自己发消息
        const std::string sender_username=sender_it->second.username;
        if(target_username==sender_username){
            queue_message(
            client_fd,
            "[error] you cannot send a private message to yourself.\n");
            return;
        }

        //检查目标用户是否存在
        if(!account_exists(target_username)){
            queue_message(client_fd,"[error] user " +
            target_username +
            " does not exist.\n");
            return;
        }

        //检查好友关系
        if(!are_friends(sender_username,target_username)){
            queue_message(client_fd,"[error] private messaging is allowed only between friends.\n");
            return;
        }
        //在线
        const auto online_it=online_users_.find(target_username);
        if(online_it==online_users_.end()){
            queue_message(client_fd,"[error] friend "+target_username+" is offline.\n");
            return;
        }
        //会话有效：
        const int target_fd=online_it->second;
        const auto target_it=clients_.find(target_fd);

        if(target_it==clients_.end()||!target_it->second.logged_in||target_it->second.username!=target_username){
            queue_message(client_fd,"[error] target session is unavailable.\n");
            return;
        }
        //发送信息
        queue_message(target_fd,"[private from "+sender_username+"]"+message+"\n");
        queue_message(client_fd,"[private to "+target_username+"]"+message+"\n");

    }

    //v5新增添加好友
    void ChatServer::handle_add_friend(int client_fd,const std::string& raw_arguments){
        const auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd,"[error] you must LOGIN before adding friends .\n");
            return;
        }

        std::string target_username;
        if(!extract_single_username(client_fd,raw_arguments,"ADD_FRIEND <username>",target_username)){
            return;
        }
        const std::string sender_username=client_it->second.username;
        if(target_username==sender_username){
            queue_message(client_fd,"[error] you cannot add yourself as a friend .\n");
            return;
        }
        auto sender_account_it=users_.find(sender_username);
        auto target_account_it=users_.find(target_username);
        if(sender_account_it==users_.end()||target_account_it==users_.end()){
            queue_message(client_fd,"[error] "+target_username+"does not exist .\n");
            return;
        }
        UserAccount& sender_account=sender_account_it->second;
        UserAccount& target_account=target_account_it->second;
        if(sender_account.friends.find(target_username)!=sender_account.friends.end()){
            queue_message(client_fd,"[error] "+target_username+" is already your friend. \n");
            return;
        }
        if(sender_account.outgoing_friend_requests.find(target_username)!=sender_account.outgoing_friend_requests.end()){
            queue_message(client_fd,"[error] friend request already sent to " +target_username +".\n");
            return;
        }
        if(sender_account.incoming_friend_requests.find(target_username)!=sender_account.incoming_friend_requests.end()){
            queue_message(client_fd,"[error] " +target_username +" already sent you a request. ""Use ACCEPT_FRIEND " +target_username +".\n");
            return;
        }
        sender_account.outgoing_friend_requests.insert(target_username);
        target_account.incoming_friend_requests.insert(sender_username);
        queue_message(client_fd,"[system] friend request sent to " +target_username +".\n");

        notify_user_if_online(target_username,"[system] friend request from " +sender_username +". Use ACCEPT_FRIEND " +sender_username +" or REJECT_FRIEND " +sender_username +".\n");
    }
    //v5新增接受好友申请
    void ChatServer::handle_accept_friend(int client_fd,const std::string& raw_arguments){
        const auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd,"[error] you must LOGIN before accepting friends .\n");
            return;
        }
        std::string requester_username;
        if(!extract_single_username(client_fd,raw_arguments,"ACCEPT_FRIEND <username>",requester_username)){
            return;
        }
        const std::string current_username=client_it->second.username;
        
        auto current_account_it=users_.find(current_username);
        auto requester_account_it=users_.find(requester_username);
        if(current_account_it==users_.end()||requester_account_it==users_.end()){
            queue_message(client_fd,"[error] requester account no longer exists.\n");
            return;
        }
        UserAccount& current_account=current_account_it->second;
        UserAccount& requester_account=requester_account_it->second;

        if(current_account.incoming_friend_requests.find(requester_username)==current_account.incoming_friend_requests.end()){
            queue_message(client_fd,"[error] no pending friend request from " +requester_username +".\n");
            return;
        }
        current_account.incoming_friend_requests.erase(requester_username);
        requester_account.outgoing_friend_requests.erase(current_username);

        current_account.friends.insert(requester_username);
        requester_account.friends.insert(current_username);

        queue_message(client_fd,"[system] you and " +requester_username +" are now friends.\n");
        notify_user_if_online(requester_username,"[system] " +current_username +" accepted your friend request.\n");

    }
    
    //v5:拒绝好友请求
    void ChatServer::handle_reject_friend(int client_fd,const std::string& raw_arguments){
        const auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd,"[error] you must LOGIN before rejecting friends .\n");
            return;
        }
        std::string requester_username;
        if(!extract_single_username(client_fd,raw_arguments,"REJECT_FRIEND <username>",requester_username)){
            return;
        }
        const std::string current_username=client_it->second.username;
        
        auto current_account_it=users_.find(current_username);
        auto requester_account_it=users_.find(requester_username);
        if(current_account_it==users_.end()||requester_account_it==users_.end()){
            queue_message(client_fd,"[error] requester account no longer exists.\n");
            return;
        }
        UserAccount& current_account=current_account_it->second;
        UserAccount& requester_account=requester_account_it->second;

        if(current_account.incoming_friend_requests.find(requester_username)==current_account.incoming_friend_requests.end()){
            queue_message(client_fd,"[error] no pending friend request from " +requester_username +".\n");
            return;
        }
        current_account.incoming_friend_requests.erase(requester_username);
        requester_account.outgoing_friend_requests.erase(current_username);

        queue_message(client_fd,"[system] friend request from " +requester_username +" rejected.\n");
        notify_user_if_online(requester_username,"[system] " +current_username +" rejected your friend request.\n");
    }
    //v5：新增删除好友
    void ChatServer::handle_remove_friend(int client_fd,const std::string& raw_arguments){
        const auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd,"[error] you must LOGIN before removing friends .\n");
            return;
        }
        std::string friend_username;
        if(!extract_single_username(client_fd,raw_arguments,"REMOVE_FRIEND <username>",friend_username)){
            return;
        }
        const std::string current_username=client_it->second.username;
        
        auto current_account_it=users_.find(current_username);
        auto friend_account_it=users_.find(friend_username);

        if(current_account_it==users_.end()||friend_account_it==users_.end()){
            queue_message(client_fd,"[error] user "+friend_username+" does not exist.\n");
            return;
        }
        UserAccount& current_account=current_account_it->second;
        UserAccount& friend_account=friend_account_it->second;

        if(current_account.friends.find(friend_username) ==current_account.friends.end()){
            queue_message(client_fd,"[error] " +friend_username +" is not your friend.\n");
            return;
        }

        current_account.friends.erase(friend_username);
        friend_account.friends.erase(current_username);

        queue_message(client_fd,"[system] removed " +friend_username +" from your friend list.\n");
        notify_user_if_online(friend_username,"[system] " +current_username +" removed you from their friend list.\n");

    }

    //v5:新增获取好友列表，并且标注是否在线
    void ChatServer::send_friend_list(int client_fd){
        const auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd,"[error] you must LOGIN before viewing friends .\n");
            return;
        }
        
        auto account_it=users_.find(client_it->second.username);

        if(account_it==users_.end()){
            queue_message(client_fd, "[error] current account is unavailable.\n");
            return;
        }
        //不一样的地方
        std::vector<std::string> friend_names(account_it->second.friends.begin(),account_it->second.friends.end());
        std::sort(friend_names.begin(),friend_names.end());
        std::ostringstream output;
        output<< "[system] friends ("<< friend_names.size()<< "):\n";
        if(friend_names.empty()){
            output << "  (none)\n";
        }else{
            for(const std::string& friend_name:friend_names){
                const bool online =online_users_.find(friend_name)!=online_users_.end();
                output<< "  "<< friend_name << " ["<< (online ? "online" : "offline") << "]\n";
                
            }
        }
        queue_message(client_fd,output.str());
    }
    //v5显示好友申请列表：发送和接受
    void ChatServer::send_friend_requests(int client_fd){
        const auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            queue_message(client_fd,"[error] you must LOGIN before viewing friend requeses .\n");
            return;
        }
        
        auto account_it=users_.find(client_it->second.username);

        if(account_it==users_.end()){
            queue_message(client_fd,"[error] current account is unavailable.\n");
            return;
        }
        UserAccount& account=account_it->second;
        
        queue_message(client_fd,"[system] incoming requests (" +std::to_string(account.incoming_friend_requests.size()) +"): " +join_sorted(account.incoming_friend_requests ) +"\n""[system] outgoing requests (" +std::to_string(account.outgoing_friend_requests.size()) +"): " + join_sorted(account.outgoing_friend_requests) +"\n");
    }

    //v5新增登出操作
    bool ChatServer::logout_session(int client_fd,bool notify_client,bool notify_others){
        auto client_it=clients_.find(client_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in){
            return false;
        }
        const std::string username=client_it->second.username;
        const auto online_it=online_users_.find(username);
        if(online_it!=online_users_.end()&&online_it->second==client_fd){
            online_users_.erase(online_it);
        }
        client_it->second.logged_in=false;
        client_it->second.username.clear();
        if(notify_client){
            queue_message(client_fd,"[system] logout successful.\n");
        }
        if(notify_others){
            broadcast_system_message(username+" is offline.",client_fd);
        }
        return true;
    }

    // v2新增解析命令函数
    // 实现：首先判断长度是1到20之间，其次只能游数字字母下划线构成，其他组成不通过
    // 目的：判断昵称是否合规

    bool ChatServer::is_valid_username(const std::string &username)
    {
        if (username.size() < 3|| username.size() > 20)
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
        if (password.size() < 4 || password.size() > 64)
        {
            return false;
        }
        for (char ch : password)
        {
            const unsigned char value = static_cast<unsigned char>(ch);
            if (std::isspace(value) ||!std::isprint(value))
                return false;
        }
        return true;
    }

    //v5:检查用户名是否已经存在
    bool ChatServer::account_exists(const std::string& username)const{
        return users_.find(username)!=users_.end();
    }
    
    //v5:双方是不是好友
    bool ChatServer::are_friends(const std::string& left,const std::string& right)const{
        const auto left_it=users_.find(left);
        const auto right_it=users_.find(right);
        if(left_it==users_.end()||right_it==users_.end())return false;
        bool left_has_right=left_it->second.friends.find(right)!=left_it->second.friends.end();
        bool right_has_left=right_it->second.friends.find(left)!=right_it->second.friends.end();
        return left_has_right&&right_has_left;

    }
    //v5:帮助好友功能分离一个用户名
    bool ChatServer::extract_single_username(int client_fd,const std::string& raw_arguments,const std::string& usage,std::string& username){
        const std::vector<std::string> arguments=split_words(raw_arguments);
        if(arguments.size()!=1){
            queue_message(client_fd,"[error] usage: "+usage+"\n");
            return false;
        }
        username=arguments[0];
        if(!is_valid_username(username)){
            queue_message(client_fd,"[error] invalid username.\n");
            return false;
        }
        return true;
    }

    //v5：目标是否在线
    void ChatServer::notify_user_if_online(const std::string& username,const std::string& message){
        const auto online_it=online_users_.find(username);
        if(online_it==online_users_.end())return;
        const int target_fd=online_it->second;
        const auto client_it=clients_.find(target_fd);
        if(client_it==clients_.end()||!client_it->second.logged_in||username!=client_it->second.username){
            online_users_.erase(online_it);
            return;
        }
        queue_message(target_fd,message);
    }
    //v5：把用户名列表排序，并且转字符串后返回
    std::string ChatServer::join_sorted(const std::unordered_set<std::string>& values){
        if(values.empty())return "(none)";

        std::vector<std::string> sorted(values.begin(),values.end());
        std::sort(sorted.begin(),sorted.end());

        std::ostringstream output;
        for(std::size_t index=0;index<sorted.size();++index){
            if(index!=0)output<<", ";
            output<<sorted[index];
        }
        return output.str();
    }
    

    
}


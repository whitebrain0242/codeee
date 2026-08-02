#include "chat_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

namespace chat
{
    ChatServer::ChatServer(int port,IUserRepository& user_repository)
        : port_(port),user_repository_(user_repository),message_store_(1000,500) {} // 没看懂e
    ChatServer::~ChatServer()
    {
        for (const auto& [client_fd, session] : clients_) {
        (void)session;
        close(client_fd);
       }
       if (listen_fd_ != -1) {
        close(listen_fd_);
       }

    if (epoll_fd_ != -1) {
        close(epoll_fd_);
    }
    }
   
     // v3新增函数：服务端启动前的检验
    bool ChatServer::initialize()
    {
        return load_registered_users()&&
        create_listen_socket()&&
        create_epoll()&&
        add_listen_socket_to_epoll();
    }

    //把数据库里面村的已经注册的用户名列表，同步到服务的内存缓存（哈希表中
    bool ChatServer::load_registered_users(){
        //准备数据容器，查询数据库，清除旧的缓存，逐条写入
        std::vector<std::string> usernames;
        std::string error;

        if(!user_repository_.load_usernames(usernames,error)){
            std::cerr
            << "failed to load registered users: "
            << error
            << std::endl;
        return false;
        }
        users_.clear();

        for(const std::string& username:usernames){
            users_.emplace(
                username,UserAccount{username,{},{},{}}
            );
        }
        std::cout
        << "loaded "
        << users_.size()
        << " registered account(s) from user repository"
        << std::endl;

        return true;
    }

    int ChatServer::run(){
    if (listen_fd_ == -1 || epoll_fd_ == -1) {
        std::cerr
            << "server is not initialized"
            << std::endl;
        return 1;
    }

    std::cout
        << "chat_server v6 started on port "
        << port_
        << std::endl;

    epoll_event events[kMaxEvents];

    while (true) {
        const int event_count =
            epoll_wait(
                epoll_fd_,
                events,
                kMaxEvents,
                -1
            );

        if (event_count == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr
                << "epoll_wait failed: "
                << std::strerror(errno)
                << std::endl;
            return 1;
        }

        for (int index = 0;
             index < event_count;
             ++index) {
            const int fd = events[index].data.fd;
            const std::uint32_t flags =
                events[index].events;

            if (fd == listen_fd_) {
                accept_new_clients();
                continue;
            }

            if (clients_.find(fd) == clients_.end()) {
                continue;
            }

            if ((flags & EPOLLIN) != 0U) {
                handle_client_read(fd);
            }

            if (
                clients_.find(fd) != clients_.end() &&
                (flags & EPOLLOUT) != 0U
            ) {
                handle_client_write(fd);
            }

            if (
                clients_.find(fd) != clients_.end() &&
                (flags & (
                    EPOLLERR |
                    EPOLLHUP |
                    EPOLLRDHUP
                )) != 0U
            ) {
                close_client(fd);
            }
        }
    }
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
        addr.sin_port =  htons(static_cast<std::uint16_t>(port_));

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
        if(!set_non_blocking(listen_fd_)){
            std::cerr
            << "failed to set listen socket "
            << "non-blocking"
            << std::endl;
        return false;
        }

        return true;
    }

// v3新增：创建一个epoll,并且把监听socket加进去
    bool ChatServer::create_epoll()
    {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1)
        {
            std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
            return false;
        }

        return true;
    }
    bool ChatServer::add_listen_socket_to_epoll(){
        epoll_event event{};
        event.data.fd = listen_fd_;
        event.events = EPOLLIN;
        if(epoll_ctl(epoll_fd_,EPOLL_CTL_ADD,listen_fd_,&event)==-1){
            std::cerr<< "epoll_ctl add listen socket failed: "
                     << std::strerror(errno)
                     << std::endl;
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
    while (true) {
        sockaddr_in client_address{};
        socklen_t client_length =
            sizeof(client_address);

        const int client_fd =
            accept(
                listen_fd_,
                reinterpret_cast<sockaddr*>(
                    &client_address
                ),
                &client_length
            );

        if (client_fd == -1) {
            if (
                errno == EAGAIN ||
                errno == EWOULDBLOCK
            ) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            std::cerr
                << "accept failed: "
                << std::strerror(errno)
                << std::endl;
            break;
        }

        if (!set_non_blocking(client_fd)) {
            std::cerr
                << "failed to set client socket "
                << "non-blocking"
                << std::endl;
            close(client_fd);
            continue;
        }

        epoll_event event{};
        event.data.fd = client_fd;
        event.events =
            EPOLLIN |
            EPOLLRDHUP;

        if (
            epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_ADD,
                client_fd,
                &event
            ) == -1
        ) {
            std::cerr
                << "epoll_ctl add client failed: "
                << std::strerror(errno)
                << std::endl;
            close(client_fd);
            continue;
        }

        clients_.emplace(
            client_fd,
            ClientSession{
                client_fd,
                false,
                "",
                false,
                "",
                ""
            }
        );

        char ip[INET_ADDRSTRLEN]{};

        inet_ntop(
            AF_INET,
            &client_address.sin_addr,
            ip,
            sizeof(ip)
        );

        std::cout
            << "new client connected, fd="
            << client_fd
            << ", ip="
            << ip
            << ", port="
            << ntohs(client_address.sin_port)
            << std::endl;

        queue_message(
            client_fd,
            "[system] welcome to chatroom v6.\n"
            "[system] type HELP to view commands.\n"
        );
    }
}

//实现：1.先找到客户端存在与否2.把用户名字保存下来然后在在线人员表里面把他删除
//3.epoll里面移除，关闭socket连接
void ChatServer::close_client(int client_fd,bool announce_offline)
{
    const auto it = clients_.find(client_fd);
    if (it == clients_.end()) {
        return;
    }
    std::string offline_username;

    if (it->second.logged_in) {
        offline_username=it->second.username;
        const auto online_it=online_users_.find(offline_username);
        if(online_it!=online_users_.end()&&online_it->second==client_fd){
            online_users_.erase(online_it);
        }
    }
    epoll_ctl(epoll_fd_,EPOLL_CTL_DEL,client_fd,nullptr);
    close(client_fd);
    clients_.erase(it);

    std::cout << "client disconnected, fd=" << client_fd<< std::endl;

     if (announce_offline&&!offline_username.empty()){
        broadcast_to_logged_in("[system] " + offline_username + " is offline.\n");
    }
    
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
            it->second.in_buffer.append(
                buffer,
                static_cast<std::size_t>(received));


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
                if(!handle_command(client_fd,line))return;

                const auto state_it =clients_.find(client_fd);
                if(state_it==clients_.end()||state_it->second.close_after_write)return;

            }

            it=clients_.find(client_fd);
            if(it==clients_.end())return;

            if(it->second.in_buffer.size()>kMaxInputBuffer){
                queue_message(client_fd,"[error] input line is too long; connection will close.\n");
                it->second.close_after_write=true;
                update_epoll_events(client_fd);
                return;
            }
            continue;
        }
        
        if (received == 0)
        {
            close_client(client_fd);
            return;
        }
        if (errno == EINTR) {
                continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
                break;
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

    while (!client_it->second.out_buffer.empty())
    {
        const ssize_t sent = send(
            client_fd,
            client_it->second.out_buffer.data(),
            client_it->second.out_buffer.size(),
            MSG_NOSIGNAL);

        if (sent > 0) {
            client_it->second.out_buffer.erase(
                0,
                static_cast<std::size_t>(sent)
            );
            continue;
        }

        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
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

        if (it == clients_.end()) {
        return;
        }

        it->second.out_buffer += message;
         update_epoll_events(client_fd);
    }
    // v3：新增群发广播，把一条消息发给当前所有登陆用户
    // 实现：筛选一登陆用户，是否回显，放入发送队列
    void ChatServer::broadcast_to_logged_in(const std::string &message, int excluded_fd)
    {
        std::vector<int> recipients;
        recipients.reserve(clients_.size());

        for (const auto &[fd, session] : clients_)
        {
            if (session.logged_in&& fd != excluded_fd)
               recipients.push_back(fd);
        }
        for(const int recipient_fd:recipients){
            queue_message(recipient_fd,message);
        }
    }


    bool ChatServer::handle_command(int client_fd,const std::string &raw_line)
{
    
    const Command command = parse_command(raw_line);

    if (command.name.empty()) {
        return true;
    }

    if (command.name == "HELP") {
        send_help(client_fd);
    } else if (command.name == "REGISTER") {
        handle_register(client_fd, command);
    } else if (command.name == "LOGIN") {
        handle_login(client_fd, command);
    } else if (command.name == "LOGOUT") {
        handle_logout(client_fd);
    } else if (command.name == "SAY") {
        handle_public_message(client_fd, command);
    } else if (command.name == "MSG") {
        handle_private_message(client_fd, command);
    } else if (command.name == "WHO") {
        handle_who(client_fd);
    } else if (command.name == "ADD_FRIEND") {
        handle_add_friend(client_fd, command);
    } else if (command.name == "ACCEPT_FRIEND") {
        handle_accept_friend(client_fd, command);
    } else if (command.name == "REJECT_FRIEND") {
        handle_reject_friend(client_fd, command);
    } else if (command.name == "REMOVE_FRIEND") {
        handle_remove_friend(client_fd, command);
    } else if (command.name == "FRIENDS") {
        send_friend_list(client_fd);
    } else if (command.name == "FRIEND_REQUESTS") {
        send_friend_requests(client_fd);
    } else if (command.name == "HISTORY_PUBLIC") {
        handle_public_history(client_fd, command);
    } else if (command.name == "HISTORY_PRIVATE") {
        handle_private_history(client_fd, command);
    } else if (command.name == "QUIT") {
        auto it = clients_.find(client_fd);
        if(it!=clients_.end()){
            it->second.close_after_write=true;
            queue_message(client_fd,"[system] goobye.\n");
        }   
    }else{
        queue_message(client_fd,"[error] unknown command.  Type HELP to view commands.\n");
    }
    
    return clients_.find(client_fd)!=clients_.end();
    
}

    void ChatServer::send_help(int client_fd)
    {
        queue_message(
        client_fd,
        "[system] commands:\n"
        "  HELP\n"
        "  REGISTER <username> <password>\n"
        "  LOGIN <username> <password>\n"
        "  LOGOUT\n"
        "  SAY <message>\n"
        "  MSG <username> <message>\n"
        "  WHO\n"
        "  ADD_FRIEND <username>\n"
        "  ACCEPT_FRIEND <username>\n"
        "  REJECT_FRIEND <username>\n"
        "  REMOVE_FRIEND <username>\n"
        "  FRIENDS\n"
        "  FRIEND_REQUESTS\n"
        "  HISTORY_PUBLIC [count]\n"
        "  HISTORY_PRIVATE <username> [count]\n"
        "  QUIT\n"
    );
    }

    
    //v3新增注册函数
    //实现：1.先取客户端信息2.检查是否一登陆3.检查参数是不是2个4.检查名字和密码是否可以使用5.检查名字是否已经被使用
    //     最后增加名字和密码，发送信息
    void ChatServer::handle_register(int client_fd,const Command& command){
        auto client_it = clients_.find(client_fd);

        if (client_it == clients_.end()) {
            return;
        }
        if(client_it->second.logged_in){
            queue_message(client_fd,"[error] LOGOUT before registering another account.\n");
            return;
        }
        const std::vector<std::string> arguments =split_words(command.raw_arguments);

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
        if(users_.find(username)!=users_.end()){
            queue_message(client_fd,"[error] username already exists.\n");
            return;
        }

        std::string repository_error;
        const CreateUserResult result=user_repository_.create_user(username,password,repository_error);

        if(result==CreateUserResult::AlreadyExists){
             queue_message(
            client_fd,
            "[error] username already exists.\n"
            );
        return;
        }
        if(result==CreateUserResult::Error){
            std::cerr
            << "REGISTER database error for "
            << username
            << ": "
            << repository_error
            << std::endl;

            queue_message(
            client_fd,
            "[error] database operation failed; "
            "try again later.\n"
            );
        return;
        }

        users_.emplace(username,UserAccount{username,{},{},{}});
        queue_message(
            client_fd,
            "[system] registration successful. "
            "Use LOGIN <username> <password> to log in.\n"
        );
    }
    //v3新增登陆函数
    //实现：1.取客户端2.检查是否登陆3.检查参数4.身份验证
    void ChatServer::handle_login(int client_fd,const Command& command){
        auto client_it = clients_.find(client_fd);

        if (client_it == clients_.end()) {
            return;
        }
        if(client_it->second.logged_in){
            queue_message(client_fd,"[error] this connection is already logged in.\n");
            return;
        }
        const std::vector<std::string> arguments =split_words(command.raw_arguments);

        if(arguments.size()!=2){
            queue_message(client_fd,"[error] usage: LOGIN <username> <password>\n");
            return;
        }

        const std::string& username = arguments[0];
        const std::string& password = arguments[1];

        std::string repository_error;
        const VerifyUserResult verification=
            user_repository_.verify_user(
            username,password,repository_error
        );

        if(verification==VerifyUserResult::InvalidCredentials){
            queue_message(
            client_fd,
            "[error] invalid username or password.\n"
            );
            return;
        }
        if(verification==VerifyUserResult::Error){
            std::cerr
            << "LOGIN database error for "
            << username
            << ": "
            << repository_error
            << std::endl;

           queue_message(
            client_fd,
            "[error] database operation failed; "
            "try again later.\n"
           );
           return;
        }
    
         auto account_it = users_.find(username);

        if(account_it==users_.end()){
            account_it=users_.emplace(username,UserAccount{username,{},{},{}}).first;
        }

        if(online_users_.find(username)!=online_users_.end()){
            queue_message(client_fd,"[error] this account is already logged in.\n");
            return;
        }
        client_it->second.logged_in=true;
        client_it->second.username=username;
        online_users_[username]=client_fd;
        queue_message(client_fd,"[system] login successful. Welcome, " +username + ".\n");
        //登陆之后，告诉有多少个好友请求
        const std::size_t pending_count=account_it->second.incoming_friend_requests.size();
        if(pending_count>0){
            queue_message(client_fd,"[system] you have " +
            std::to_string(pending_count) +
            " pending friend request(s). "
            "Use FRIEND_REQUESTS to view them.\n");
        }
        broadcast_to_logged_in("[system] "+username+" is online.\n",client_fd);
    
    }


    void ChatServer::handle_logout(int client_fd){
        const auto client_it = clients_.find(client_fd);

        if(client_it==clients_.end()){
            queue_message(client_fd, "[error] you are not logged in.\n");
            return;
        }
        const std::string username=client_it->second.username;
        online_users_.erase(username);
        client_it->second.logged_in=false;
        client_it->second.username.clear();
        queue_message(client_fd,"[system] logout successful.\n");
        broadcast_to_logged_in("[system] "+username+" is offline.\n");
        
    }

    void ChatServer::handle_public_message(int client_fd,const Command& command){
        if(!require_login(client_fd,"chatting"))return;

        const std::string message=trim(command.raw_arguments);

        if (message.empty()) {
            queue_message(client_fd, "[error] usage: SAY <message>\n");
            return ;
        }

        if (message.size() > kMaxChatMessage) {
            queue_message(
                client_fd,
                "[error] message is too long; maximum is 1000 bytes.\n"
            );
            return ;
        }
        const std::string sender=clients_.at(client_fd).username;
        // 直接调用，不赋值
        message_store_.add_public(sender, message);

        broadcast_to_logged_in( "[" +sender +"] " +message + "\n");
    }

    void ChatServer::handle_private_message(int client_fd,const Command& command){
        //验证登陆状态
        if(!require_login(client_fd,"sending private message"))return;

        //解析命令参数
        std::string target_username;
        std::string message;
        if(!split_first_token(command.raw_arguments,target_username,message)||message.empty()){
            queue_message(client_fd,"[error] usage :MSG <username> <message>\n");
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

        send_private_message(client_fd,target_username,message);
    }

    //v5新增添加好友
    void ChatServer::handle_add_friend(int client_fd,const Command& command){
        if(!require_login(client_fd,"adding friends")){
            return;
        }

        std::string target_username;
        if(!extract_single_username(client_fd,command,"ADD_FRIEND <username>",target_username)){
            return;
        }

        const std::string current_username=clients_.at(client_fd).username;
        if(target_username==current_username){
            queue_message(client_fd,"[error] you cannot add yourself as a friend .\n");
            return;
        }

        
        auto target_it=users_.find(target_username);
        if(target_it==users_.end()){
            queue_message(client_fd,"[error] "+target_username+"does not exist .\n");
            return;
        }
        auto& current_account=users_.at(current_username);
        auto& target_account=target_it->second;

        if(current_account.friends.find(target_username)!=current_account.friends.end()){
            queue_message(client_fd,"[error] "+target_username+" is already your friend. \n");
            return;
        }
        if(current_account.outgoing_friend_requests.find(target_username)!=current_account.outgoing_friend_requests.end()){
            queue_message(client_fd,"[error] friend request already sent to " +target_username +".\n");
            return;
        }
        if(current_account.incoming_friend_requests.find(target_username)!=current_account.incoming_friend_requests.end()){
            queue_message(client_fd,"[error] " +target_username +" already sent you a request. ""Use ACCEPT_FRIEND " +target_username +".\n");
            return;
        }
        current_account.outgoing_friend_requests.insert(target_username);
        target_account.incoming_friend_requests.insert(current_username);
        queue_message(client_fd,"[system] friend request sent to " +target_username +".\n");

        notify_user_if_online(target_username,"[system] friend request from " +current_username +". Use ACCEPT_FRIEND " +current_username +" or REJECT_FRIEND " +current_username +".\n");
    }
    //v5新增接受好友申请：登陆，提取名字，我的名字，users里面找两人，找好友申请，移除，插入
    void ChatServer::handle_accept_friend(int client_fd,const Command& command){
        if(!require_login(client_fd,"accepting friend requests")){
            return;
        }

        std::string requester_username;
        if(!extract_single_username(client_fd,command,"ACCEPT_FRIEND <username>",requester_username)){
            return;
        }


        const std::string current_username=clients_.at(client_fd).username;
   
        auto requester_it=users_.find(requester_username);
        if(requester_it==users_.end()){
            queue_message(client_fd,"[error] requester account no longer exists.\n");
            return;
        }
        auto& current_account=users_.at(current_username);
        auto& requester_account=requester_it->second;

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
    void ChatServer::handle_reject_friend(int client_fd,const Command& command){
        if(!require_login(client_fd,"rejecting friend requests"))return;
        std::string requester_username;
        if(!extract_single_username(client_fd,command,"REJECT_FRIEND <username>",requester_username)){
            return;
        }
        const std::string current_username=clients_.at(client_fd).username;
        
        auto requester_it=users_.find(requester_username);
        if(requester_it==users_.end()){
            queue_message(client_fd,"[error] requester account no longer exists.\n");
            return;
        }
        UserAccount& current_account=users_.at(current_username);

        if(current_account.incoming_friend_requests.find(requester_username)==current_account.incoming_friend_requests.end()){
            queue_message(client_fd,"[error] no pending friend request from " +requester_username +".\n");
            return;
        }
        current_account.incoming_friend_requests.erase(requester_username);
        requester_it->second.outgoing_friend_requests.erase(current_username);

        queue_message(client_fd,"[system] friend request from " +requester_username +" rejected.\n");
        notify_user_if_online(requester_username,"[system] " +current_username +" rejected your friend request.\n");
    }
    //v5：新增删除好友:登陆，提取名字，我的名字，在users里面找
    void ChatServer::handle_remove_friend(int client_fd,const Command& command){
        if(!require_login(client_fd,"removing friends"))return;

        std::string friend_username;
        if(!extract_single_username(client_fd,command,"REMOVE_FRIEND <username>",friend_username)){
            return;
        }
        const std::string current_username=clients_.at(client_fd).username;
        auto friend_it=users_.find(friend_username);

        if(friend_it == users_.end()){
            queue_message(client_fd,"[error] user "+friend_username+" does not exist.\n");
            return;
        }
         auto& current_account =users_.at(current_username);

        if(current_account.friends.find(friend_username) ==current_account.friends.end()){
            queue_message(client_fd,"[error] " +friend_username +" is not your friend.\n");
            return;
        }

        current_account.friends.erase(friend_username);
        friend_it->second.friends.erase(current_username);

        queue_message(client_fd,"[system] removed " +friend_username +" from your friend list.\n");
        notify_user_if_online(friend_username,"[system] " +current_username +" removed you from their friend list.\n");

    }

    //v5:新增获取好友列表，并且标注是否在线
    void ChatServer::send_friend_list(int client_fd){
        if(!require_login(client_fd,"viewing friends"))return;
        const std::string username=clients_.at(client_fd).username;
        
        std::vector<std::string> friends(users_.at(username).friends.begin(),users_.at(username).friends.end());

        std::sort(friends.begin(),friends.end());
        std::ostringstream output;
        output<< "[system] friends ("<< friends.size()<< "):\n";
        if(friends.empty()){
            output << "  (none)\n";
        }else{
            for(const std::string& friend_name:friends){
                const bool online =online_users_.find(friend_name)!=online_users_.end();
                output<< "  "<< friend_name << " ["<< (online ? "online" : "offline") << "]\n";
                
            }
        }
        queue_message(client_fd,output.str());
    }
    //v5显示好友申请列表：发送和接受
    void ChatServer::send_friend_requests(int client_fd){
        if(!require_login(client_fd,"viewing friend requests"))return;

        const std::string username=clients_.at(client_fd).username;
        const auto& account=users_.at(username);

        queue_message(client_fd,"[system] incoming requests (" +std::to_string(account.incoming_friend_requests.size()) +"): " +join_sorted(account.incoming_friend_requests ) +"\n""[system] outgoing requests (" +std::to_string(account.outgoing_friend_requests.size()) +"): " + join_sorted(account.outgoing_friend_requests) +"\n");
    }

    //v6新增函数：处理who
    void ChatServer::handle_who(int client_fd){
       if(!require_login(client_fd,"using WHO")){
        return;
       }

        std::vector<std::string> names;
        names.reserve(online_users_.size());

        for (const auto &[username, fd] : online_users_)
        {
            (void)fd;
            names.push_back(username);
        }

        std::sort(names.begin(), names.end());

        std::ostringstream output;
        output<<"[system] online users (" + std::to_string(names.size()) + "): ";

        if(names.empty()){
            output<<"(none)";
        }else{
         for (std::size_t i = 0; i < names.size(); ++i)
         {
            if (i != 0)
            {
                output<< ", ";
            }
             output<< names[i];
         }
    }
        output<< "\n";

        queue_message(client_fd, output.str());

    }

    //v6新增函数：处理公开历史消息
    void ChatServer::handle_public_history(int client_fd,const Command& command){
        if(!require_login(client_fd,"viewing public history"))return;
        std::size_t count=0;
        if(!parse_count(command.raw_arguments,kDefaultHistoryCount,kMaxHistoryQueryCount,count)){
            queue_message(client_fd,"[error] usage: HISTORY_PUBLIC [count], where count is 1-100.\n");
            return;
        }
        const auto messages=message_store_.recent_public(count);
        std::ostringstream output;
       output
        << "[history public] showing "
        << messages.size()
        << " message(s):\n";

        if(messages.empty()){
            output<<"(none)\n";
        }else{
            for(const ChatMessage& message:messages){
                output<< format_public_history_line(
                    message);
            }
        }
        queue_message(client_fd,output.str());

    }
    //v6新增函数：处理私聊历史信息
    void ChatServer::handle_private_history(int client_fd,const Command& command){
        if(!require_login(client_fd,"viewing private history"))return;
        
        std::string peer_username;
        std::string count_text;
        if(!split_first_token(command.raw_arguments,peer_username,count_text)||peer_username.empty()){
            queue_message(client_fd,"[error] usage: HISTORY_PRIVATE <username> [count]\n");
            return;
        }
        if(!is_valid_username(peer_username)){
            queue_message(client_fd,"[error] invalid username.\n");
            return;
        }
        if(users_.find(peer_username)==users_.end()){
            queue_message(client_fd,"[error] user " + peer_username + " does not exist.\n");
            return;
        }
        const std::string current_username=clients_.at(client_fd).username;
        if(peer_username==current_username){
            queue_message(client_fd,"[error] private history requires another username.\n");
            return;
        }
        std::size_t count = 0;//查询的消息数量
        if(!parse_count(
            count_text,
            kDefaultHistoryCount,
            kMaxHistoryQueryCount,
            count
        )){
            queue_message(client_fd,"[error] usage: HISTORY_PRIVATE <username> [count], where count is 1-100.\n");
            return;
        }
        const auto messages=message_store_.recent_private(current_username,peer_username,count);
        std::ostringstream output;
        output
        << "[history private with "
        << peer_username
        << "] showing "
        << messages.size()
        << " message(s):\n";

        if(messages.empty()){
            output<<"(none)\n";
        }else{
            for(const ChatMessage& message:messages){
                 output
                << format_private_history_line(
                    message
                );
            }
        }
        queue_message(client_fd,output.str());
    }

    void ChatServer::send_private_message(int sender_fd,const std::string& target_username,const std::string& message){
        //验证登陆状态
        const auto sender_it=clients_.find(sender_fd);
        if (sender_it == clients_.end()) {
            return;
        }
        
        //禁止给自己发消息
        const std::string sender_username=sender_it->second.username;
        if(target_username==sender_username){
            queue_message(
            sender_fd,
            "[error] you cannot send a private message to yourself.\n");
            return;
        }

        if(users_.find(target_username)==users_.end()){
            queue_message(sender_fd,"[error] user " + target_username +" does not exist.\n");
            return;
        }
        

        //检查好友关系
        if(!are_friends(sender_username,target_username)){
            queue_message(sender_fd,"[error] private messaging is allowed only between friends.\n");
            return;
        }
        //在线
        const auto online_it=online_users_.find(target_username);
        if(online_it==online_users_.end()){
            queue_message(sender_fd,"[error] friend "+target_username+" is offline.\n");
            return;
        }
        //会话有效：
        const int target_fd=online_it->second;
        const auto target_it=clients_.find(target_fd);

        if(target_it==clients_.end()||!target_it->second.logged_in||target_it->second.username!=target_username){
            online_users_.erase(online_it);
            queue_message(sender_fd,"[error] target session is unavailable.\n");
            return;
        }
        message_store_.add_private(sender_username,target_username,message);
        //发送信息
        queue_message(target_fd,"[private from "+sender_username+"]"+message+"\n");
        queue_message(sender_fd,"[private to "+target_username+"]"+message+"\n");
    }
    //v6新增功能
    bool ChatServer::require_login(int client_fd,const std::string& action){
        const auto it=clients_.find(client_fd);
        if(it==clients_.end())return false;
        if(it->second.logged_in){
            return true;
        }
        queue_message(client_fd,"[error] you must LOGIN before "+action+".\n");
        return false;
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
    bool ChatServer::extract_single_username(int client_fd,const Command& command,const std::string& usage,std::string& username){
        const std::vector<std::string> arguments=split_words(command.raw_arguments);
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
    
    //v6新增函数，处理查询历史消息
    std::string ChatServer::format_public_history_line(const ChatMessage& message) {
        std::ostringstream output;
        output
        << "  #"
        << message.id
        << " "
        << InMemoryMessageStore::format_timestamp(
            message.created_at
        )
        << " ["
        << message.sender
        << "] "
        << message.content
        << "\n";

    return output.str();
    }
    std::string ChatServer::format_private_history_line(const ChatMessage& message) {
        std::ostringstream output;
        output
        << "  #"
        << message.id
        << " "
        << InMemoryMessageStore::format_timestamp(
            message.created_at
        )
        << " "
        << message.sender
        << " -> "
        << message.recipient
        << ": "
        << message.content
        << "\n";

    return output.str();
    }


    
}


#include "chat_server.hpp"
#include "minimuduo/net/TcpConnection.hpp"

void ChatServer::configure_command_routes() {
  auto add_arguments_handler = [this](const std::string &name, auto handler) {
    command_router_.add(
        name, [this, handler](const TcpConnectionPtr &connection,
                              ClientSession &session, const Command &command) {
          (this->*handler)(connection, session, command.raw_arguments);
        });
  };

  command_router_.add(
      "HELP", [this](const TcpConnectionPtr &connection, ClientSession &,
                     const Command &) { send_help(connection); });

  add_arguments_handler("REGISTER", &ChatServer::handle_register);

  add_arguments_handler("LOGIN", &ChatServer::handle_login);

  command_router_.add(
      "LOGOUT",
      [this](const TcpConnectionPtr &connection, ClientSession &session,
             const Command &) { handle_logout(connection, session); });

  add_arguments_handler("SAY", &ChatServer::handle_public_message);

  add_arguments_handler("MSG", &ChatServer::handle_private_message);

  command_router_.add(
      "WHO", [this](const TcpConnectionPtr &connection, ClientSession &session,
                    const Command &) { handle_who(connection, session); });

  add_arguments_handler("ADD_FRIEND", &ChatServer::handle_add_friend);

  add_arguments_handler("ACCEPT_FRIEND", &ChatServer::handle_accept_friend);

  add_arguments_handler("REJECT_FRIEND", &ChatServer::handle_reject_friend);

  add_arguments_handler("REMOVE_FRIEND", &ChatServer::handle_remove_friend);

  add_arguments_handler("BLOCK_FRIEND", &ChatServer::handle_block_friend);

  add_arguments_handler("UNBLOCK_FRIEND", &ChatServer::handle_unblock_friend);

  command_router_.add(
      "BLOCKED_FRIENDS",
      [this](const TcpConnectionPtr &connection, ClientSession &session,
             const Command &) { handle_blocked_friends(connection, session); });

  command_router_.add(
      "FRIENDS",
      [this](const TcpConnectionPtr &connection, ClientSession &session,
             const Command &) { handle_friends(connection, session); });

  command_router_.add(
      "FRIEND_REQUESTS",
      [this](const TcpConnectionPtr &connection, ClientSession &session,
             const Command &) { handle_friend_requests(connection, session); });

  add_arguments_handler("HISTORY_PUBLIC", &ChatServer::handle_history_public);

  add_arguments_handler("HISTORY_PRIVATE", &ChatServer::handle_history_private);

  add_arguments_handler("CREATE_GROUP", &ChatServer::handle_create_group);

  add_arguments_handler("DISSOLVE_GROUP", &ChatServer::handle_dissolve_group);

  add_arguments_handler("APPLY_GROUP", &ChatServer::handle_apply_group);

  command_router_.add(
      "MY_GROUPS",
      [this](const TcpConnectionPtr &connection, ClientSession &session,
             const Command &) { handle_my_groups(connection, session); });

  add_arguments_handler("LEAVE_GROUP", &ChatServer::handle_leave_group);

  add_arguments_handler("GROUP_MEMBERS", &ChatServer::handle_group_members);

  add_arguments_handler("ADD_GROUP_ADMIN", &ChatServer::handle_add_group_admin);

  add_arguments_handler("REMOVE_GROUP_ADMIN",
                        &ChatServer::handle_remove_group_admin);

  add_arguments_handler("GROUP_REQUESTS", &ChatServer::handle_group_requests);

  add_arguments_handler("APPROVE_GROUP", &ChatServer::handle_approve_group);

  add_arguments_handler("REJECT_GROUP", &ChatServer::handle_reject_group);

  add_arguments_handler("REMOVE_GROUP_MEMBER",
                        &ChatServer::handle_remove_group_member);

  add_arguments_handler("GROUP_MSG", &ChatServer::handle_group_message);

  add_arguments_handler("HISTORY_GROUP", &ChatServer::handle_history_group);

  add_arguments_handler("FILE_BEGIN_PRIVATE",
                        &ChatServer::handle_file_begin_private);

  add_arguments_handler("FILE_BEGIN_GROUP",
                        &ChatServer::handle_file_begin_group);

  add_arguments_handler("FILE_CHUNK", &ChatServer::handle_file_chunk);

  add_arguments_handler("FILE_END", &ChatServer::handle_file_end);

  add_arguments_handler("FILE_ABORT", &ChatServer::handle_file_abort);

  add_arguments_handler("FILE_RESUME_REQUEST",
                        &ChatServer::handle_file_resume_request);

  add_arguments_handler("FILE_RECEIVED", &ChatServer::handle_file_received);

  add_arguments_handler("FILE_RECEIVE_FAILED",
                        &ChatServer::handle_file_receive_failed);

  command_router_.add(
      "PENDING",
      [this](const TcpConnectionPtr &connection, ClientSession &session,
             const Command &) { handle_pending(connection, session); });

  command_router_.add("PING", [this](const TcpConnectionPtr &connection,
                                     ClientSession &, const Command &command) {
    handle_ping(connection, command.raw_arguments);
  });

  command_router_.add("QUIT", [](const TcpConnectionPtr &connection,
                                 ClientSession &, const Command &) {
    connection->send("[system] goodbye.\n");

    connection->shutdown();
  });
}

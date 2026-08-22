#include "server/command_router.hpp"

#include <utility>

void ServerCommandRouter::add(std::string command_name, Handler handler) {
  handlers_[std::move(command_name)] = std::move(handler);
}

bool ServerCommandRouter::dispatch(const TcpConnectionPtr &connection,
                                   ClientSession &session,
                                   const Command &command) const {
  const auto iterator = handlers_.find(command.name);

  if (iterator == handlers_.end()) {
    return false;
  }

  iterator->second(connection, session, command);

  return true;
}

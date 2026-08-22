#include "client/client_message_cache.hpp"

#include "client/client_common.hpp"

#include <iostream>

bool parse_private_message_line(const std::string &line,
                                const std::string &active_username,
                                LocalPrivateMessage &message) {
  if (active_username.empty()) {
    return false;
  }

  bool offline = false;
  std::size_t id_begin = 0U;

  if (starts_with(line, "[offline #")) {
    offline = true;

    id_begin = std::string("[offline #").size();
  } else if (starts_with(line, "[#") && !starts_with(line, "[#G")) {
    id_begin = 2U;
  } else {
    return false;
  }

  const std::size_t id_end = line.find(']', id_begin);

  if (id_end == std::string::npos ||
      !parse_uint64(line.substr(id_begin, id_end - id_begin),
                    message.server_message_id)) {
    return false;
  }

  const std::string from_marker = " [private from ";

  const std::string to_marker = " [private to ";

  const std::size_t marker_begin = id_end + 1U;

  bool outgoing = false;
  std::size_t name_begin = 0U;

  if (line.compare(marker_begin, from_marker.size(), from_marker) == 0) {
    name_begin = marker_begin + from_marker.size();
  } else if (line.compare(marker_begin, to_marker.size(), to_marker) == 0) {
    outgoing = true;

    name_begin = marker_begin + to_marker.size();
  } else {
    return false;
  }

  const std::size_t name_end = line.find("] ", name_begin);

  if (name_end == std::string::npos) {
    return false;
  }

  const std::string peer = line.substr(name_begin, name_end - name_begin);

  if (peer.empty()) {
    return false;
  }

  message.account_username = active_username;

  message.peer_username = peer;

  message.outgoing = outgoing;

  message.offline_delivery = offline;

  message.received_at_unix_ms = client_now_unix_ms();

  message.content = line.substr(name_end + 2U);

  if (outgoing) {
    message.sender_username = active_username;

    message.recipient_username = peer;
  } else {
    message.sender_username = peer;

    message.recipient_username = active_username;
  }

  return true;
}

bool parse_group_message_line(const std::string &line,
                              const std::string &active_username,
                              LocalGroupMessage &message) {
  if (active_username.empty()) {
    return false;
  }

  bool offline = false;
  std::size_t id_begin = 0U;

  if (starts_with(line, "[offline #G")) {
    offline = true;

    id_begin = std::string("[offline #G").size();
  } else if (starts_with(line, "[#G")) {
    id_begin = 3U;
  } else {
    return false;
  }

  const std::size_t id_end = line.find(']', id_begin);

  if (id_end == std::string::npos ||
      !parse_uint64(line.substr(id_begin, id_end - id_begin),
                    message.server_message_id)) {
    return false;
  }

  const std::string group_marker = " [group ";

  const std::size_t group_begin = id_end + 1U;

  if (line.compare(group_begin, group_marker.size(), group_marker) != 0) {
    return false;
  }

  const std::size_t group_name_begin = group_begin + group_marker.size();

  const std::size_t group_name_end = line.find("] [", group_name_begin);

  if (group_name_end == std::string::npos) {
    return false;
  }

  const std::size_t sender_begin = group_name_end + 3U;

  const std::size_t sender_end = line.find("] ", sender_begin);

  if (sender_end == std::string::npos) {
    return false;
  }

  message.account_username = active_username;

  message.group_name =
      line.substr(group_name_begin, group_name_end - group_name_begin);

  message.sender_username =
      line.substr(sender_begin, sender_end - sender_begin);

  message.content = line.substr(sender_end + 2U);

  message.received_at_unix_ms = client_now_unix_ms();

  message.outgoing = message.sender_username == active_username;

  message.offline_delivery = offline;

  return !message.group_name.empty() && !message.sender_username.empty();
}

void cache_server_message(const std::string &line, const ClientState &state,
                          SqliteClient &cache) {
  if (state.active_username.empty()) {
    return;
  }

  std::string error;

  LocalPrivateMessage private_message;

  if (parse_private_message_line(line, state.active_username,
                                 private_message)) {
    if (!cache.cache_private_message(private_message, error)) {
      std::cerr << "[local sqlite error] " << error << '\n';
    }

    return;
  }

  LocalGroupMessage group_message;

  if (parse_group_message_line(line, state.active_username, group_message)) {
    if (!cache.cache_group_message(group_message, error)) {
      std::cerr << "[local sqlite error] " << error << '\n';
    }
  }
}

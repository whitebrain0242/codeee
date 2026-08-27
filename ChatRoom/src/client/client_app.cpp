#include "client/client_app.hpp"
#include "client/client_common.hpp"
#include "client/client_chat_session.hpp"
#include "client/client_numeric_commands.hpp"
#include "client/client_file_transfer.hpp"
#include "client/client_local_commands.hpp"
#include "client/client_message_cache.hpp"
#include "config.hpp"
#include "file_utils.hpp"
#include "protocol.hpp"
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <stdexcept>
#include <vector>

int ClientApp::run(const ClientAppConfig &config) {
  if (!initialize(config)) return 1;

  std::cout << "TLS 已连接到 " << config.host << ":" << config.port << '\n'
            << "[TLS] 版本：" << transport_.tls_version()
            << "，加密套件：" << transport_.cipher_name() << '\n'
            << "[TLS] 已验证服务端身份：" << transport_.peer_identity() << '\n'
            << "[心跳] 客户端每 20 秒发送 PING，服务端回复 PONG，"
               "60 秒未收到对应 PONG 则判定超时。\n"
            << "[本地] SQLite 缓存：" << cache_.database_path() << '\n'
            << "[本地] 下载目录：" << state_.download_root.string() << '\n';

  print_current_menu();

  pollfd descriptors[2]{};
  descriptors[0].fd = STDIN_FILENO;
  descriptors[0].events = POLLIN;
  descriptors[1].fd = transport_.fd();

  bool running = true;
  while (running) {
    // SSL 内部已经解密好的数据不必等待内核再次产生 POLLIN。
    if (transport_.pending() > 0 && !read_tls_available()) break;

    descriptors[0].revents = 0;
    descriptors[1].revents = 0;
    descriptors[1].events =
        static_cast<short>(POLLIN |
                           (transport_.wants_write_event() ? POLLOUT : 0));

    const int result = ::poll(descriptors, 2, 1000);
    if (result < 0) {
      if (errno == EINTR) continue;
      std::cerr << "poll 调用失败：" << std::strerror(errno) << '\n';
      break;
    }

    // 网络接收优先。大量消息突发时，先尽快把服务端数据读走并显示，
    // 避免 stdin 连续可读时把接收端“饿死”。
    if (descriptors[1].revents & POLLIN) {
      if (!read_tls_available()) {
        running = false;
      }

      // 某次 SSL_write 可能处于 WANT_READ；读事件完成后立即重试发送队列。
      if (running && transport_.has_pending_output()) {
        std::string send_error;
        if (!transport_.flush_queued(send_error)) {
          std::cerr << "[网络错误] 刷新消息发送队列失败：" << send_error << '\n';
          running = false;
        }
      }
    }

    if (!running) break;

    if (descriptors[0].revents & POLLIN) {
      std::string line;
      if (!std::getline(std::cin, line)) break;
      if (!handle_user_input(line)) break;
    }

    if (descriptors[1].revents & POLLOUT) {
      std::string send_error;
      if (!transport_.flush_queued(send_error)) {
        std::cerr << "[网络错误] 刷新消息发送队列失败：" << send_error << '\n';
        break;
      }
    }

    if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      if (transport_.pending() > 0) (void)read_tls_available();
      break;
    }

    // 心跳只入非阻塞 TLS 队列，不再直接做可能阻塞的 SSL_write。
    std::string heartbeat_error;
    if (!heartbeat_.tick(transport_, heartbeat_error)) {
      std::cerr << "[心跳] " << heartbeat_error << "；正在关闭连接。\n";
      break;
    }
  }

  if (!server_buffer_.empty()) process_server_line(server_buffer_);
  preserve_partial_downloads(state_);
  cache_writer_.stop();
  transport_.shutdown();
  return 0;
}

bool ClientApp::handle_user_input(const std::string &line) {
  // 33/34 编号选择状态优先于普通数字菜单。
  if (state_.group_request_selection_active) {
    return handle_group_request_selection_input(line);
  }

  // 只有用户在好友/群会话里主动输入 6 后，才可能进入下面两个状态。
  if (state_.chat_mode_selection_active) {
    const bool ok = handle_chat_mode_selection_input(state_, line);
    if (ok && !chat_input_active(state_)) print_current_menu();
    return ok;
  }

  if (state_.chat_input_mode != ChatInputMode::None) {
    const bool was_in_chat = has_active_chat(state_);
    const bool ok = handle_chat_message_input(transport_, state_, line);
    if (ok && !chat_input_active(state_)) {
      // /back：仍在会话；/quit：已经离开会话。两种情况都打印对应菜单。
      print_current_menu();
    } else if (ok && was_in_chat && !has_active_chat(state_)) {
      print_current_menu();
    }
    return ok;
  }

  // 输入数字命令后进入参数状态时，下一行就是参数文本。
  if (pending_input_command_ != 0) {
    if (trim(line) == "/cancel") {
      pending_input_command_ = 0;
      std::cout << "[本地] 已取消本次输入。\n";
      print_current_menu();
      return true;
    }

    const int number = pending_input_command_;
    pending_input_command_ = 0;
    return execute_numeric_command(number, line);
  }

  ParsedNumericCommand command;
  if (!parse_numeric_command_line(line, command)) {
    std::cout << "[本地错误] 当前状态只能输入菜单中的数字命令。\n";
    print_current_menu();
    return true;
  }

  if (command.arguments.empty() && command_requires_followup_input(command.number)) {
    begin_followup_input(command.number);
    return true;
  }

  return execute_numeric_command(command.number, command.arguments);
}

bool ClientApp::command_requires_followup_input(int number) const {
  switch (number) {
  case 1:
  case 2:
  case 4:
  case 5:
  case 8:
  case 9:
  case 14:
  case 17:
  case 18:
  case 19:
  case 20:
  case 21:
  case 22:
  case 24:
  case 25:
  case 26:
  case 28:
  case 29:
  case 30:
  case 31:
  case 35:
    return true;
  case 11:
    return !(state_.chat_scope == ClientChatScope::Private &&
             !state_.chat_target.empty());
  case 12:
    return !(state_.chat_scope == ClientChatScope::Group &&
             !state_.chat_target.empty());
  default:
    return false;
  }
}

void ClientApp::begin_followup_input(int number) {
  pending_input_command_ = number;
  switch (number) {
  case 1:
    std::cout << "[输入] 注册请输入: 用户名 密码\n";
    break;
  case 2:
    std::cout << "[输入] 登录请输入: 用户名 密码\n";
    break;
  case 4:
    std::cout << "[输入] 注销账户请输入当前密码（客户端会自动补 CONFIRM）:\n";
    break;
  case 5:
    std::cout << "[输入] 请输入公共消息内容:\n";
    break;
  case 8:
    std::cout << "[输入] 请输入要进入私聊的好友用户名:\n";
    break;
  case 9:
    std::cout << "[输入] 请输入要进入的群名称:\n";
    break;
  case 11:
    std::cout << "[输入] 请输入好友用户名，可选再加历史条数，例如: alice 20\n";
    break;
  case 12:
    std::cout << "[输入] 请输入群名称，可选再加历史条数，例如: group1 20\n";
    break;
  case 14:
    std::cout << "[输入] 请输入要发送的本地文件路径:\n";
    break;
  case 17:
    std::cout << "[输入] 请输入要添加的好友用户名:\n";
    break;
  case 18:
    std::cout << "[输入] 请输入要通过申请的好友用户名:\n";
    break;
  case 19:
    std::cout << "[输入] 请输入要拒绝申请的好友用户名:\n";
    break;
  case 20:
    std::cout << "[输入] 请输入要删除的好友用户名:\n";
    break;
  case 21:
    std::cout << "[输入] 请输入要屏蔽的好友用户名:\n";
    break;
  case 22:
    std::cout << "[输入] 请输入要解除屏蔽的用户名:\n";
    break;
  case 24:
    std::cout << "[输入] 请输入要创建的群名称:\n";
    break;
  case 25:
    std::cout << "[输入] 请输入要解散的群名称:\n";
    break;
  case 26:
    std::cout << "[输入] 请输入要申请加入的群名称:\n";
    break;
  case 28:
    std::cout << "[输入] 请输入要退出的群名称:\n";
    break;
  case 29:
    std::cout << "[输入] 请输入要查看成员的群名称:\n";
    break;
  case 30:
    std::cout << "[输入] 请输入: 群名称 用户名（设置管理员）\n";
    break;
  case 31:
    std::cout << "[输入] 请输入: 群名称 用户名（取消管理员）\n";
    break;
  case 35:
    std::cout << "[输入] 请输入: 群名称 用户名（移出群成员）\n";
    break;
  default:
    pending_input_command_ = 0;
    std::cout << "[本地错误] 该命令不需要额外输入。\n";
    print_current_menu();
    break;
  }

  if (pending_input_command_ != 0) {
    std::cout << "[输入] 输入 /cancel 可取消并返回当前菜单。\n";
  }
}

bool ClientApp::execute_numeric_command(int number,
                                        const std::string &raw_arguments) {
  std::string arguments = trim(raw_arguments);
  const bool logged_in = !state_.active_username.empty();
  const bool in_chat = has_active_chat(state_);

  // 三层菜单是实际权限状态，不只是显示效果。
  if (!logged_in) {
    if (number != 1 && number != 2 && number != 38 && number != 39 &&
        number != 40) {
      std::cout << "[本地错误] 当前未登录，只能使用未登录菜单中的操作。\n";
      print_current_menu();
      return true;
    }
  } else if (in_chat) {
    const bool private_chat = state_.chat_scope == ClientChatScope::Private;
    const bool group_chat = state_.chat_scope == ClientChatScope::Group;
    const bool allowed = number == 6 || number == 10 || number == 13 ||
                         number == 14 || number == 39 ||
                         (private_chat && number == 11) ||
                         (group_chat && number == 12);
    if (!allowed) {
      std::cout << "[本地错误] 当前处于会话中，请先用 10 或 /quit 退出会话，"
                   "再处理好友申请、群管理等操作。\n";
      print_current_menu();
      return true;
    }
  } else {
    if (number == 1 || number == 2 || number == 6 || number == 7 ||
        number == 10 || number == 13 || number == 14) {
      std::cout << "[本地错误] 该操作与当前登录状态不匹配。\n";
      print_current_menu();
      return true;
    }
  }

  if (number == 4 && !arguments.empty()) {
    const std::vector<std::string> words = split_words(arguments);
    if (words.size() == 1U) arguments += " CONFIRM";
  }

  // 在对应会话里查看历史时，默认使用当前好友/群，不再要求重复输入目标。
  if (number == 11 && in_chat && state_.chat_scope == ClientChatScope::Private &&
      arguments.empty()) {
    arguments = state_.chat_target;
  }
  if (number == 12 && in_chat && state_.chat_scope == ClientChatScope::Group &&
      arguments.empty()) {
    arguments = state_.chat_target;
  }

  if (number == 6) {
    if (!arguments.empty()) {
      std::cout << "[本地错误] 命令 6 不需要参数。\n";
      print_current_menu();
      return true;
    }
    (void)begin_chat_mode_selection(state_);
    return true;
  }

  if (number == 7) {
    std::cout << "[本地错误] 群聊发送消息也统一使用命令 6；"
                 "进入群会话后请输入 6 再选择消息模式。\n";
    print_current_menu();
    return true;
  }

  if (number == 33 || number == 34) {
    if (!arguments.empty()) {
      std::cout << "[本地错误] 33/34 不需要手动输入群名和用户名；"
                   "客户端会自动列出全部可处理申请。\n";
      print_current_menu();
      return true;
    }
    return begin_group_request_action(
        number == 33 ? GroupRequestAction::Approve : GroupRequestAction::Reject);
  }

  if (number == 8 || number == 9) {
    const std::vector<std::string> words = split_words(arguments);
    if (words.size() != 1U) {
      std::cout << "[本地错误] 请输入且只输入一个"
                << (number == 8 ? "好友用户名" : "群名称") << "。\n";
      print_current_menu();
      return true;
    }
    const ClientChatScope scope =
        number == 8 ? ClientChatScope::Private : ClientChatScope::Group;
    const bool ok = request_chat_entry(transport_, state_, scope, words[0]);
    if (ok) request_menu_refresh();
    return ok;
  }

  if (number == 10) {
    if (!arguments.empty()) {
      std::cout << "[本地错误] 退出会话不需要参数。\n";
      print_current_menu();
      return true;
    }
    leave_chat_session(state_, true);
    print_current_menu();
    return true;
  }

  if (handle_local_numeric_command(transport_, number, arguments, state_, cache_)) {
    print_current_menu();
    return true;
  }

  const char *name = numeric_command_name(number);
  if (name == nullptr) {
    std::cout << "[本地错误] 未知数字命令。\n";
    print_current_menu();
    return true;
  }

  std::string wire = name;
  if (!arguments.empty()) {
    wire += " ";
    wire += arguments;
  }
  wire += "\n";

  remember_login_attempt(wire);

  std::string send_error;
  if (!transport_.send(wire, send_error)) {
    std::cerr << "TLS 发送失败：" << send_error << '\n';
    return false;
  }

  request_menu_refresh();
  return true;
}

void ClientApp::print_current_menu() const {
  if (state_.active_username.empty()) {
    // 未登录菜单保持不变
    std::cout
        << "\n========== 未登录 ==========" << '\n'
        << "1-注册账户" << '\n'
        << "2-登录账户" << '\n'
        << "38-查看服务端帮助" << '\n'
        << "39-查看完整数字命令帮助" << '\n'
        << "40-查看本地缓存/下载路径" << '\n'
        << "请选择数字编号: " << std::flush;
    return;
  }

  if (has_active_chat(state_)) {
    // 会话内菜单保持不变（简短）
    const bool private_chat = state_.chat_scope == ClientChatScope::Private;
    std::cout << "\n========== "
              << (private_chat ? "好友私聊" : "群聊") << "："
              << state_.chat_target << " ==========" << '\n'
              << "6-发送消息（先选择“回车立即发送/长文本”模式）" << '\n'
              << (private_chat ? "11-查看当前好友本地历史" : "12-查看当前群本地历史")
              << '\n'
              << "13-查看当前会话文件记录" << '\n'
              << "14-向当前会话发送文件" << '\n'
              << "10-退出当前会话" << '\n'
              << "39-查看完整数字命令帮助" << '\n'
              << "请选择数字编号: " << std::flush;
    return;
  }

  // ---------- 已登录主菜单（分组，不强制列对齐） ----------
  std::cout
      << "\n========== 已登录：" << state_.active_username << " ==========\n"
      << "\n[好友/私聊]\n"
      << "  8-进入私聊  11-私聊历史  15-好友列表  16-好友申请\n"
      << "  17-添加好友  18-通过申请  19-拒绝申请  20-删除好友\n"
      << "  21-屏蔽  22-解除屏蔽  23-屏蔽列表\n"
      << "\n[群组]\n"
      << "  9-进入群聊  12-群聊历史  24-创建群  25-解散群\n"
      << "  26-申请入群  27-我的群  28-退出群  29-群成员\n"
      << "  30-设置管理  31-取消管理  33-通过入群\n"
      << "  34-拒绝入群  35-移出成员\n"
      << "\n[其他]\n"
      << "  3-退出登录  4-注销账户  5-公共消息  36-在线用户\n"
      << "  37-待处理  41-公共历史  39-完整帮助\n"
      << "\n请选择数字编号: " << std::flush;
}

void ClientApp::request_menu_refresh() { menu_refresh_pending_ = true; }

void ClientApp::flush_menu_refresh() {
  if (!menu_refresh_pending_ || chat_input_active(state_) ||
      state_.chat_entry_pending || state_.binary_download.remaining_bytes > 0U) {
    return;
  }
  menu_refresh_pending_ = false;
  print_current_menu();
}

bool ClientApp::handle_chat_message_visibility(const std::string &line) {
  if (state_.active_username.empty()) return false;

  LocalPrivateMessage private_message;
  if (parse_private_message_line(line, state_.active_username, private_message)) {
    cache_writer_.enqueue(private_message);

    const bool matching_chat =
        state_.chat_scope == ClientChatScope::Private &&
        state_.chat_target == private_message.peer_username;
    if (matching_chat) {
      (void)display_chat_message_line(line, state_);
      return true;
    }

    state_.pending_private_message_lines[private_message.peer_username].push_back(line);
    if (state_.private_unread_notified.insert(private_message.peer_username).second) {
      std::cout << "[未读提示] 好友 " << private_message.peer_username
                << " 有未读消息；进入与该好友的私聊后显示正文。\n";
    }
    return true;
  }

  LocalGroupMessage group_message;
  if (parse_group_message_line(line, state_.active_username, group_message)) {
    cache_writer_.enqueue(group_message);

    const bool matching_chat =
        state_.chat_scope == ClientChatScope::Group &&
        state_.chat_target == group_message.group_name;
    if (matching_chat) {
      (void)display_chat_message_line(line, state_);
      return true;
    }

    state_.pending_group_message_lines[group_message.group_name].push_back(line);
    if (state_.group_unread_notified.insert(group_message.group_name).second) {
      std::cout << "[未读提示] 群 " << group_message.group_name
                << " 有未读消息；进入该群聊后显示正文。\n";
    }
    return true;
  }

  return false;
}

void ClientApp::flush_deferred_chat_items() {
  if (!has_active_chat(state_)) return;

  if (state_.chat_scope == ClientChatScope::Private) {
    auto message_it = state_.pending_private_message_lines.find(state_.chat_target);
    if (message_it != state_.pending_private_message_lines.end()) {
      for (const std::string &line : message_it->second) {
        (void)display_chat_message_line(line, state_);
      }
      state_.pending_private_message_lines.erase(message_it);
    }
    state_.private_unread_notified.erase(state_.chat_target);

    auto file_it = state_.pending_private_file_offers.find(state_.chat_target);
    if (file_it != state_.pending_private_file_offers.end()) {
      const std::vector<std::string> offers = std::move(file_it->second);
      state_.pending_private_file_offers.erase(file_it);
      state_.private_file_unread_notified.erase(state_.chat_target);
      for (const std::string &offer : offers) {
        (void)handle_file_protocol_line(transport_, offer, state_, cache_);
      }
    }
    return;
  }

  auto message_it = state_.pending_group_message_lines.find(state_.chat_target);
  if (message_it != state_.pending_group_message_lines.end()) {
    for (const std::string &line : message_it->second) {
      (void)display_chat_message_line(line, state_);
    }
    state_.pending_group_message_lines.erase(message_it);
  }
  state_.group_unread_notified.erase(state_.chat_target);

  auto file_it = state_.pending_group_file_offers.find(state_.chat_target);
  if (file_it != state_.pending_group_file_offers.end()) {
    const std::vector<std::string> offers = std::move(file_it->second);
    state_.pending_group_file_offers.erase(file_it);
    state_.group_file_unread_notified.erase(state_.chat_target);
    for (const std::string &offer : offers) {
      (void)handle_file_protocol_line(transport_, offer, state_, cache_);
    }
  }
}

bool ClientApp::initialize(const ClientAppConfig &config) {
  std::string error;

  if (!cache_.open(config.sqlite_path, error)) {
    std::cerr << "SQLite 打开失败：" << error << '\n';

    return false;
  }

  cache_writer_.start(cache_);

  TlsClientConfig tls_config;

  if (!load_tls_client_config(config.tls_config_path, tls_config, error)) {
    std::cerr << "TLS 客户端配置加载失败：" << error << '\n';

    return false;
  }

  if (!transport_.connect(config.host, config.port, tls_config, error)) {
    std::cerr << "TLS 连接失败：" << error << '\n';

    return false;
  }

  state_.download_root = config.download_root;

  return true;
}

bool ClientApp::read_tls_available() {
  char buffer[kClientBufferSize]{};
  bool received_any = false;

  while (true) {
    const TransportReadResult result =
        transport_.receive(buffer, sizeof(buffer));

    if (result.status == TransportReadStatus::Data) {
      server_buffer_.append(buffer, result.bytes);
      received_any = true;

      // 非阻塞 socket 下继续读，直到 SSL_read 返回 WANT_READ/WANT_WRITE。
      // 这样 100 条突发消息通常在一次 POLLIN 中整体吸收，而不是每 4 KiB
      // 处理/flush 一次终端输出。
      continue;
    }

    if (result.status == TransportReadStatus::Retry) {
      if (received_any) consume_complete_lines();
      return true;
    }

    if (result.status == TransportReadStatus::Closed) {
      if (received_any) consume_complete_lines();
      return false;
    }

    std::cerr << "TLS 接收失败：" << result.error << '\n';
    return false;
  }
}

void ClientApp::consume_complete_lines() {
  while (true) {
    // 正在接收二进制文件数据
    if (state_.binary_download.remaining_bytes > 0U) {
      if (server_buffer_.empty()) {
        break;
      }

      if (!consume_file_binary_payload(transport_, server_buffer_, state_)) {
        std::cerr << "[文件错误] 无法保存"
                     "二进制下载数据。\n";
        break;
      }

      continue;
    }
    // 查找文本行 用\n分隔
    const std::size_t newline = server_buffer_.find('\n');

    if (newline == std::string::npos) {
      break; // 没有完整行就等待数据
    }

    std::string line = server_buffer_.substr(0, newline);

    server_buffer_.erase(0, newline + 1U);

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    process_server_line(line);
  }

  flush_menu_refresh();
  std::cout.flush();
}

void ClientApp::process_server_line(const std::string &line) {
  if (heartbeat_.consume_protocol_line(line, transport_)) return;
  if (handle_file_protocol_line(transport_, line, state_, cache_)) return;
  if (handle_group_request_protocol_line(line)) return;
  if (consume_chat_entry_response(line, state_)) {
    if (has_active_chat(state_)) flush_deferred_chat_items();
    request_menu_refresh();
    return;
  }

  // 聊天正文在线路上保持 percent-encoded；始终先落库。
  // 只有当前正处于对应好友/群会话时才显示正文，否则只提示一次未读。
  if (handle_chat_message_visibility(line)) return;

  std::string display_line = line;
  if (starts_with(display_line, "[system]")) {
    display_line.replace(0U, std::string("[system]").size(), "[系统]");
  } else if (starts_with(display_line, "[error]")) {
    display_line.replace(0U, std::string("[error]").size(), "[错误]");
  }
  std::cout << display_line << '\n';

  const std::string removed_group_prefix_en = "[system] you were removed from group ";
  const std::string removed_group_prefix_zh = "[system] 你已被移出群 ";
  const bool removed_en = starts_with(line, removed_group_prefix_en);
  const bool removed_zh = starts_with(line, removed_group_prefix_zh);
  const std::string removed_group_prefix =
      removed_zh ? removed_group_prefix_zh : removed_group_prefix_en;
  if (removed_en || removed_zh) {
    const std::size_t name_begin = removed_group_prefix.size();
    std::size_t by = line.find(" by ", name_begin);
    if (by == std::string::npos) by = line.find("，操作者：", name_begin);
    const std::string group_name =
        by == std::string::npos ? trim(line.substr(name_begin))
                                : line.substr(name_begin, by - name_begin);
    if (!group_name.empty()) {
      state_.pending_group_message_lines.erase(group_name);
      state_.group_unread_notified.erase(group_name);
      state_.pending_group_file_offers.erase(group_name);
      state_.group_file_unread_notified.erase(group_name);
      if (state_.chat_scope == ClientChatScope::Group &&
          state_.chat_target == group_name) {
        leave_chat_session(state_, false);
        std::cout << "[本地] 当前群会话已关闭，因为你已不再是该群成员。\n";
        request_menu_refresh();
      }
    }
  }

  if ((starts_with(line, "[system] login successful.") ||
       starts_with(line, "[system] 登录成功")) &&
      !state_.pending_login_username.empty()) {
    state_.active_username = state_.pending_login_username;
    state_.pending_login_username.clear();
    std::cout << "[本地] SQLite 缓存账户：" << state_.active_username << '\n';
    (void)load_and_resume_pending_uploads(transport_, state_, cache_);
  }

  if (starts_with(line, "[error] invalid username or password.") ||
      starts_with(line, "[error] this account is already logged in.") ||
      starts_with(line, "[error] 用户名或密码错误") ||
      starts_with(line, "[error] 该账号已经登录")) {
    state_.pending_login_username.clear();
  }

  const bool logged_out =
      starts_with(line, "[system] logout successful.") ||
      starts_with(line, "[system] 退出登录成功");
  const bool account_deleted =
      starts_with(line, "[system] account deleted successfully.") ||
      starts_with(line, "[system] 账户注销成功");

  // 如果是永久注销账号，删除这个账号对应的 SQLite 缓存
  if (account_deleted && !state_.active_username.empty()) {
    cache_writer_.flush();
    std::string cache_error;
    if (!cache_.clear_account_data(state_.active_username, cache_error)) {
      std::cout << "[本地 SQLite 警告] "
                   "account was deleted on the server, "
                   "but local cache cleanup failed: "
                << cache_error << '\n';
    }
  }

  // LOGOUT 和 DELETE_ACCOUNT 都要清理客户端登录状态以及当前聊天会话
  if (logged_out || account_deleted) {
    leave_chat_session(state_, false);
    state_.active_username.clear();
    state_.pending_login_username.clear();
    state_.pending_uploads.clear();
    state_.upload_queue.clear();
    state_.active_upload_token.clear();
    state_.downloads.clear();
    state_.binary_download = {};
    state_.pending_private_message_lines.clear();
    state_.pending_group_message_lines.clear();
    state_.private_unread_notified.clear();
    state_.group_unread_notified.clear();
    state_.pending_private_file_offers.clear();
    state_.pending_group_file_offers.clear();
    state_.private_file_unread_notified.clear();
    state_.group_file_unread_notified.clear();
  }
}

void ClientApp::clear_group_request_action() {
  state_.group_request_action = GroupRequestAction::None;
  state_.group_request_loading = false;
  state_.group_request_selection_active = false;
  state_.group_request_items.clear();
}

bool ClientApp::begin_group_request_action(GroupRequestAction action) {
  if (action == GroupRequestAction::None) return true;
  clear_group_request_action();
  state_.group_request_action = action;
  state_.group_request_loading = true;

  std::string error;
  if (!transport_.send("GROUP_REQUESTS_ALL\n", error)) {
    clear_group_request_action();
    std::cerr << "[网络错误] 加载入群申请失败：" << error << '\n';
    return false;
  }

  std::cout << "[本地] 正在加载你作为群主或管理员可处理的全部入群申请……\n";
  return true;
}

bool ClientApp::handle_group_request_protocol_line(const std::string &line) {
  if (line == "[group-requests-begin]") {
    if (state_.group_request_action == GroupRequestAction::None) return true;
    state_.group_request_loading = true;
    state_.group_request_items.clear();
    return true;
  }

  const std::string item_prefix = "[group-request-item] ";
  if (starts_with(line, item_prefix)) {
    if (state_.group_request_action == GroupRequestAction::None) return true;
    const std::vector<std::string> words =
        split_words(line.substr(item_prefix.size()));
    if (words.size() != 2U) {
      std::cout << "[本地错误] 入群申请列表数据格式错误。\n";
      clear_group_request_action();
      request_menu_refresh();
      return true;
    }

    PendingGroupJoinRequestItem item;
    std::string decode_error;
    if (!fileutil::percent_decode(words[0], item.group_name, decode_error) ||
        !fileutil::percent_decode(words[1], item.username, decode_error)) {
      std::cout << "[本地错误] 入群申请列表解码失败：" << decode_error << '\n';
      clear_group_request_action();
      request_menu_refresh();
      return true;
    }
    state_.group_request_items.push_back(std::move(item));
    return true;
  }

  const std::string error_prefix = "[group-requests-error] ";
  if (starts_with(line, error_prefix)) {
    std::string reason = "加载可处理入群申请失败";
    std::string decode_error;
    const std::string token = trim(line.substr(error_prefix.size()));
    if (!token.empty()) {
      std::string decoded;
      if (fileutil::percent_decode(token, decoded, decode_error)) reason = decoded;
    }
    std::cout << "[错误] " << reason << '\n';
    clear_group_request_action();
    request_menu_refresh();
    return true;
  }

  if (line != "[group-requests-end]") return false;
  if (state_.group_request_action == GroupRequestAction::None) return true;

  state_.group_request_loading = false;
  if (state_.group_request_items.empty()) {
    std::cout << "\n【可处理入群申请】\n当前没有可处理的入群申请。\n";
    clear_group_request_action();
    request_menu_refresh();
    return true;
  }

  std::cout << "\n【可处理入群申请】\n";
  std::string last_group;
  for (std::size_t i = 0; i < state_.group_request_items.size(); ++i) {
    const auto &item = state_.group_request_items[i];
    if (item.group_name != last_group) {
      if (!last_group.empty()) std::cout << '\n';
      last_group = item.group_name;
      std::cout << last_group << "：\n";
    }
    std::cout << "  " << (i + 1U) << ". " << item.username << '\n';
  }

  state_.group_request_selection_active = true;
  std::cout << "\n当前操作："
            << (state_.group_request_action == GroupRequestAction::Approve
                    ? "通过入群申请"
                    : "拒绝入群申请")
            << "\n请输入处理编号（1-" << state_.group_request_items.size()
            << "），输入 /cancel 可取消： " << std::flush;
  return true;
}

bool ClientApp::handle_group_request_selection_input(const std::string &line) {
  const std::string cleaned = trim(line);
  if (cleaned == "/cancel" || cleaned == "取消") {
    clear_group_request_action();
    std::cout << "[本地] 已取消处理入群申请。\n";
    print_current_menu();
    return true;
  }

  std::size_t selected = 0U;
  try {
    std::size_t consumed = 0U;
    const unsigned long value = std::stoul(cleaned, &consumed, 10);
    if (consumed != cleaned.size() || value == 0UL ||
        value > state_.group_request_items.size()) {
      throw std::out_of_range("selection");
    }
    selected = static_cast<std::size_t>(value - 1UL);
  } catch (...) {
    std::cout << "[本地错误] 请输入列表中的有效数字编号。\n"
              << "请输入处理编号： " << std::flush;
    return true;
  }

  const PendingGroupJoinRequestItem item = state_.group_request_items[selected];
  const GroupRequestAction action = state_.group_request_action;
  const char *command =
      action == GroupRequestAction::Approve ? "APPROVE_GROUP" : "REJECT_GROUP";

  const std::string wire =
      std::string(command) + " " + item.group_name + " " + item.username + "\n";
  std::string error;
  if (!transport_.send(wire, error)) {
    std::cerr << "[网络错误] 提交入群申请处理结果失败：" << error << '\n';
    return false;
  }

  std::cout << "[本地] 已提交："
            << (action == GroupRequestAction::Approve ? "通过 " : "拒绝 ")
            << item.group_name << " / " << item.username << "。\n";
  clear_group_request_action();
  request_menu_refresh();
  return true;
}

void ClientApp::remember_login_attempt(const std::string &line) {
  const Command command = parse_command(line);

  if (command.name != "LOGIN") {
    return;
  }

  const std::vector<std::string> words = split_words(command.raw_arguments);

  if (words.size() == 2U) {
    state_.pending_login_username = words[0];
  }
}

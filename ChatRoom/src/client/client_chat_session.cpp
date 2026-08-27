#include "client/client_chat_session.hpp"

#include "client/client_common.hpp"
#include "client/tls_client_transport.hpp"
#include "file_utils.hpp"
#include "protocol.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kMaxEditableChatMessage = 1000U;

const char *scope_human_name(ClientChatScope scope) {
  switch (scope) {
  case ClientChatScope::Private: return "好友";
  case ClientChatScope::Group: return "群";
  default: return "聊天";
  }
}

std::string join_message_lines(const std::vector<std::string> &lines) {
  std::string message;
  for (std::size_t i = 0U; i < lines.size(); ++i) {
    if (i != 0U) message.push_back('\n');
    message.append(lines[i]);
  }
  return message;
}

bool send_line(TlsClientTransport &transport, const std::string &line) {
  std::string error;
  if (transport.send(line, error)) return true;
  std::cerr << "[网络错误] TLS 发送失败：" << error << '\n';
  return false;
}

void clear_pending_entry(ClientState &state) {
  state.pending_chat_scope = ClientChatScope::None;
  state.pending_chat_target.clear();
  state.chat_entry_pending = false;
}

void reset_chat_input(ClientState &state) {
  state.chat_mode_selection_active = false;
  state.chat_input_mode = ChatInputMode::None;
  state.chat_multiline_finished = false;
  state.chat_message_lines.clear();
}

bool send_chat_message(TlsClientTransport &transport, ClientState &state,
                       const std::string &message) {
  if (message.empty()) {
    std::cout << "[本地错误] 消息内容不能为空。\n";
    return true;
  }
  if (message.size() > kMaxEditableChatMessage) {
    std::cout << "[本地错误] 单条消息最多 1000 字节；当前为 "
              << message.size() << " 字节。\n";
    return true;
  }

  const char *command =
      state.chat_scope == ClientChatScope::Private ? "MSG" : "GROUP_MSG";
  const std::string wire =
      std::string(command) + " " + state.chat_target + " " +
      encode_text_token(message) + "\n";

  std::string error;
  if (transport.queue_send(wire, error)) {
    return true;
  }

  std::cerr << "[网络错误] 消息加入发送队列失败：" << error << '\n';
  // 队列满属于可恢复背压，不要因此退出客户端。
  return transport.connected();
}
} // namespace

bool has_active_chat(const ClientState &state) {
  return state.chat_scope != ClientChatScope::None && !state.chat_target.empty();
}

bool chat_input_active(const ClientState &state) {
  return state.chat_mode_selection_active ||
         state.chat_input_mode != ChatInputMode::None;
}

void leave_chat_input_mode(ClientState &state, bool print_notice) {
  const bool was_active = chat_input_active(state);
  reset_chat_input(state);
  if (print_notice && was_active) {
    std::cout << "[本地] 已退出消息输入模式，仍停留在当前会话。\n";
  }
}

void leave_chat_session(ClientState &state, bool print_notice) {
  const bool had_chat = has_active_chat(state) || state.chat_entry_pending ||
                        chat_input_active(state);
  state.chat_scope = ClientChatScope::None;
  state.chat_target.clear();
  clear_pending_entry(state);
  reset_chat_input(state);

  if (print_notice && had_chat) {
    std::cout << "[系统] 已退出当前会话。\n";
  } else if (print_notice) {
    std::cout << "[本地] 当前没有活动会话。\n";
  }
}

bool request_chat_entry(TlsClientTransport &transport, ClientState &state,
                        ClientChatScope scope, const std::string &target) {
  if (!require_local_account(state)) return true;

  if (target.empty() ||
      (scope != ClientChatScope::Private && scope != ClientChatScope::Group)) {
    std::cout << "[本地错误] 必须指定聊天对象。\n";
    return true;
  }
  if (has_active_chat(state) || chat_input_active(state)) {
    std::cout << "[本地错误] 请先输入 10 退出当前会话，再切换聊天对象。\n";
    return true;
  }
  if (state.chat_entry_pending) {
    std::cout << "[本地错误] 另一个会话目标仍在校验中，请稍后再试。\n";
    return true;
  }
  if (scope == ClientChatScope::Private && target == state.active_username) {
    std::cout << "[本地错误] 不能与自己建立好友私聊会话。\n";
    return true;
  }

  state.pending_chat_scope = scope;
  state.pending_chat_target = target;
  state.chat_entry_pending = true;

  const std::string command =
      std::string(scope == ClientChatScope::Private ? "ENTER_PRIVATE "
                                                    : "ENTER_GROUP ") +
      target + "\n";
  if (!send_line(transport, command)) {
    clear_pending_entry(state);
    return false;
  }

  std::cout << "[本地] 正在校验" << scope_human_name(scope)
            << "会话目标：" << target << "……\n";
  return true;
}

bool consume_chat_entry_response(const std::string &line, ClientState &state) {
  const std::string ok_prefix = "[chat-enter-ok] ";
  const std::string error_prefix = "[chat-enter-error] ";

  if (starts_with(line, ok_prefix)) {
    const std::vector<std::string> words =
        split_words(line.substr(ok_prefix.size()));
    if (words.size() != 2U) {
      std::cout << "[本地错误] 服务端返回的会话校验结果格式错误。\n";
      clear_pending_entry(state);
      return true;
    }

    ClientChatScope scope = ClientChatScope::None;
    if (words[0] == "PRIVATE") scope = ClientChatScope::Private;
    else if (words[0] == "GROUP") scope = ClientChatScope::Group;

    if (!state.chat_entry_pending || scope == ClientChatScope::None ||
        scope != state.pending_chat_scope ||
        words[1] != state.pending_chat_target) {
      std::cout << "[本地警告] 已忽略不匹配的会话校验响应。\n";
      return true;
    }

    state.chat_scope = scope;
    state.chat_target = words[1];
    reset_chat_input(state);
    clear_pending_entry(state);

    // 这里绝不弹出消息模式选择；只进入会话并回到会话菜单。
    std::cout << "[系统] 已进入" << scope_human_name(scope)
              << "会话：" << state.chat_target
              << "。需要发送消息时，请在会话菜单输入 6。\n";
    return true;
  }

  if (starts_with(line, error_prefix)) {
    const std::vector<std::string> words =
        split_words(line.substr(error_prefix.size()));

    if (words.size() >= 2U) {
      const ClientChatScope scope =
          words[0] == "PRIVATE" ? ClientChatScope::Private :
          words[0] == "GROUP" ? ClientChatScope::Group : ClientChatScope::None;
      if (state.chat_entry_pending && scope != ClientChatScope::None &&
          scope != state.pending_chat_scope) {
        std::cout << "[本地警告] 已忽略过期的会话校验失败响应。\n";
        return true;
      }
    }

    std::string reason = "聊天目标校验失败";
    if (words.size() >= 2U) {
      std::string decoded;
      std::string error;
      if (fileutil::percent_decode(words[1], decoded, error)) reason = decoded;
    }

    clear_pending_entry(state);
    std::cout << "[错误] " << reason << '\n';
    return true;
  }

  return false;
}

bool begin_chat_mode_selection(ClientState &state) {
  if (state.chat_entry_pending) {
    std::cout << "[本地错误] 请先等待 8/9 的会话目标校验完成。\n";
    return false;
  }
  if (!has_active_chat(state)) {
    std::cout << "[本地错误] 当前没有活动会话，请先用 8 进入好友私聊或用 9 进入群聊。\n";
    return false;
  }

  reset_chat_input(state);
  state.chat_mode_selection_active = true;
  std::cout
      << "\n请选择消息模式：\n"
      << "1. 回车立即发送\n"
      << "2. 长文本编辑模式（支持换行）\n"
      << "请输入 1 或 2；输入 /cancel 可返回当前会话菜单： "
      << std::flush;
  return true;
}

bool handle_chat_mode_selection_input(ClientState &state,
                                      const std::string &line) {
  if (!state.chat_mode_selection_active) return true;

  const std::string cleaned = trim(line);
  if (cleaned == "/cancel" || cleaned == "取消" || cleaned == "返回") {
    leave_chat_input_mode(state, false);
    std::cout << "[本地] 已取消消息模式选择。\n";
    return true;
  }

  if (cleaned == "1") {
    state.chat_mode_selection_active = false;
    state.chat_input_mode = ChatInputMode::Instant;
    std::cout
        << "[消息模式] 已进入“回车立即发送”。\n"
        << "输入一行内容后按回车就会立即发送。\n"
        << "输入 /back 返回当前会话菜单；输入 /quit 退出当前会话。\n";
    return true;
  }

  if (cleaned == "2") {
    state.chat_mode_selection_active = false;
    state.chat_input_mode = ChatInputMode::MultiLine;
    state.chat_multiline_finished = false;
    state.chat_message_lines.clear();
    std::cout
        << "[消息模式] 已进入“长文本编辑模式”，支持换行和空行。\n"
        << "逐行输入正文；输入“结束”结束本段编辑，再输入“发送”发送。\n"
        << "也可直接输入 /send 发送当前内容；/cancel 清空当前内容；"
           "/back 返回会话菜单；/quit 退出会话。\n";
    return true;
  }

  std::cout << "[本地错误] 请选择 1（回车立即发送）或 2（长文本编辑模式）。\n";
  return true;
}

bool handle_chat_message_input(TlsClientTransport &transport,
                               ClientState &state,
                               const std::string &line) {
  if (state.chat_input_mode == ChatInputMode::None) return true;

  const std::string cleaned = trim(line);

  if (cleaned == "/quit") {
    leave_chat_session(state, true);
    return true;
  }
  if (cleaned == "/back" || cleaned == "返回") {
    leave_chat_input_mode(state, true);
    return true;
  }

  if (state.chat_input_mode == ChatInputMode::Instant) {
    // 即时模式：每一行回车就是一条消息，空行不发送。
    if (line.empty()) {
      std::cout << "[本地错误] 空消息不会发送。\n";
      return true;
    }
    // 不为每条即时消息额外打印“已发送”，减少一秒 100 条时的终端 I/O。
    // 服务端回显带全局消息 ID 的正文就是最终确认。
    if (!send_chat_message(transport, state, line)) return false;
    return true;
  }

  // 长文本模式
  if (cleaned == "/cancel" || cleaned == "取消") {
    state.chat_message_lines.clear();
    state.chat_multiline_finished = false;
    std::cout << "[长文本] 已清空当前未发送内容，可以重新输入。\n";
    return true;
  }

  if (state.chat_multiline_finished) {
    if (cleaned == "发送" || cleaned == "/send") {
      const std::string message = join_message_lines(state.chat_message_lines);
      if (!send_chat_message(transport, state, message)) return false;
      if (!message.empty() && message.size() <= kMaxEditableChatMessage) {
        state.chat_message_lines.clear();
        state.chat_multiline_finished = false;
        std::cout << "[长文本] 已加入发送队列，可以继续输入下一条长文本。\n";
      }
      return true;
    }
    if (cleaned == "继续") {
      state.chat_multiline_finished = false;
      std::cout << "[长文本] 继续编辑。\n";
      return true;
    }
    std::cout << "[长文本] 当前正文已结束，请输入“发送”发送、"
                 "“继续”继续编辑，或“取消”清空。\n";
    return true;
  }

  if (cleaned == "结束") {
    if (state.chat_message_lines.empty()) {
      std::cout << "[本地错误] 当前长文本还没有内容。\n";
      return true;
    }
    state.chat_multiline_finished = true;
    std::cout << "[长文本] 已结束本段编辑。输入“发送”发送，"
                 "输入“继续”返回编辑。\n";
    return true;
  }

  if (cleaned == "/send" || cleaned == "发送") {
    const std::string message = join_message_lines(state.chat_message_lines);
    if (!send_chat_message(transport, state, message)) return false;
    if (!message.empty() && message.size() <= kMaxEditableChatMessage) {
      state.chat_message_lines.clear();
      state.chat_multiline_finished = false;
      std::cout << "[长文本] 已加入发送队列，可以继续输入下一条长文本。\n";
    }
    return true;
  }

  state.chat_message_lines.push_back(line); // 空行也保留
  const std::string message = join_message_lines(state.chat_message_lines);
  if (message.size() > kMaxEditableChatMessage) {
    state.chat_message_lines.pop_back();
    std::cout << "[本地错误] 加入这一行后会超过单条消息 1000 字节，"
                 "本行未加入缓冲区。\n";
  }
  return true;
}

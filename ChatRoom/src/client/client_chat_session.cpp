#include "client/client_chat_session.hpp"

#include "client/client_common.hpp"
#include "client/tls_client_transport.hpp"
#include "file_utils.hpp"
#include "protocol.hpp"

#include <iostream>
#include <string>

namespace {
constexpr std::size_t kMaxEditableChatMessage = 1000U;

const char *scope_human_name(ClientChatScope scope) {
  switch (scope) {
  case ClientChatScope::Private: return "private";
  case ClientChatScope::Group: return "group";
  default: return "chat";
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
  std::cerr << "TLS send failed: " << error << '\n';
  return false;
}

void clear_pending_entry(ClientState &state) {
  state.pending_chat_scope = ClientChatScope::None;
  state.pending_chat_target.clear();
  state.chat_entry_pending = false;
}
} // namespace

bool has_active_chat(const ClientState &state) {
  return state.chat_scope != ClientChatScope::None && !state.chat_target.empty();
}

void leave_chat_session(ClientState &state, bool print_notice) {
  const bool had_chat = has_active_chat(state) || state.chat_entry_pending ||
                        state.chat_editor_active;
  state.chat_scope = ClientChatScope::None;
  state.chat_target.clear();
  clear_pending_entry(state);
  state.chat_editor_active = false;
  state.chat_message_lines.clear();

  if (print_notice && had_chat) {
    std::cout << "[system] Left current chat.\n";
  } else if (print_notice) {
    std::cout << "[local] no active chat session.\n";
  }
}

bool request_chat_entry(TlsClientTransport &transport, ClientState &state,
                        ClientChatScope scope, const std::string &target) {
  if (!require_local_account(state)) return true;

  if (target.empty() ||
      (scope != ClientChatScope::Private && scope != ClientChatScope::Group)) {
    std::cout << "[local error] chat target is required.\n";
    return true;
  }
  if (state.chat_editor_active) {
    std::cout << "[local error] use /quit before switching chat sessions.\n";
    return true;
  }
  if (state.chat_entry_pending) {
    std::cout << "[local error] another chat target is still being validated.\n";
    return true;
  }
  if (scope == ClientChatScope::Private && target == state.active_username) {
    std::cout << "[local error] you cannot enter a private chat with yourself.\n";
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

  std::cout << "[local] validating " << scope_human_name(scope)
            << " chat target " << target << "...\n";
  return true;
}

bool consume_chat_entry_response(const std::string &line, ClientState &state) {
  const std::string ok_prefix = "[chat-enter-ok] ";
  const std::string error_prefix = "[chat-enter-error] ";

  if (starts_with(line, ok_prefix)) {
    const std::vector<std::string> words =
        split_words(line.substr(ok_prefix.size()));
    if (words.size() != 2U) {
      std::cout << "[local error] malformed chat-entry response.\n";
      clear_pending_entry(state);
      return true;
    }

    ClientChatScope scope = ClientChatScope::None;
    if (words[0] == "PRIVATE") scope = ClientChatScope::Private;
    else if (words[0] == "GROUP") scope = ClientChatScope::Group;

    if (!state.chat_entry_pending || scope == ClientChatScope::None ||
        scope != state.pending_chat_scope ||
        words[1] != state.pending_chat_target) {
      std::cout << "[local warning] ignored unexpected chat-entry response.\n";
      return true;
    }

    state.chat_scope = scope;
    state.chat_target = words[1];
    state.chat_editor_active = false;
    state.chat_message_lines.clear();
    clear_pending_entry(state);

    std::cout << "[system] Entered " << scope_human_name(scope)
              << " chat with " << state.chat_target << ".\n";
    return true;
  }

  if (starts_with(line, error_prefix)) {
    const std::vector<std::string> words =
        split_words(line.substr(error_prefix.size()));

    if (words.size() >= 2U) {
      const ClientChatScope scope =
          words[0] == "PRIVATE" ? ClientChatScope::Private :
          words[0] == "GROUP" ? ClientChatScope::Group : ClientChatScope::None;

      // 过期响应不能清掉后来发起的另一个会话验证。
      if (state.chat_entry_pending && scope != ClientChatScope::None &&
          scope != state.pending_chat_scope) {
        std::cout << "[local warning] ignored stale chat-entry error.\n";
        return true;
      }
    }

    std::string reason = "chat target validation failed";
    if (words.size() >= 2U) {
      std::string decoded;
      std::string error;
      if (fileutil::percent_decode(words[1], decoded, error)) reason = decoded;
    }

    clear_pending_entry(state);
    std::cout << "[error] " << reason << '\n';
    return true;
  }

  return false;
}

bool begin_continuous_chat(ClientState &state, ClientChatScope expected_scope) {
  if (state.chat_entry_pending) {
    std::cout << "[local error] wait for the current 8/9 target validation result.\n";
    return false;
  }
  if (!has_active_chat(state) || state.chat_scope != expected_scope) {
    std::cout << "[local error] enter the matching chat session first with command "
              << (expected_scope == ClientChatScope::Private ? 8 : 9) << ".\n";
    return false;
  }

  state.chat_editor_active = true;
  state.chat_message_lines.clear();
  std::cout << "[multi-line] Continuous "
            << (expected_scope == ClientChatScope::Private ? "private" : "group")
            << " chat with " << state.chat_target
            << ". Type /send to send one message; /quit to leave the chat.\n";
  return true;
}

bool handle_chat_editor_input(TlsClientTransport &transport, ClientState &state,
                              const std::string &line) {
  if (!state.chat_editor_active) return true;

  if (line == "/quit") {
    state.chat_message_lines.clear();
    leave_chat_session(state, true);
    return true;
  }

  if (line != "/send") {
    state.chat_message_lines.push_back(line); // 空行也保留
    return true;
  }

  const std::string message = join_message_lines(state.chat_message_lines);
  if (message.empty()) {
    std::cout << "[local error] current message is empty; enter content first.\n";
    return true;
  }
  if (message.size() > kMaxEditableChatMessage) {
    std::cout << "[local error] message must be 1-1000 bytes after decoding; "
                 "current message is " << message.size() << " bytes.\n";
    return true;
  }

  const char *command =
      state.chat_scope == ClientChatScope::Private ? "MSG" : "GROUP_MSG";
  const std::string wire =
      std::string(command) + " " + state.chat_target + " " +
      encode_text_token(message) + "\n";

  if (!send_line(transport, wire)) return false;

  state.chat_message_lines.clear();
  std::cout << "[multi-line] sent; continue typing the next message, "
               "then /send again.\n";
  return true;
}

#pragma once
#include "client/client_state.hpp"
#include <string>

class TlsClientTransport;

bool has_active_chat(const ClientState &state);
bool chat_input_active(const ClientState &state);
void leave_chat_session(ClientState &state, bool print_notice = true);
void leave_chat_input_mode(ClientState &state, bool print_notice = true);

bool request_chat_entry(TlsClientTransport &transport, ClientState &state,
                        ClientChatScope scope, const std::string &target);
bool consume_chat_entry_response(const std::string &line, ClientState &state);

// 只有已经进入好友/群会话后，用户输入 6 才调用这里。
bool begin_chat_mode_selection(ClientState &state);
bool handle_chat_mode_selection_input(ClientState &state,
                                      const std::string &line);
bool handle_chat_message_input(TlsClientTransport &transport,
                               ClientState &state,
                               const std::string &line);

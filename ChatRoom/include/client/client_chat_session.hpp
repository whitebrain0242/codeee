#pragma once
#include "client/client_state.hpp"
#include <string>

class TlsClientTransport;

bool has_active_chat(const ClientState &state);
void leave_chat_session(ClientState &state, bool print_notice = true);

bool request_chat_entry(TlsClientTransport &transport, ClientState &state,
                        ClientChatScope scope, const std::string &target);
bool consume_chat_entry_response(const std::string &line, ClientState &state);

bool begin_continuous_chat(ClientState &state, ClientChatScope expected_scope);
bool handle_chat_editor_input(TlsClientTransport &transport, ClientState &state,
                              const std::string &line);

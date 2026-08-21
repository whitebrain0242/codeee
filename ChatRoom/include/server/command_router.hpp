#pragma once

#include "protocol.hpp"
#include "server/client_session.hpp"
#include "minimuduo/net/Callbacks.hpp"

#include <functional>
#include <string>
#include <unordered_map>

class ServerCommandRouter {
public:
    using TcpConnectionPtr =
        minimuduo::net::TcpConnectionPtr;

    using Handler =
        std::function<
            void(
                const TcpConnectionPtr&,
                ClientSession&,
                const Command&
            )
        >;

    void add(
        std::string command_name,
        Handler handler
    );

    bool dispatch(
        const TcpConnectionPtr& connection,
        ClientSession& session,
        const Command& command
    ) const;

private:
    std::unordered_map<
        std::string,
        Handler
    > handlers_;
};

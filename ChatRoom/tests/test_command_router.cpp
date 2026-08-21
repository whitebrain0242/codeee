#include "server/command_router.hpp"

#include <cstdlib>
#include <string>

int main() {
    ServerCommandRouter router;
    ClientSession session;

    bool called = false;

    router.add(
        "TEST",
        [&called](
            const minimuduo::net::
                TcpConnectionPtr&,
            ClientSession&,
            const Command& command
        ) {
            called =
                command.raw_arguments ==
                "payload";
        }
    );

    Command command;
    command.name = "TEST";
    command.raw_arguments =
        "payload";

    const bool dispatched =
        router.dispatch(
            {},
            session,
            command
        );

    Command unknown;
    unknown.name = "UNKNOWN";

    const bool unknown_dispatched =
        router.dispatch(
            {},
            session,
            unknown
        );

    return
        dispatched &&
        called &&
        !unknown_dispatched
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
}

#include "minimuduo/net/Buffer.hpp"

#include <cstdlib>
#include <string>

int main() {
    minimuduo::net::Buffer buffer(8);
    buffer.append("first\nsecond", 12U);

    const char* eol = buffer.findEOL();
    if (eol == nullptr) {
        return EXIT_FAILURE;
    }

    const std::string first = buffer.retrieveAsString(
        static_cast<std::size_t>(eol - buffer.peek()));
    if (first != "first") {
        return EXIT_FAILURE;
    }

    buffer.retrieve(1U);
    if (buffer.retrieveAllAsString() != "second") {
        return EXIT_FAILURE;
    }

    return buffer.readableBytes() == 0U
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

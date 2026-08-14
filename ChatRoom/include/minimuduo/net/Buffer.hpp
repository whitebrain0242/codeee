#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
//可动态扩展的字节缓冲区】
namespace minimuduo::net{
class Buffer{
public:
    static constexpr std::size_t kCheapPrepend = 8;//在缓冲区头部预留8个字节的空间
    static constexpr std::size_t kInitialSize = 1024;
    explicit Buffer(std::size_t initialSize = kInitialSize);

    std::size_t readableBytes() const noexcept;
    std::size_t writableBytes() const noexcept;
    std::size_t prependableBytes() const noexcept;

    const char* peek() const noexcept;
    char* beginWrite() noexcept;
    const char* beginWrite() const noexcept;

    void retrieve(std::size_t length);
    void retrieveUntil(const char* end);
    void retrieveAll() noexcept;
    std::string retrieveAsString(std::size_t length);
    std::string retrieveAllAsString();

    const char* findEOL() const noexcept;

    void ensureWritableBytes(std::size_t length);
    void hasWritten(std::size_t length);
    void append(const char* data, std::size_t length);
    void append(const std::string& data);

    ssize_t readFd(int fd, int* savedErrno);
    ssize_t writeFd(int fd, int* savedErrno);

private:
    char* begin() noexcept;
    const char* begin() const noexcept;
    void makeSpace(std::size_t length);

    std::vector<char> buffer_;
    std::size_t readerIndex_;
    std::size_t writerIndex_;
};

}

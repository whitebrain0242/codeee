#pragma once

#include "config.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <cstddef>
#include <string>

enum class TransportReadStatus {
    Data,
    Closed,
    Retry,
    Error
};

struct TransportReadResult {
    TransportReadStatus status =
        TransportReadStatus::Error;

    std::size_t bytes = 0;
    std::string error;
};

class TlsClientTransport {
public:
    TlsClientTransport() = default;
    ~TlsClientTransport();

    TlsClientTransport(
        const TlsClientTransport&
    ) = delete;

    TlsClientTransport& operator=(
        const TlsClientTransport&
    ) = delete;
    //建立TLS加密连接---执行TCP三次握手+TLS握手,验证服务端证书
    bool connect(
        const std::string& ip,//服务端IP
        int port,
        const TlsClientConfig& config,//TLS客户端配置
        std::string& error
    );
    //发送数据,将 data 通过 TLS 连接发送到服务端
    bool send(
        const std::string& data,
        std::string& error
    );
    //从 TLS 连接中读取最多 capacity 字节数据到 buffer 中
    TransportReadResult receive(
        char* buffer,
        std::size_t capacity//缓冲区的最大容量
    );
    //获取底层 TCP Socket 的文件描述符
    int fd() const noexcept;
    //获取底层 SSL 对象中待读取的数据字节数
    int pending() const noexcept;
    //获取当前使用的 TLS 协议版本
    std::string tls_version() const;
    //获取当前使用的加密套件名称
    std::string cipher_name() const;

    const std::string&
    //获取服务端证书中的身份标识
    peer_identity() const noexcept;
    //检查当前连接是否处于活跃状态
    bool connected() const noexcept;
    //主动关闭连接
    void shutdown();

private:
    minimuduo::net::TlsClientContext tls_context_;//TLS上下文
    minimuduo::net::SslPtr ssl_;//OPENSSL的SSL指针

    int socket_fd_ = -1;

    std::string peer_identity_;//服务端证书的身份标识
    bool connected_ = false;//连接状态
};

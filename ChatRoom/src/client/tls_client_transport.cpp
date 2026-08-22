#include "client/tls_client_transport.hpp"

#include "minimuduo/net/SocketOptions.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>

TlsClientTransport::~TlsClientTransport() { shutdown(); }
//建立TCP连接,接着建立TLS连接
bool TlsClientTransport::connect(const std::string &ip, int port,
                                 const TlsClientConfig &config,
                                 std::string &error) {
  shutdown();

  if (!tls_context_.initialize(config, error)) {
    return false;
  }

  socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

  if (socket_fd_ < 0) {
    error = "socket failed: " + std::string(std::strerror(errno));
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;

  address.sin_port = htons(static_cast<std::uint16_t>(port));

  if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
    error = "invalid IPv4 address";
    shutdown();
    return false;
  }

  if (::connect(socket_fd_, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    error = "connect failed: " + std::string(std::strerror(errno));

    shutdown();
    return false;
  }

  std::string keepalive_error;

  (void)minimuduo::net::configureTcpKeepAlive(socket_fd_, 60, 15, 3,
                                              keepalive_error);

  peer_identity_ = config.server_name.empty() ? ip : config.server_name;

  ssl_ = tls_context_.createSsl(socket_fd_, peer_identity_, error);

  if (!ssl_) {
    shutdown();
    return false;
  }

  if (!tls_context_.connectBlocking(ssl_.get(), error)) {
    shutdown();
    return false;
  }

  connected_ = true;
  return true;
}
//不使用kTLS:加密数据以及底层调用send发送到socket缓冲区
bool TlsClientTransport::send(const std::string &data, std::string &error) {
  if (!connected_ || !ssl_) {
    error = "TLS transport is not connected";
    return false;
  }

  std::size_t offset = 0U;

  while (offset < data.size()) {
    const int write_size = static_cast<int>(std::min<std::size_t>(
        data.size() - offset, static_cast<std::size_t>(INT_MAX)));

    ERR_clear_error();

    const int sent = SSL_write(ssl_.get(), data.data() + offset, write_size);

    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }

    const int ssl_error = SSL_get_error(ssl_.get(), sent);

    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      continue;
    }

    error = minimuduo::net::openssl_error_text("SSL_write");

    return false;
  }

  return true;
}

//检查当前TLS连接是否选择kTLS,支持零拷贝
bool TlsClientTransport::zero_copy_send_available() const noexcept {
  //OPENSSL版本大于3并且编译的时候没有禁用KTLS
#if OPENSSL_VERSION_NUMBER >= 0x30000000L &&             \
    !defined(OPENSSL_NO_KTLS)
  //如果连接未建立或者SSL对象为空
  if (!connected_ || !ssl_) {
    return false;
  }
  //获取SSL的写BIO(底层IO接口),BIO是OPENSSL的抽象IO层,这里指向TCPsocket
  BIO *write_bio = SSL_get_wbio(ssl_.get());
  //BIO存在且kTLS以协商并启用
  return write_bio != nullptr && BIO_get_ktls_send(write_bio) != 0;
#else
//不满足直接返回false
  return false;
#endif
}

//可以使用就是用KTLS,不可以就使用TLS,最后都可以发到socket缓冲区中
bool TlsClientTransport::send_file(const std::filesystem::path &path,//要发送的文件路径
                                   std::uint64_t offset,//偏移量
                                   std::uint64_t byte_count,//一共要发送的字节数
                                   bool &used_zero_copy, std::string &error) {
  used_zero_copy = false;

  if (!connected_ || !ssl_) {
    error = "TLS transport is not connected";
    return false;
  }

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);

  if (fd < 0) {
    error = "cannot open file for upload: " + std::string(std::strerror(errno));
    return false;
  }

  struct FdGuard {
    int fd = -1;
    ~FdGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } guard{fd};
//检查前置条件:linux系统+OPENSSL3.0以上版本,没有禁用KTLS
#if defined(__linux__) && OPENSSL_VERSION_NUMBER >= 0x30000000L &&             \
    !defined(OPENSSL_NO_KTLS)
  if (zero_copy_send_available()) {
    std::uint64_t sent_total = 0U;

    while (sent_total < byte_count) {
      const std::size_t request =
          static_cast<std::size_t>(std::min<std::uint64_t>(
              byte_count - sent_total, 1024ULL * 1024ULL * 1024ULL));

      ERR_clear_error();

      const int sent = SSL_sendfile(
          ssl_.get(), fd, static_cast<off_t>(offset + sent_total), request, 0);

      if (sent > 0) {
        sent_total += static_cast<std::uint64_t>(sent);
        continue;
      }

      const int ssl_error = SSL_get_error(ssl_.get(), sent);

      if (ssl_error == SSL_ERROR_WANT_READ ||
          ssl_error == SSL_ERROR_WANT_WRITE) {
        continue;
      }

      error = minimuduo::net::openssl_error_text("SSL_sendfile");
      return false;
    }

    used_zero_copy = true;
    return true;
  }
#endif

//如果kTLS用不了就使用TLS
  std::array<char, 256 * 1024> buffer{};

  std::uint64_t sent_total = 0U;

  while (sent_total < byte_count) {
    const std::size_t request = static_cast<std::size_t>(
        std::min<std::uint64_t>(byte_count - sent_total, buffer.size()));
    //从指定的offset读取数据,不改变指针位置
    const ssize_t read_count = ::pread(fd, buffer.data(), request,
                                       static_cast<off_t>(offset + sent_total));

    if (read_count <= 0) {
      error = read_count == 0
                  ? "file became shorter during upload"
                  : "pread failed: " + std::string(std::strerror(errno));
      return false;
    }

    std::size_t written = 0U;

    while (written < static_cast<std::size_t>(read_count)) {
      ERR_clear_error();
      //发送数据
      const int result = SSL_write(
          ssl_.get(), buffer.data() + written,
          static_cast<int>(static_cast<std::size_t>(read_count) - written));

      if (result > 0) {
        written += static_cast<std::size_t>(result);
        continue;
      }

      const int ssl_error = SSL_get_error(ssl_.get(), result);

      if (ssl_error == SSL_ERROR_WANT_READ ||
          ssl_error == SSL_ERROR_WANT_WRITE) {
        continue;
      }

      error = minimuduo::net::openssl_error_text("SSL_write(file)");
      return false;
    }

    sent_total += static_cast<std::uint64_t>(read_count);
  }

  return true;
}

TransportReadResult TlsClientTransport::receive(char *buffer,//存放接收到的解密后的明文数据
                                                std::size_t capacity) {//缓冲区大小
  TransportReadResult result;

  if (!connected_ || !ssl_ || buffer == nullptr || capacity == 0U) {
    result.status = TransportReadStatus::Error;

    result.error = "invalid TLS receive state";

    return result;
  }

  ERR_clear_error();
  //解密并且读数据到buffer
  const int received =
      SSL_read(ssl_.get(), buffer,
               static_cast<int>(std::min<std::size_t>(
                   capacity, static_cast<std::size_t>(INT_MAX))));

  if (received > 0) {
    result.status = TransportReadStatus::Data;//状态设置为有数据成功存入

    result.bytes = static_cast<std::size_t>(received);

    return result;
  }

  const int ssl_error = SSL_get_error(ssl_.get(), received);
  //连接被对端关闭
  if (ssl_error == SSL_ERROR_ZERO_RETURN) {
    connected_ = false;

    result.status = TransportReadStatus::Closed;

    return result;
  }
  //非阻塞下的重试
  if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
    result.status = TransportReadStatus::Retry;

    return result;
  }
  //真正的错误
  result.status = TransportReadStatus::Error;

  result.error = minimuduo::net::openssl_error_text("SSL_read");

  return result;
}

int TlsClientTransport::fd() const noexcept { return socket_fd_; }

int TlsClientTransport::pending() const noexcept {
  if (!ssl_) {
    return 0;
  }

  return SSL_pending(ssl_.get());
}

std::string TlsClientTransport::tls_version() const {
  if (!ssl_) {
    return {};
  }

  const char *value = SSL_get_version(ssl_.get());

  return value == nullptr ? std::string() : std::string(value);
}

std::string TlsClientTransport::cipher_name() const {
  if (!ssl_) {
    return {};
  }

  const char *value = SSL_get_cipher_name(ssl_.get());

  return value == nullptr ? std::string() : std::string(value);
}

const std::string &TlsClientTransport::peer_identity() const noexcept {//获取身份标识
  return peer_identity_;
}

bool TlsClientTransport::connected() const noexcept { return connected_; }

void TlsClientTransport::shutdown() {
  if (ssl_) {
    ERR_clear_error();
    (void)SSL_shutdown(ssl_.get());

    ssl_.reset();
  }

  if (socket_fd_ >= 0) {
    ::close(socket_fd_);
    socket_fd_ = -1;
  }

  connected_ = false;
  peer_identity_.clear();
}

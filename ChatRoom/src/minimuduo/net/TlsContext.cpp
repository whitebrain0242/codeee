#include "minimuduo/net/TlsContext.hpp"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include <array>
#include <sstream>

namespace minimuduo::net {

namespace {

bool isIpLiteral(
    const std::string &
        value) { // 客户端验证服务端的身份的时候，IP地址和域名都可以，域名使用DNS匹配
  std::array<unsigned char, 16> bytes{};
  // 尝试将字符串转换称二进制IP格式
  return ::inet_pton(AF_INET, value.c_str(), bytes.data()) == 1 ||
         ::inet_pton(AF_INET6, value.c_str(), bytes.data()) == 1;
}
// 配置通用参数--选择最低版本和最高版本
bool configureCommonContext(SSL_CTX *context, std::string &error) {
  if (context == nullptr) {
    error = "SSL_CTX is null";
    return false;
  }
  // 设置最版本是1.2
  if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1) {
    error = openssl_error_text("SSL_CTX_set_min_proto_version");
    return false;
  }

#ifdef TLS1_3_VERSION
  if (SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1) {
    error = openssl_error_text("SSL_CTX_set_max_proto_version");
    return false;
  }
#endif
  // 关闭SSL压缩功能：防止CRIME攻击
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);

#ifdef SSL_OP_ENABLE_KTLS
  // Keep TLS semantics while allowing Linux/OpenSSL to move eligible
  // file payloads with kernel TLS + SSL_sendfile zero-copy.
  SSL_CTX_set_options(context, SSL_OP_ENABLE_KTLS);
#endif
  return true;
}

} // namespace
// 自d定义删除器
void SslDeleter::operator()(SSL *ssl) const noexcept {
  if (ssl != nullptr) {
    SSL_free(ssl);
  }
}

TlsServerContext::~TlsServerContext() {
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
    context_ = nullptr;
  }
}

bool TlsServerContext::initialize(const TlsServerConfig &config,
                                  std::string &error) {
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
    context_ = nullptr;
  }

  if (!config.enabled) {
    error = "TLS server config has enabled=false; v8.4 requires TLS";
    return false;
  }

  context_ = SSL_CTX_new(TLS_server_method());
  if (context_ == nullptr) {
    error = openssl_error_text("SSL_CTX_new(TLS_server_method)");
    return false;
  }

  if (!configureCommonContext(context_, error)) {
    SSL_CTX_free(context_);
    context_ = nullptr;
    return false;
  }

  if (SSL_CTX_use_certificate_chain_file(
          context_, config.certificate_file.c_str()) != 1) {
    error = openssl_error_text("SSL_CTX_use_certificate_chain_file");
    SSL_CTX_free(context_);
    context_ = nullptr;
    return false;
  }

  if (SSL_CTX_use_PrivateKey_file(context_, config.private_key_file.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    error = openssl_error_text("SSL_CTX_use_PrivateKey_file");
    SSL_CTX_free(context_);
    context_ = nullptr;
    return false;
  }
  // 检查私钥是否与证书匹配
  if (SSL_CTX_check_private_key(context_) != 1) {
    error = openssl_error_text("SSL_CTX_check_private_key");
    SSL_CTX_free(context_);
    context_ = nullptr;
    return false;
  }
  // 设置，服务端不要求客户端提供证书
  SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);

  return true;
}

SslPtr TlsServerContext::createSsl(int socketFd, std::string &error) const {
  if (context_ == nullptr) {
    error = "TLS server context is not initialized";
    return {};
  }
  // 创建SSL对象，使用智能指针接管
  SslPtr ssl(SSL_new(context_));
  if (!ssl) {
    error = openssl_error_text("SSL_new");
    return {};
  }
  // 将socket文件描述副绑定SSl对象
  if (SSL_set_fd(ssl.get(), socketFd) != 1) {
    error = openssl_error_text("SSL_set_fd");
    return {};
  }

  SSL_set_accept_state(ssl.get());//服务端
  return ssl;
}

bool TlsServerContext::initialized() const noexcept {
  return context_ != nullptr;
}

TlsClientContext::~TlsClientContext() {
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
    context_ = nullptr;
  }
}

bool TlsClientContext::initialize(const TlsClientConfig &config,
                                  std::string &error) {
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
    context_ = nullptr;
  }

  if (!config.enabled) {
    error = "TLS client config has enabled=false; v8.4 requires TLS";
    return false;
  }

  context_ = SSL_CTX_new(TLS_client_method());
  if (context_ == nullptr) {
    error = openssl_error_text("SSL_CTX_new(TLS_client_method)");
    return false;
  }

  if (!configureCommonContext(context_, error)) {
    SSL_CTX_free(context_);
    context_ = nullptr;
    return false;
  }

  verify_peer_ = config.verify_peer;

  if (verify_peer_) {
    SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);

    if (config.ca_file.empty()) {
      error = "TLS client verify_peer=true requires ca_file";
      SSL_CTX_free(context_);
      context_ = nullptr;
      return false;
    }
    // 加载CA证书文件
    if (SSL_CTX_load_verify_locations(context_, config.ca_file.c_str(),
                                      nullptr) != 1) {
      error = openssl_error_text("SSL_CTX_load_verify_locations");
      SSL_CTX_free(context_);
      context_ = nullptr;
      return false;
    }
  } else { // 如果不需要验证，那么跳过验证
    SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
  }

  return true;
}

SslPtr TlsClientContext::createSsl(int socketFd,
                                   const std::string &peerIdentity,
                                   std::string &error) const {
  if (context_ == nullptr) {
    error = "TLS client context is not initialized";
    return {};
  }

  SslPtr ssl(SSL_new(context_));
  if (!ssl) {
    error = openssl_error_text("SSL_new");
    return {};
  }

  if (SSL_set_fd(ssl.get(), socketFd) != 1) {
    error = openssl_error_text("SSL_set_fd");
    return {};
  }

  if (verify_peer_) {
    if (peerIdentity.empty()) {
      error = "TLS peer identity cannot be empty when verification is enabled";
      return {};
    }
    // 获取 X509_VERIFY_PARAM 对象，用于设置证书验证参数
    X509_VERIFY_PARAM *parameters = SSL_get0_param(ssl.get());
    if (parameters == nullptr) {
      error = "SSL_get0_param returned null";
      return {};
    }

    if (isIpLiteral(peerIdentity)) { // 如果是IP地址
      if (X509_VERIFY_PARAM_set1_ip_asc(parameters, peerIdentity.c_str()) !=
          1) {
        error = "X509_VERIFY_PARAM_set1_ip_asc failed";
        return {};
      }
    } else { // 如果是域名
      if (SSL_set1_host(ssl.get(), peerIdentity.c_str()) !=
          1) { // 设置期望的域名，用于证书匹配
        error = "SSL_set1_host failed";
        return {};
      }
      // 设置 SNI（Server Name
      // Indication），告诉服务端，客户端想要访问哪一个域名
      if (SSL_set_tlsext_host_name(ssl.get(), peerIdentity.c_str()) != 1) {
        error = openssl_error_text("SSL_set_tlsext_host_name");
        return {};
      }
    }
  }

  SSL_set_connect_state(ssl.get());//客户短
  return ssl;
}

bool TlsClientContext::connectBlocking(
    SSL *ssl,
    std::string &error) const { // 执行阻塞式握手，要么握手完成要么出错
  if (ssl == nullptr) {
    error = "SSL is null";
    return false;
  }
  //客户端发起SSl握手
  if (SSL_connect(ssl) != 1) {
    error = openssl_error_text("SSL_connect");
    return false;
  }
  // 如果验证开启，调用，检查结果
  if (verify_peer_ && SSL_get_verify_result(ssl) != X509_V_OK) {
    error =
        "TLS peer certificate verification failed: " +
        std::string(X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
    return false;
  }

  return true;
}

bool TlsClientContext::initialized() const noexcept {
  return context_ != nullptr;
}

std::string openssl_error_text(const std::string &operation) {
  std::ostringstream output;
  output << operation;

  bool found = false;
  while (true) {
    const unsigned long code = ERR_get_error();
    if (code == 0UL) {
      break;
    }

    char buffer[256]{};
    ERR_error_string_n(code, buffer,
                       sizeof(buffer)); // 对每一个错误吗，都转换为可读字符串
    output << (found ? " | " : ": ") << buffer; // 第一个错误加:,后续加|
    found = true;
  }

  if (!found) { // 没有错误
    output << ": no OpenSSL error detail";
  }

  return output.str();
}

} // namespace minimuduo::net

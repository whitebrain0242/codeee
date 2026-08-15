#pragma once

#include <string>
//封装证书，密钥的位置
struct TlsServerConfig {
    bool enabled = true;

    std::string certificate_file =//服务器证书位置
        "config/tls/server.crt";

    std::string private_key_file =//服务器私钥位置
        "config/tls/server.key";
};

struct TlsClientConfig {
    bool enabled = true;//要不要TLS
    bool verify_peer = true;//客户端要不要验证服务端的TLS证书

    std::string ca_file =//CA证书
        "config/tls/ca.crt";

    std::string server_name;//要验证的服务器身份
};

# TLS通信加密

## OPENSSL对象

### SSL_CTX

```
SSL_CTX* ctx;
```

里面可以配置：

```
TLS版本
证书
私钥
CA
验证规则
密码套件
```

>   通常是一个TLS Server创建一个SSL_CTX

```
             SSL_CTX（共享
                │
       ┌────────┼────────┐
       │        │        │
      SSL      SSL      SSL（一个连接一个SSL
       │        │        │
     client1  client2  client3
```

## 流程

服务端

```c++
                    【服务端进程】

                   SSL_CTX
              TLS Server 配置
                     │
        ┌────────────┼────────────┐
        │            │            │
   server.crt   server.key    TLS配置
        │            │
        └────────────┘
                     │
                  SSL_new()
                     │
          ┌──────────┼──────────┐
          │          │          │
        SSL_A      SSL_B      SSL_C
          │          │          │
       Client A   Client B   Client C
          │          │          │
    accept_state accept_state accept_state
          │          │          │
     SSL_accept  SSL_accept  SSL_accept
```

客户端

```c++
                    【客户端进程】

                   SSL_CTX
              TLS Client 配置
                     │
             ┌───────┴───────┐
             │               │
           CA证书          TLS配置
             │
             ▼
          SSL_new()
             │
             ▼
            SSL
             │
     set_connect_state()
             │
             ▼
        SSL_connect()
             │
             ▼
          Server
```



### 代码实际流程

```c++
程序启动
   │
   ▼
ctx=SSL_CTX_new(TLS_server_method())//创建一个TLS Server
   │
   ▼
加载服务器证书
SSL_CTX_use_certificate_file()
   │
   ▼
加载服务器私钥
SSL_CTX_use_PrivateKey_file()
   │
   ▼
muduo TcpServer 启动
   │
   ▼
客户端连接 accept（）
   │
   ▼
创建 SSL
ssl=SSL_new(ctx)//创建一个TLS connection
   │
   ▼
SSL_set_fd(ssl,fd)//这个SSL对象使用这个TCP socket
SSL_set_accept_state(ssl);
   │
   ▼
TLS handshake
SSL_accept(ssl)//TLS Server握手
   │
   ▼
握手成功
   │
   ▼
SSL_read()
SSL_write()
   │
   ▼
业务处理
```

socket 的数据由 OpenSSL 消费，muduo 负责通知“socket 可读了”。

```
Server                       Client

SSL_accept()     <---->     SSL_connect()
```

tls_config.hpp是配置层
TlsContext是关于SSL的底层API调用封装
tls_client_transport是传输层,传输加密数据
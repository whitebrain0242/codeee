```
main.cpp
  │
  ├─ load_mysql_config()    → 读取 MySQL 连接信息（业务用）
  ├─ load_redis_config()     → 读取 Redis 连接信息（业务用）
  ├─ load_tls_server_config() → 读取 TLS 证书路径（配置给网络层）
  │
  └─ TcpServer 启动
       └─ setTlsContext(tlsContext)  ← 把 TLS 配置传给网络层
```

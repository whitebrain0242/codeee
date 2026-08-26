



###### 局域网IP

```
hostname -I
```

------

###### 安装全部依赖

直接执行：

```bash
sudo apt update
```

然后：

```bash
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  default-libmysqlclient-dev \
  libssl-dev \
  libhiredis-dev \
  libsqlite3-dev \
  protobuf-compiler \
  libprotobuf-dev \
  libspdlog-dev \
  redis-server \
  mysql-server \
  unzip \
  openssl
```

检查：

```bash
g++ --version
cmake --version
protoc --version
openssl version
redis-server --version
```

检查 spdlog：

```bash
pkg-config --modversion spdlog
```

如果这些都正常，再继续。

------

###### 启动 MySQL

启动：

```bash
mysql -u root -p
```

------

###### 初始化 MySQL 表

项目有 6 个 SQL 文件。

一次性执行：

```bash
SOURCE /home/white/Programming/code_c++/ChatRoom/sql/001_create_users.sql;
SOURCE /home/white/Programming/code_c++/ChatRoom/sql/002_create_friends_and_events.sql;
SOURCE /home/white/Programming/code_c++/ChatRoom/sql/003_create_messages.sql;
SOURCE /home/white/Programming/code_c++/ChatRoom/sql/004_create_groups_and_offline_delivery.sql;
SOURCE /home/white/Programming/code_c++/ChatRoom/sql/005_create_file_transfers.sql;
SOURCE /home/white/Programming/code_c++/ChatRoom/sql/006_create_friend_blocks.sql;
```

输入密码：

```text
xiyoulinux
```

检查表：

```bash
show tables;
```

应该至少看到：

```text
+----------------------------+
| Tables_in_chatroom         |
+----------------------------+
| chat_groups                |//群组信息
| file_transfer_deliveries   |//文件投递状态表
| file_transfers             |//存储文件传输元数据
| friend_blocks              |//存储拉黑名单
| friend_events              |//好友操作日志
| friend_requests            |//好友申请
| friendships                |//好友关系
| group_join_requests        |//进群申请
| group_members              |//群组成员
| group_message_deliveries   |//群组消息投递私聊表
| group_messages             |//群聊消息实体
| messages                   |//饲料消息实体
| private_message_deliveries |//私聊消息投递状态表
| users                      |//用户账号密码
+----------------------------+
```

```
EXIT;
```

##### 启动 Redis

执行：

```bash
sudo systemctl enable --now redis-server
```

检查：

```bash
sudo systemctl status redis-server
```

------

##### 准备配置文件

先创建必要目录：

```bash
mkdir -p data
mkdir -p data/server_files
mkdir -p downloads
mkdir -p logs
```



##### 生成 TLS 开发证书

先给脚本增加执行权限

```bash
chmod +x scripts/generate_dev_tls_cert.sh
```

执行脚本,传入服务端的IP

```bash
./scripts/generate_dev_tls_cert.sh 172.17.0.1 config/tls
```

然后：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```
##### cmake配置
如果成功，执行：

```bash
cmake --build build -j$(nproc)
```
##### 启动
```
./build/chat_server
./build/chat_client 10.30.0.119 9000
```

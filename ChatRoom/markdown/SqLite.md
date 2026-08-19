# SqLite
```
SqliteClient.cpp
├── 匿名命名空间（工具函数）
│   ├── StatementPtr：sqlite3_stmt 的 RAII 包装器（unique_ptr + 自定义 deleter）
│   ├── bind_text()：SQLite 参数绑定的辅助函数
│   └── sqlite_error()：获取 SQLite 错误信息
├── 生命周期管理
│   ├── ~SqliteClient()：析构时自动关闭数据库
│   ├── open()：打开数据库 + 自动创建目录 + 初始化表结构
│   ├── close_locked()：关闭数据库（必须在持有锁时调用）
│   └── initialize_schema()：建表 + 索引 + PRAGMA 设置
├── 消息存储（4 个函数）
│   ├── cache_private_message()：存私聊消息
│   ├── cache_group_message()：存群聊消息
│   ├── recent_private_messages()：查私聊历史
│   └── recent_group_messages()：查群聊历史
├── 文件归档（2 个函数）
│   ├── cache_file_transfer()：存已完成文件记录
│   └── recent_file_transfers()：查已完成的文件列表
├── 断点续传 - 上传（3 个函数）
│   ├── save_pending_upload()：保存待上传任务
│   ├── list_pending_uploads()：列出所有待上传任务
│   └── remove_pending_upload()：删除已完成/取消的上传任务
├── 断点续传 - 下载（3 个函数）
│   ├── save_partial_download()：保存部分下载进度
│   ├── get_partial_download()：查询某个下载进度
│   └── remove_partial_download()：删除已完成的下载进度
├── 统计工具（1 个函数）
│   └── stats()：返回本地缓存的统计信息
└── 底层工具（1 个函数）
    └── execute()：执行无返回值的 SQL
```


## 我们要使用它解决什么？

1.   历史消息存储
2.   大文件传输与下载

也就是**存储聊天记录**和实现**断点续传**功能

## 那么我们需要什么成员？

### 成员

1.   一个sqlite句柄
2.   操作的文件路径
3.   锁---确保线程安全

### 成员函数

1.   打开或者创建数据库，不存在就自动创建表---**`open(path, error)`**
2.   （写）保存聊天记录，收到服务器消息时，存进本地---**`cache_private_message(msg)`** 和 **`cache_group_message(msg)`**
3.   （读）查看聊天记录，用户发出命令时，翻出聊天记录---**`recent_private_messages(account, peer, count)`** 和 **`recent_group_messages(account, group, count)`**
4.   断点续传-上传，当上传文件中途下线过后，仍然记录文件传输---**`save_pending_upload(upload)`**（存任务）、**`list_pending_uploads(account)`**（查任务）、**`remove_pending_upload(token)`**（删任务）
5.   断点续传-下载，当文件下载到一半时下线，客户端可以从上次下载处继续下载---**`save_partial_download(download)`**（存进度）、**`get_partial_download(account, transfer_id)`**（查进度）、**`remove_partial_download(account, transfer_id)`**（删进度）
6.   记录已经完成的文件（避免重复）---**`cache_file_transfer(file)`**（存记录）、**`recent_file_transfers(account, count)`**（查记录）
7.   快速查看本地缓存状况，客户端输入命令查看本地储存多少消息，多少文件---**`stats(account, stats)`**

## 断点续传

之前错误的以为，断点续传就是记录一个偏移量的事情 ，但是没有这么简单。如果只记录偏移量，那么一旦**文件被修改、服务器重启、或临时文件丢失**，整个传输就废掉了

我们使用断点续传的目标是：**在任意一方宕机/断网后，双方能握手确认“你那边还剩多少没发？”，然后从那个精确的字节处继续，且确保最终文件完整无误**

### 上传

```c
客户端                                          服务器
  |                                              |
  | ① FILE_BEGIN (token, 元数据)                 |
  |--------------------------------------------->|
  |                                              | ● 检查 tmp/token.part 是否存在
  |                                              | ● 若存在，读磁盘大小 => offset
  |                                              | ● 若不存在，创建空 .part 和 .resume.pb
  | ② FILE_READY (token, offset)                 |
  |<---------------------------------------------|
  |                                              |
  | ● 用 seekg(offset) 跳到本地文件指定位置       |
  |                                              |
  | ③ FILE_CHUNK (token, offset, 数据块)         |
  |--------------------------------------------->| ● 追加写入 .part
  | ③ ... (循环几百次) ...                       |
  |--------------------------------------------->|
  |                                              |
  | ④ FILE_END (token)                           |
  |--------------------------------------------->| ● SHA256 校验
  |                                              | ● rename(.part → files/xxx)
  | ⑤ FILE_UPLOAD_OK (token)                     | ● 删除 .resume.pb
  |<---------------------------------------------|
```
① 你在客户端敲 SEND_FILE Bob ubuntu.iso。

客户端计算文件 SHA256 和大小，生成唯一的 transfer_token（如 a1b2c3...）。

客户端调用 SqliteClient::save_pending_upload，把 token、source_path、file_size、sha256_hex 存入本地 SQLite。

客户端发送 FILE_BEGIN_PRIVATE a1b2c3... Bob ubuntu.iso 1073741824 <sha256>


② 服务器收到后，在 data/server_files/tmp/ 目录下创建两个文件：

a1b2c3...part（空的，准备接收字节流）。

a1b2c3...resume.pb（Protocol Buffers 二进制文件，里面存储了 FileUploadResumeState，包含完整的元数据：发送者、接收者、文件名、总大小、SHA256）。

服务器回复 FILE_READY a1b2c3... 0（意思是：我从偏移量 0 开始等你）。


③ 客户端循环读取本地文件，每次发 FILE_CHUNK a1b2c3... <offset> <base64数据块>。

服务器每收到一块，就追加写入 .part 文件，并更新内存中的偏移量。

注意：服务器不会每收到一块就去更新 .resume.pb（磁盘 IO 太重）。.resume.pb 只在传输开始时创建，在传输结束时删除。偏移量存在内存里。


④ **客户端WIFI断开时**
服务器内存中的偏移量（600MB）消失了。

客户端进程退出了，内存里的信息也没了。

但是：

服务器磁盘上有一个 600MB 的 .part 文件（真实字节）。

服务器磁盘上有一个 .resume.pb 文件（记录了元数据，但没有记录偏移量）。

客户端 SQLite 里有一条 pending_upload 记录（记录了 source_path 和 token）。

**客户端重新连接上后**

客户端重启，登录成功，自动调用 SqliteClient::list_pending_uploads()，发现有 ubuntu.iso 没传完。

客户端再次发送 FILE_BEGIN_PRIVATE a1b2c3...（同样的 token，同样的元数据）。

服务器收到后，发现 a1b2c3...part 已经存在。它不会去读 .resume.pb 里的偏移量（因为没存），而是直接调用系统 API：std::filesystem::file_size(part_path)，得到 600MB。

服务器核对请求中的元数据（大小、SHA256）是否与 .resume.pb 中一致。一致则回复 FILE_READY a1b2c3... 629145600（告诉客户端，我已经有了 600MB，从 600MB 开始发）。

客户端收到 FILE_READY 里的 629145600，用 seekg(629145600) 跳到本地文件的 600MB 处，继续发送剩余的 400MB。


⑤ 客户端发完最后一块，发 FILE_END a1b2c3...。

服务器收到后，立刻做两件事：

校验：计算 .part 的 SHA256，对比请求中的 SHA256，确保文件完整。
原子移动：把 .part 重命名为 data/server_files/files/a1b2c3..._ubuntu.iso（移出临时目录）。
删除 .resume.pb 文件。
服务器回复 FILE_UPLOAD_OK a1b2c3... 999（传输完成）。

客户端收到后，调用 SqliteClient::remove_pending_upload(token)，清除本地任务。




### 下载

```c
客户端                                          服务器
  |                                              |
  | ① FILE_OFFER (transfer_id, 元数据)            |
  |<---------------------------------------------|
  |                                              |
  | ● 创建 id_file.part                           |
  | ● 若存在，读磁盘大小 => offset                |
  |                                              |
  | ② FILE_RESUME_REQUEST (id, offset)           |
  |--------------------------------------------->| ● 用 seekg(offset) 跳到文件指定位置
  |                                              |
  | ③ FILE_DATA (id, offset, 数据块)             |
  |<---------------------------------------------| ● 追加写入 .part
  | ③ ... (循环几百次) ...                       |
  |<---------------------------------------------|
  |                                              |
  | ④ FILE_DONE (id)                             |
  |<---------------------------------------------|
  | ● SHA256 校验                                 |
  | ● rename(.part → 最终文件名)                  |
  |                                              |
  | ⑤ FILE_RECEIVED (id, sha256)                 |
  |--------------------------------------------->| ● 更新 MySQL delivered_at
```


① Alice 上传成功后，服务器生成一个 transfer_id（如 888）。

如果你在线，服务器直接发 FILE_OFFER 888 PRIVATE Alice - ubuntu.iso 1073741824 <sha256>。

如果你不在线，这条 FILE_OFFER 会等到你下次登录时，由 ChatServer::deliver_pending_files() 推送。


② 你的客户端收到 FILE_OFFER，在 download_root/alice/ 目录下创建 888_ubuntu.iso.part（空文件）。

调用 SqliteClient::save_partial_download，存入 transfer_id=888、temp_path=...part、file_size=1GB、sha256_hex。

客户端发送 FILE_RESUME_REQUEST 888 0（从 0 开始要数据）。


③ 服务器收到请求，开启 ifstream 读取磁盘上的 files/...iso。

循环发 FILE_DATA 888 <offset> <base64块>。

客户端收到后，追加写入 .part 文

**客户端蓝屏**
磁盘上的 888_ubuntu.iso.part 顽强地保留了 600MB 数据。

SQLite 里的 partial_downloads 表仍存着这条记录。
**客户端重启**
客户端重启登录，没有收到任何主动推送。

你输入 PENDING 命令，客户端向服务器请求所有未完成的文件。

服务器发现 transfer_id=888 还没有 delivered_at 时间戳，于是再次推送 FILE_OFFER。

你的客户端收到 FILE_OFFER，先调用 SqliteClient::get_partial_download(888)，查到本地 temp_path。

调用 std::filesystem::file_size(temp_path)，得到 600MB。

客户端发送 FILE_RESUME_REQUEST 888 629145600（告诉服务器，我已经有 600MB 了）。

服务器收到 629145600，用 seekg(629145600) 跳到文件 600MB 处，继续发送 FILE_DATA


④ 服务器发完最后一块，发 FILE_DONE 888。

客户端收到后，校验 SHA256（对比 .part 和 FILE_OFFER 里的值）。

校验通过后，执行原子操作：rename(888_ubuntu.iso.part, 888_ubuntu.iso)（移除 .part 后缀）。

调用 SqliteClient::remove_partial_download(888) 清理数据库。

⑤ 发送 FILE_RECEIVED 888 <sha256> 给服务器。

服务器收到后，更新 MySQL 的 file_transfer_deliveries 表，标记为 delivered_at = now

### 总结
1. 偏移量不是一个单纯的数字，而是使用file_size()获取已经发送或者接受的文件大小
2. Token：是什么？一个32 位十六进制随机字符串（由 OpenSSL 的 RAND_bytes 生成
          干什么？用在客户端上传阶段，用户敲下 SEND_FILE 的那一刻生成
          在哪里？在客户端的`pending_uploads` 表，服务端的临时文件名字中
          作用？重连之后，客户端上传这个文件时，直接查找目录中有没有这个文件名，有就接着传，这是找回临时文件的唯一索引
3. TransferID：
          是什么？一个 MySQL 自增的 64 位整数
          干什么？客户端下载阶段，服务器在 FILE_END 校验通过后，插入 MySQL file_transfers 表时生成，MySQL 自动分配一个永不重复的数字 ID。
          在哪里？服务端的存 MySQL file_transfers 表，客户端的file_transfers 表和 partial_downloads 表里
          作用？是服务器定位正式文件的索引
4. 为什么不用一个呢？还要在上传和下载的时候专门分两个？
   Token解决了多人同时上传同名文件：
   因为上传到服务端的文件名不是依靠客户端上传的文件名字的，而是靠生成的随即Token字符串来上传
   TransferID解决了一个文件被多人下载：
   多个人下载同一个文件的时候不会拷贝多个数量的文件（每一个人一份），硬盘撑饱
   但是有ID之后，服务器只会保存一份物理文件，在MySql的表中记录
      当 Bob 发来 FILE_RESUME_REQUEST 123 0（我要下 ID 为 123 的文件），服务器查表，确认 Bob 是合法接收者。
   
      服务器不需要复制文件，直接读取磁盘上唯一的 files/123_movie.mp4，用 TCP 流发送给 Bob。

      当 Bob 发来 FILE_RECEIVED 123 ...，
      服务器执行 UPDATE file_transfer_deliveries SET delivered_at=NOW() WHERE transfer_id=123 AND recipient='Bob'，
      也就是在表中把投递时间改成此时此刻，只改这个用户的这一行
      下次谁问“Bob 下完没有”，查这一行就知道，没有下载的话就是NULL























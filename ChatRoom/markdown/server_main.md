## 结构

```
main()
 │
 ├── tls_context
 │
 ├── database
 │
 ├── redis
 │
 ├── main_loop
 │
 ├── tcp_server
 │       │
 │       └── tls_context
 │
 └── chat_server
         ├── tcp_server
         ├── database
         ├── redis
         ├── presence TTL
         └── file_storage_root
```



## 启动顺序

```
配置
 ↓
TLS
 ↓
MySQL
 ↓
Redis
 ↓
EventLoop
 ↓
TcpServer
 ↓
ChatServer
 ↓
tcp_server.start()
 ↓
main_loop.loop()
```

我们首先是是初始化Mysql,Redis,Tcpserver之类的,为什么我们不直接全部装进Chatserver呢?

如果我们把任意一个初始化交给Chatserver,那么chatserver的任务会很重

所以一般的设计就是main复制组装系统,而ChatServer负责聊天业务



## 多服务器

### **我们创建了服务器ID,为什么要创建呢?**

#### 断点续传

主要是为了REdis而服务的,就比如说在断电续传的时候:

假如一共要传输100G的数据,服务器A受到了50G,但是因为网络抖动,所以客户端重连,此时客户端又被分到了服务器B,服务器B搜索发现已经存了50G了,就会从50G开始储存,但是服务器B有前面的50G数据吗???

答案是没有,数据在服务器A上

我们怎么解决呢?

在Redis中我们会储存进度和服务器ID,当服务器B接受到客户端重新连接的时候,回去查REDIS,保存的服务器ID和我的ID一致吗?只有ID对的上,才允许继续传

#### 心跳检测

客户端要向自己对应的服务器ID发送PING,服务器恢复PONG,如果长时间没有收到的话,就会主动断开与这个客户端的连接,销毁connection对象

#### 在线状态

用户A连接服务器A,用户B连接服务器B,而服务器A无法直接获取服务器B的在线状态,所以都把在线状态写在Redis上面,A去Redis查找就好了


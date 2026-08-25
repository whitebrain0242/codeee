# 心跳检测

## 心跳检测是什么?

### 为什么需要?

心跳检测主要解决 **TCP 协议“长连接”的三大致命痛点**：

-   **死连接检测（保活）**：如果客户端突然断电、拔网线或进程崩溃，它无法发送 FIN 包。操作系统内核可能要等数小时（TCP Keep-Alive 默认 7200 秒）才发现。心跳（60秒超时）能让服务器**快速回收**这些僵死连接的文件描述符和内存资源。
-   **NAT/防火墙保活（穿透）**：中间路由器或防火墙为了节省资源，会清理长时间没有数据包传输的 UDP/TCP 映射表。心跳包作为“合法业务数据”，能**刷新路由表**，防止连接被中间设备踢掉。
-   **应用层存活（假死检测）**：TCP 连接可能正常，但**服务器业务线程死锁或卡顿**,内核 Keep-Alive 探测包（操作系统的）依然能正常 ACK，而应用层的心跳（PING/PONG）**必须经过业务线程处理**，能准确检测到“服务假死”。

### 方向

心跳检测有很多方向:最常见的是客户端向服务端发送PING,服务端恢复PONG

还有就是双向心跳检测

**那么这三种有什么不同的使用场景呢?**

1.   客户端主动:适用于**互联网高并发**,有海量设备,服务器无需为每一个连接维护定时器扫描,压力低
2.   服务端手动:适用于**连接数量可控的系统**,服务器掌握绝对的控制权,能强制踢掉异常的客户端
3.   双向心跳:**金融交易,工业自动化**,容忍任一方网络故障,确保故障发现时间

### 实现

##### 前提

##### socket option

```c++
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
```

###### level

-   **`SOL_SOCKET`**设置socket选项
-   **`IPPROTO_TCP`**设置TCP选项



###### optname

对于socket选项

-   **`SO_KEEPALIVE`**：保活功能的总开关
-   **`TCP_KEEPIDLE`**：连接空闲多少秒后开始第一次探测
-   **`TCP_KEEPINTVL`**：每次探测的间隔秒数
-   **`TCP_KEEPCNT`**：连续探测失败多少次后判定连接死亡

###### optval

它是一个**内存地址**，指向你要设置的数据

因为设置的是整数（开/关，秒数），所以传的是 `int` 变量的地址（如 `&value`）。如果设置的是复杂结构体（比如超时时间 `struct timeval`），就传结构体指针。

###### optlen

`optval` 指向的数据占用多少个字节



##### 步骤

```c
if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        std::cerr << "setsockopt(SO_KEEPALIVE) failed: " << strerror(errno) << std::endl;
        return false;
    }
```

开启保活标志

仅仅开启这一步，系统会使用默认的“极保守”参数（Linux 默认空闲 7200 秒即 **2小时** 后才开始探测，且只探测 9 次）

```c#
if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_sec, sizeof(idle_sec)) < 0) {
        std::cerr << "setsockopt(TCP_KEEPIDLE) failed: " << strerror(errno) << std::endl;
        return false;
    }
```

设置空闲等待时间idle_sec,如果在这个时间中,没有任何操作,此时就会发送一个心跳包

注意这里是TCP设置选项

```c#
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_sec, sizeof(interval_sec)) < 0) {
        std::cerr << "setsockopt(TCP_KEEPINTVL) failed: " << strerror(errno) << std::endl;
        return false;
    }
```

设置每一个探测包的间隔时间,如果第一次发送没有受到ACK,那么隔这个时间再发一次

```c#
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) {
        std::cerr << "setsockopt(TCP_KEEPCNT) failed: " << strerror(errno) << std::endl;
        return false;
    }
```

设置对打探测次数,如果连续发count次探测包都没有相应,那么判断连接死亡

###### 对于错误

虽然是交给了操作系统的内核去处理,但是呢?我们还是要处里报错的

当心跳检测出现问题时,内核会在这个socket的error上打上标记,所以在下一次系统调用的时候就可以检测出来(recv,send)

当然我们也要及时使用系统调用,才能发现问题
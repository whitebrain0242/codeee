#pragma once

#include "minimuduo/net/NonCopyable.hpp"

#include <chrono>
#include <memory>
#include <vector>

struct epoll_event;

namespace minimuduo::net {

class Channel;
class EventLoop;
//不能被继承
class Poller final : private NonCopyable {
public:
    using ChannelList = std::vector<Channel*>;//输出参数，存放活跃事件的channel列表

    explicit Poller(EventLoop* loop);
    ~Poller();
    //等待超时时间：如果timeout是0,表示不阻塞，负数阻塞            //输出参数
    void poll(std::chrono::milliseconds timeout, ChannelList* activeChannels);//epollwait
    void updateChannel(Channel* channel);//添加或修改一个 Channel 的监听事件
    void removeChannel(Channel* channel);//从 epoll 中永久移除一个 Channel

private:
    void fillActiveChannels(int eventCount, ChannelList* activeChannels) const;//将epoll返回的事件数组events转换成channel列表
    void update(int operation, Channel* channel);//epoll——ctl
    //channel状态
    static constexpr int kNew = -1;//未注册
    static constexpr int kAdded = 1;//已注册
    static constexpr int kDeleted = 2;//已删除

    EventLoop* ownerLoop_;
    int epollFd_;
    std::vector<epoll_event> events_;//存储epollwait返回的事件数组
};

}  // namespace minimuduo::net

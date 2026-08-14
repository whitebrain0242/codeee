#pragma once

#include "minimuduo/net/NonCopyable.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace minimuduo::net {

class EventLoop;

class Channel final : private NonCopyable {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void handleEvent();

    void setReadCallback(EventCallback callback);
    void setWriteCallback(EventCallback callback);
    void setCloseCallback(EventCallback callback);
    void setErrorCallback(EventCallback callback);

    void tie(const std::shared_ptr<void>& owner);

    int fd() const noexcept;
    std::uint32_t events() const noexcept;
    void setRevents(std::uint32_t revents) noexcept;
    bool isNoneEvent() const noexcept;
    bool isWriting() const noexcept;

    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();
    void remove();

    int index() const noexcept;
    void setIndex(int index) noexcept;
    EventLoop* ownerLoop() noexcept;

private:
    void update();
    void handleEventWithGuard();

    static constexpr std::uint32_t kNoneEvent = 0;
    static constexpr std::uint32_t kReadEvent = 0x001U | 0x2000U;  // EPOLLIN | EPOLLRDHUP
    static constexpr std::uint32_t kWriteEvent = 0x004U;           // EPOLLOUT

    EventLoop* loop_;
    const int fd_;// 被监听的文件描述符
    std::uint32_t events_;
    std::uint32_t revents_;
    int index_;//在Poller中的状态，1是已添加删除操作，-1未添加，0是修改操作

    bool tied_;//是否与外部对象绑定-生命周期管理
    std::weak_ptr<void> tie_;//弱引用，指向绑定的tcpconnection

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

}  // namespace minimuduo::net

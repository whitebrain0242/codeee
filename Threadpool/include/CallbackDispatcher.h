#ifndef CALLBACK_DISPATCHER_H
#define CALLBACK_DISPATCHER_H

#include <queue>
#include <mutex>
#include <functional>

// ============================================================
// 2. 回调调度器 CallbackDispatcher
// ============================================================

class CallbackDispatcher//
{
public:
//提交一段要执行的代码，放进回调队列
    void post(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.push(std::move(callback));//放回回调队列
    }

    void processAll()
    {//主线程统一执行所有回调
        //创建一个空队列
        std::queue<std::function<void()>> localCallbacks;

        {//一次性 把所有回调交换到本地队列，交换很快不会卡住
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(localCallbacks, callbacks_);
        }

        //执行每一个回调，执行完就丢掉
        while (!localCallbacks.empty())
        {
            localCallbacks.front()();
            localCallbacks.pop();
        }
    }

    //看看回调数列是不是空的
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return callbacks_.empty();//true空的
    }

private:
    mutable std::mutex mutex_;//互斥锁
    std::queue<std::function<void()>> callbacks_;//回调函数队列:任务做完先不打印存能在这里，等主线程有空了在打印
};

#endif
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stdexcept>


// ============================================================
// 1. 通用线程池 ThreadPool
// ============================================================

class ThreadPool
{
public:
    // 传入参数是要创建多少个人工线程来执行任务，如果传入0则默认创建1个线程。
    // 构造函数会启动这些线程，并让它们进入工作循环，等待任务的到来。
    explicit ThreadPool(std::size_t threadCount)
        // 无符号整数类型，冒号是初始化赋值
        : accepting_(true), // 可以接受新任务
          stopping_(false), // 没有停止
          activeTasks_(0)
    { // 当前没有正在跑的任务
        if (threadCount == 0)
        {
            threadCount = 1; // 默认一个线程
        }
        // 创建线程并让它们进入工作循环workLoop
        // 配合threadcount，依旧是无符号整数类型
        for (std::size_t i = 0; i < threadCount; ++i)
        {
            workers_.emplace_back([this, i]()
                                  { workerLoop(i); });
        }
    }

    // 线程池不能拷贝，感觉是直接把一个线程池的地址传过去了，但是线程池不能串
    ThreadPool(const ThreadPool &) = delete;
    // 禁止线程池之间赋值
    ThreadPool &operator=(const ThreadPool &) = delete;

    //~是析构函数，类对象生命周期结束时自动调用，负责清理资源
    // 程序自动关闭，防止内存泄漏，调用sutdown关闭销毁
    ~ThreadPool()
    {
        shutdown();
    }

    //模板：支持任何函数和任意数量和类型的参数
    template <typename F, typename... Args>
    //函数：传入函数f+参数args，返回future结果
    auto submit(F &&f, Args &&...args)//&&是万能引用，不拷贝数据效率最高
        //返回值类型：future是未来拿结果的，获取函数F的返回值类型
        -> std::future<std::invoke_result_t<F, Args...>>
    {

        //获取函数返回值类型
        using ReturnType = std::invoke_result_t<F, Args...>;

        //把函数和参数绑定在一起，形成一个不用传参数就能直接跑的任务（线程只认识没有参数的函数）
        auto boundTask = std::bind(
            std::forward<F>(f),//函数
            std::forward<Args>(args)...);//参数
        //把打包好的任务，包装成线程安全能拿返回值，共享的任务指针
        auto taskPtr = std::make_shared<std::packaged_task<ReturnType()>>(
            //里面 任务包装器：1.跑任务2.存返回值给future使用
            //外面 创建一个智能指针，让认为在线程之间安全1的共享
            //最外面 指向包装好的任务·的智能指针，方便在线程池里管理和执行任务
            std::move(boundTask));//直接转移任务而不是拷贝

            //把结果存在result里面
        std::future<ReturnType> result = taskPtr->get_future();


        //把任务放进任务队列需要上锁，因为任务队列不能同时被多个人修改
        //括号结束锁就释放
        {//上锁
            std::lock_guard<std::mutex> lock(mutex_);

            if (!accepting_)//不能接受新任务了
            {//报错
                throw std::runtime_error("ThreadPool is stopping. Cannot submit new task.");
            }

            //结束判断：把任务放进任务队列，线程池的工作线程会从这个队列里取任务来执行
        
            tasks_.emplace([taskPtr]()
                           { (*taskPtr)(); });
        }//解锁

        //叫醒一个线程完成任务
        //条件变量是工具 只通知一个
        taskCondition_.notify_one();

        return result;
    }

    void waitAll()//等所有任务跑完
    {
        //上锁，使用wait必须要它
        std::unique_lock<std::mutex> lock(mutex_);

        //等的时候解锁lock让别的线程能拿到锁来修改任务队列和activeTasks_
        //等条件满足了再继续往下走，并且阻塞等待，醒来工作的时候自动上锁
        finishedCondition_.wait(lock, [this]()
                                { return tasks_.empty() && activeTasks_ == 0; });
    }

    // 程序关闭，防止泄露
    void shutdown()
    {
        {//上锁
            std::lock_guard<std::mutex> lock(mutex_);

            //安全修改线程池状态，先判断如果已经在停止了就直接返回，避免重复操作
            if (stopping_)
            {
                return;
            }

            //不接受新任务，标记线程池关闭
            accepting_ = false;
            stopping_ = true;
        }

        //叫醒所有线程，让它们知道要停止了，
        taskCondition_.notify_all();

        //完成剩余任务后退出，遍历所有线程，挨个等待结束
        for (std::thread &worker : workers_)
        {//检查这个线程是否可被等待
            //如果线程还在工作就join等待它结束，join会阻塞当前线程直到worker线程结束 
            if (worker.joinable())
            {
                //主线程堵塞等子线程跑完
                worker.join();
            }
        }
    }

private:
//每个线程一直要重复做的事情，传入的参数是线程id
    void workerLoop(std::size_t workerId)
    {
        while (true)//无线循环
        {
            std::function<void()> task;//定义一个任务变量，类型是无参无返回的函数，准备待会处理的任务

            //先取出任务
            {
                std::unique_lock<std::mutex> lock(mutex_);

                taskCondition_.wait(lock, [this]()
                                    { return stopping_ || !tasks_.empty(); });

                if (stopping_ && tasks_.empty())
                {
                    return;
                }

                //取出一个任务来执行，
                //先把任务从队列里拿出来，
                //然后把activeTasks_加1表示有一个正在跑的任务了
                task = std::move(tasks_.front());
                tasks_.pop();
                ++activeTasks_;
            }

            //在执行任务
            try//试一试执行
            {
                task();
            }
            catch (const std::exception &e)//出错了的话接住异常，打印错误信息
            {//如果是矩阵为空
                std::cerr << "[Worker " << workerId << "] Task exception: "
                          << e.what() << std::endl;
            }
            catch (...)
            {//如果是其他任何未知错误
                std::cerr << "[Worker " << workerId << "] Unknown task exception."
                          << std::endl;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);

                --activeTasks_;//计数减一
                //没有代做的任务了或者没有人干活了-》所有任务彻底完了
                if (tasks_.empty() && activeTasks_ == 0)
                {
                    //通知老板说所有任务都完成了
                    finishedCondition_.notify_all();
                }
            }
        }
    }

private:
    std::vector<std::thread> workers_;//存放所有线程的列表
    std::queue<std::function<void()>> tasks_;//任务列表，代办

    std::mutex mutex_;//互斥锁
    std::condition_variable taskCondition_;//条件变量
    std::condition_variable finishedCondition_;//完成的条件变量

    bool accepting_;//是否接受新任务
    bool stopping_;//是否正在停止
    std::size_t activeTasks_;//正在跑的任务数量
};

#endif
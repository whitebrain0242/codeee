#ifndef MATRIX_TASK_SCHEDULER_H
#define MATRIX_TASK_SCHEDULER_H

#include "ThreadPool.h"
#include "CallbackDispatcher.h"
#include <vector>
#include <future>
#include <string>
#include <iostream>

using Matrix = std::vector<std::vector<long long>>;
Matrix multiplyMatrix(const Matrix &a, const Matrix &b);

void printMatrix(
    const Matrix &matrix,
    std::size_t maxRows = 4,
    std::size_t maxCols = 4);


// ============================================================
// 4. 矩阵任务调度程序 MatrixTaskScheduler
// ============================================================

// 负责并发跑+结果演示
class MatrixTaskScheduler
{
public: // 函数声明
    // 1.创建一个线程池，线程数量由参数指定
    // 名字和类一样的是构造函数
    // explicit修饰表示只能通过显式调用来创建对象，防止隐式转换导致的错误。
    explicit MatrixTaskScheduler(std::size_t threadCount)
        : pool_(threadCount) {} // 创建线程池
    // 2.普通函数
    std::future<void> addMatrixMultiplyTask(
        int taskId,
        Matrix a,
        Matrix b)
    {
        return pool_.submit(
            [this, taskId, a = std::move(a), b = std::move(b)]() mutable//允许在任务里修改矩阵
            {//计算任务开始时间
                auto start = std::chrono::steady_clock::now();

                //执行矩阵乘法
                Matrix result = multiplyMatrix(a, b);

                //计算耗时多少毫秒
                auto end = std::chrono::steady_clock::now();

                auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  end - start)
                                  .count();

                //获取当前执行任务的线程id
                std::thread::id workerThreadId = std::this_thread::get_id();

                //把结果和信息放进回调队列，等主线程有空了再打印
                callbacks_.post(
                    [taskId, result = std::move(result), costMs, workerThreadId]() mutable
                    {
                        std::cout << "\n[Callback] Matrix task " << taskId
                                  << " finished.\n";
                        std::cout << "Executed by worker thread: "
                                  << workerThreadId << "\n";
                        std::cout << "Cost: " << costMs << " ms\n";
                        std::cout << "Result matrix preview:\n";
                        printMatrix(result);
                    });
            });
    }

    void waitAll()
    {
        pool_.waitAll();
    }

    void processCallbacks()
    {
        callbacks_.processAll();
    }

    void shutdown()
    {
        pool_.shutdown();
    }

private: // 变量
    ThreadPool pool_;//创建一个线程池：用来执行矩阵乘法任务
    CallbackDispatcher callbacks_;
};

// ============================================================
// 3. Without Network：矩阵乘法任务
// ============================================================
//using起别名，Matrix是一个二维的长整型向量，也就是矩阵
using Matrix = std::vector<std::vector<long long>>;

void validateMatrix(const Matrix &matrix, const std::string &name)
{//验证矩阵：输入：矩阵和名字，作用：检查矩阵是不是正常
    if (matrix.empty())
    {
        throw std::runtime_error(name + " cannot be empty.");
    }

    if (matrix[0].empty())
    {
        throw std::runtime_error(name + " cannot have empty row.");
    }

    std::size_t columnCount = matrix[0].size();//记住第一行有多少个列

    for (std::size_t i = 1; i < matrix.size(); ++i)//遍历每一行
    {
        //所有行的长度必须一样
        if (matrix[i].size() != columnCount)//要是不和第一行相同
        {
            throw std::runtime_error(name + " is not a rectangular matrix.");
        }
    }
}
//生成矩阵：几行几列还有种子
Matrix generateMatrix(std::size_t rows, std::size_t cols, long long seed)
{
    Matrix matrix(rows, std::vector<long long>(cols));//创建一个空矩阵，全是0

    long long value = seed;//从种子数字开始，
    //每个元素的值是上一个元素加3，
    //最后对10取模再加1，保证数值在1到10之间循环变化

    for (std::size_t i = 0; i < rows; ++i)
    {
        for (std::size_t j = 0; j < cols; ++j)
        {
            matrix[i][j] = value % 10 + 1;
            value += 3;
        }
    }

    return matrix;
}
//矩阵相乘：输入两个矩阵，输出一个矩阵
Matrix multiplyMatrix(const Matrix &a, const Matrix &b)
{
    //先验证矩阵合不合格
    validateMatrix(a, "Matrix A");
    validateMatrix(b, "Matrix B");

    //取出尺寸
    std::size_t aRows = a.size();
    std::size_t aCols = a[0].size();
    std::size_t bRows = b.size();
    std::size_t bCols = b[0].size();
//矩阵乘法必须满足的规则！
//A 的列数 必须等于 B 的行数
    if (aCols != bRows)
    {
        throw std::runtime_error("Matrix size mismatch: A columns must equal B rows.");
    }
//创建结果矩阵，尺寸是A的行数乘B的列数，初始值全是0
    Matrix result(aRows, std::vector<long long>(bCols, 0));

    for (std::size_t i = 0; i < aRows; ++i)//遍历a的每一行
    {
        for (std::size_t j = 0; j < bCols; ++j)//遍历b的每一列
        {
            for (std::size_t k = 0; k < aCols; ++k)//对应相乘后相加
            {
                result[i][j] += a[i][k] * b[k][j];
            }
        }
    }

    return result;//返回算好的结果矩阵
}

//矩阵打印机
void printMatrix(
    const Matrix &matrix,
    std::size_t maxRows ,
    std::size_t maxCols )
{//去一个小值，最多打印4行，避免刷屏
    std::size_t rows = std::min(maxRows, matrix.size());

    for (std::size_t i = 0; i < rows; ++i)//循环打印每一行
    {//每行最多4列
        std::size_t cols = std::min(maxCols, matrix[i].size());
//打印每一行里的数字，用制表符隔开
        for (std::size_t j = 0; j < cols; ++j)
        {
            std::cout << matrix[i][j] << "\t";
        }
//如果列太多，后面打印...表示省略
        if (matrix[i].size() > maxCols)
        {
            std::cout << "...";
        }

        std::cout << "\n";
    }
//如果太多，后面打印...表示省略
    if (matrix.size() > maxRows)
    {
        std::cout << "...\n";
    }
}



// 运行矩阵演示
void runMatrixDemo()
{
    std::cout << "========== Matrix Thread Pool Demo ==========\n";

    // 线程数量设定为10，同时又十个线程
    const std::size_t threadCount = 10;
    // 任务数量12，会提交12个矩阵
    const int taskCount = 12;

    MatrixTaskScheduler scheduler(threadCount);

    std::vector<std::future<void>> futures;//任务凭证列表

    //循环提交12个任务
    for (int taskId = 1; taskId <= taskCount; ++taskId)
    {
        //生成矩阵ab
        Matrix a = generateMatrix(3 + taskId % 3, 4, taskId);
        Matrix b = generateMatrix(4, 3 + taskId % 2, taskId * 10);

        //把任务交给线程池
        futures.push_back(
            scheduler.addMatrixMultiplyTask(//提交一个矩阵乘法任务，参数是任务id和两个矩阵
                taskId,
                std::move(a),//任务转移而不是复制
                std::move(b)));//回执单存起来

        std::cout << "Submitted matrix task " << taskId << "\n";
    }

    for (auto &future : futures)
    {
        try
        {
            future.get();
        }
        catch (const std::exception &e)
        {
            std::cerr << "[Main] Matrix task failed: "
                      << e.what() << std::endl;
        }
    }

    scheduler.processCallbacks();

    scheduler.shutdown();

    std::cout << "\nAll matrix tasks finished. Scheduler closed safely.\n";
}

#endif
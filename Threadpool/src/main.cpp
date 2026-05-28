#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <chrono>
#include <string>
#include <fstream>
#include <algorithm>
#include <type_traits>
#include <memory>
#include <utility>

#include <cstdio>
#include "../include/MatrixTaskScheduler.h"
#include "ThreadPool.h"
#include "CallbackDispatcher.h"

// ============================================================
// 6. main 函数
// ============================================================

int main()
{
    runMatrixDemo();
    return 0;
}
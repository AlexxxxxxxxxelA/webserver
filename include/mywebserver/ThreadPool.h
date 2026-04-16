#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "noncopyable.h"

class ThreadPool : noncopyable
{
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t threadCount);
    ~ThreadPool();

    void start();
    void stop();
    bool submit(Task task);

private:
    void runInThread();

    size_t threadCount_;
    bool running_;
    std::vector<std::thread> threads_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cond_;
};

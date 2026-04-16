#include "mywebserver/ThreadPool.h"

ThreadPool::ThreadPool(size_t threadCount)
    : threadCount_(threadCount)
    , running_(false)
{
}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
    {
        return;
    }

    running_ = true;
    for (size_t i = 0; i < threadCount_; ++i)
    {
        threads_.emplace_back(&ThreadPool::runInThread, this);
    }
}

void ThreadPool::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
        {
            return;
        }
        running_ = false;
    }

    cond_.notify_all();
    for (std::thread &thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    threads_.clear();
}

bool ThreadPool::submit(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
        {
            return false;
        }
        tasks_.push(std::move(task));
    }

    cond_.notify_one();
    return true;
}

void ThreadPool::runInThread()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this]() {
                return !running_ || !tasks_.empty();
            });

            if (!running_ && tasks_.empty())
            {
                break;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}

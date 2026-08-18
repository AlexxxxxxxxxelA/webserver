#include <BoundedThreadPool.h>

#include <Logger.h>

#include <stdexcept>

BoundedThreadPool::BoundedThreadPool(size_t threadCount, size_t maxQueuedTasks)
    : threadCount_(threadCount)
    , maxQueuedTasks_(maxQueuedTasks)
    , started_(false)
    , accepting_(false)
{
    if (threadCount_ == 0 || maxQueuedTasks_ == 0)
    {
        throw std::invalid_argument("thread count and queue capacity must be positive");
    }
}

BoundedThreadPool::~BoundedThreadPool()
{
    stop();
}

void BoundedThreadPool::start()
{
    // start 只允许真正创建一次线程；重复调用直接返回。
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_)
    {
        return;
    }

    started_ = true;
    accepting_ = true;
    workers_.reserve(threadCount_);
    for (size_t i = 0; i < threadCount_; ++i)
    {
        workers_.emplace_back(&BoundedThreadPool::workerLoop, this);
    }
}

bool BoundedThreadPool::trySubmit(Task task)
{
    {
        /*
         * 只在检查容量和 push 时持锁。notify_one 放在解锁后执行，避免刚唤醒的
         * worker 立刻又阻塞在同一把 mutex 上。
         */
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || tasks_.size() >= maxQueuedTasks_)
        {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    notEmpty_.notify_one();
    return true;
}

void BoundedThreadPool::stop()
{
    /*
     * 【为什么先 accepting_=false，再 join？】
     * false 同时表达两件事：不再接受新任务；队列清空后 worker 应退出。
     * 如果不先改变退出条件，join 会一直等待永不退出的 worker，形成死锁。
     */
    std::lock_guard<std::mutex> stopLock(stopMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_)
        {
            return;
        }
        accepting_ = false;
    }
    notEmpty_.notify_all();

    for (size_t i = 0; i < workers_.size(); ++i)
    {
        if (workers_[i].joinable())
        {
            workers_[i].join();
        }
    }
    workers_.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void BoundedThreadPool::workerLoop()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            /*
             * condition_variable 必须搭配谓词。线程可能发生“虚假唤醒”，即没有
             * notify 也从 wait 返回；谓词会再次检查真实条件，防止从空队列取任务。
             */
            notEmpty_.wait(lock, [this]() {
                return !tasks_.empty() || !accepting_;
            });

            // stop() 停止接收新任务，但会先处理完已经进入队列的任务。
            if (tasks_.empty())
            {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        // 执行任务时必须释放队列锁，否则其他 worker 无法取任务，提交者也无法入队。
        try
        {
            task();
        }
        catch (const std::exception &ex)
        {
            LOG_ERROR << "Unhandled worker task exception: " << ex.what();
        }
        catch (...)
        {
            // 异常不能越过线程入口，否则 std::thread 会调用 std::terminate 结束进程。
            LOG_ERROR << "Unhandled unknown worker task exception";
        }
    }
}

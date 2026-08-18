#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "noncopyable.h"

/**
 * 专门执行阻塞业务的有界线程池。
 *
 * EventLoopThreadPool 中的线程用于 epoll 和 Socket IO，不能拿来执行耗时的
 * DeepSeek HTTPS 请求。本线程池与 Reactor 分离，worker 阻塞时不会阻塞 IO loop。
 *
 * 队列必须有上限：如果外部 API 变慢而请求持续到来，无界队列会不断占用内存，
 * 同时让用户等待一个实际上不可能及时完成的请求。trySubmit() 在队列满时立即
 * 返回 false，让 HTTP 层快速返回 503，而不是阻塞 EventLoop 等待空位。
 *
 * 【基础知识：IO 线程池与业务线程池】
 * EventLoopThreadPool 是 IO 线程池，每个线程持续执行 epoll_wait；本类是普通
 * worker 线程池，每个线程从任务队列取一个 std::function 执行。两者虽然都叫
 * “线程池”，但职责完全不同，不能把阻塞业务重新塞回 IO 线程池。
 *
 * 【基础知识：什么是背压】
 * 当请求到达速度大于处理速度时，系统必须拒绝、限流或让上游减速。有界队列在
 * 容量用完后返回 false，就是最简单的背压。无界队列只是把过载暂时隐藏成内存
 * 增长和越来越长的等待时间，最终可能导致 OOM。
 *
 * 【常见面试问题】为什么 trySubmit 不能等待队列空位？
 * 因为调用者是 EventLoop。若它等待条件变量，整个 IO loop 又会被阻塞，业务线程池
 * 就失去了隔离阻塞操作的意义。正确策略是立即失败，让 HTTP 层返回 503。
 */
class BoundedThreadPool : noncopyable
{
public:
    using Task = std::function<void()>;

    BoundedThreadPool(size_t threadCount, size_t maxQueuedTasks);
    ~BoundedThreadPool();

    void start();
    // 非阻塞提交：成功进入队列返回 true；未启动、已停止或队列满返回 false。
    bool trySubmit(Task task);
    // 停止接收新任务，处理完已接受任务，然后 join 所有 worker。
    void stop();

private:
    void workerLoop();

    const size_t threadCount_;
    const size_t maxQueuedTasks_;
    bool started_;   // 是否已经创建 worker
    bool accepting_; // 是否仍接受任务；false 也是 worker 退出条件的一部分
    // 串行化 stop()，防止两个线程重复 join 同一批 worker。
    std::mutex stopMutex_;
    std::mutex mutex_; // 同时保护 started_、accepting_ 和 tasks_
    std::condition_variable notEmpty_; // 队列为空时让 worker 睡眠，避免空转占用 CPU
    std::deque<Task> tasks_; // FIFO：先提交的任务先被 worker 取走
    std::vector<std::thread> workers_;
};

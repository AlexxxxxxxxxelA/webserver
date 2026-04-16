#pragma once


#include<unistd.h>
#include<sys/syscall.h>

/*

它是一个当前线程信息工具类/命名空间，主要用来快速获取当前线程的线程 ID。
muduo 是多线程网络库，很多地方需要判断当前代码运行在哪个线程，比如 EventLoop 要判断调用者是不是它所属的 IO 线程，所以需要频繁获取当前线程 tid。
*/
/*
因为系统调用有一定开销。gettid 需要进入内核态，如果在日志、事件循环判断等高频路径里反复调用，会浪费性能。所以 muduo 的做法是：每个线程第一次调用时通过系统调用获取 tid，然后缓存到线程局部变量里，后续直接读变量即可。
*/
namespace CurrentThread
{
    extern thread_local int t_cachedTid;

    void cacheTid();

    inline int tid(){
        if(__builtin_expect(t_cachedTid==0,0)){
            //__builtin_expect
            /*
            这是 GCC/Clang 提供的分支预测优化提示。
__builtin_expect(t_cachedTid == 0, 0)
意思是告诉编译器：t_cachedTid == 0 这个条件大概率是假的。
也就是说，绝大多数时候 tid 已经被缓存好了，不需要再调用 cacheTid()。只有线程第一次调用 tid() 时，才会进入这个分支。
            */
            cacheTid();
        }
        return t_cachedTid;
    }
}
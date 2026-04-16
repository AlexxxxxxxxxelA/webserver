#include<CurrentThread.h>

namespace CurrentThread
{
    thread_local int t_cachedTid=0;
    void cacheTid(){
        if(t_cachedTid==0){
            //SYS_gettid 获取的是线程 ID。同一个进程中的不同线程，tid 不同。所以在多线程网络库中，如果要区分具体是哪一个线程，应该用 gettid，而不是 getpid
            t_cachedTid=static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}
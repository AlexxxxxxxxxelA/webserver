#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <stddef.h>

// 网络库底层的缓冲区类型定义
class Buffer
{
public:
    static const size_t kCheapPrepend = 8;//初始预留的prependabel空间大小
    static const size_t kInitialSize = 1024;

    /*
    初始缓冲区布局
    | 预留区(8字节) | 可读区(空) | 可写区(1024字节) |
                    ^readerIndex_
                    ^writerIndex_
    */
    /*
    可读区：当前已经存了数据、但应用还没取走的数据。
    可写区：当前已经存了数据、但应用还没取走的数据。
    */
    explicit Buffer(size_t initalSize = kInitialSize)
        : buffer_(kCheapPrepend + initalSize)
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend)
    {
    }

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    /*
    读取/消费数据：移动 readerIndex_
    比如 retrieve(len)：
    如果还没把可读区全取完，就 readerIndex_ += len
    如果正好全取完了，就 retrieveAll()，直接把两个下标都重置回 kCheapPrepend。
    */
    // 返回缓冲区中可读数据的起始地址
    const char *peek() const { return begin() + readerIndex_; }

    /**
     * 在当前可读区域 [peek(), beginWrite()) 中查找 HTTP 行结束符 "\r\n"。
     *
     * HTTP 的请求行和每个 Header 都以 CRLF 结束。例如：
     *     GET /health HTTP/1.1\r\n
     *     Host: localhost\r\n
     *
     * 返回值指向 '\r'；如果当前 Buffer 中还没有完整的 CRLF，则返回 nullptr。
     * 这正是处理 TCP 半包的关键：没有找到 CRLF 时不消费数据，等待下一次
     * EPOLLIN 把后续字节继续追加到同一个 Buffer，再从原位置继续解析。
     *
     * 【基础知识：为什么 HTTP 需要自己寻找边界？】
     * TCP 是“面向字节流”的协议，它只保证字节按顺序、可靠地到达，不保留应用层
     * 每次 send() 的边界。发送端调用一次 send()，接收端可能分多次 read() 收到；
     * 发送端调用多次 send()，接收端也可能一次 read() 全部收到。因此 HTTP 必须
     * 使用 CRLF 和 Content-Length 等规则，在连续字节流中恢复请求边界。
     *
     * 【常见面试问题】TCP 有“粘包”问题吗？如何解决？
     * 更准确地说，粘包不是 TCP 出错，而是字节流协议本来就没有消息边界。
     * 应用层常见解决方案有：
     * 1. 固定长度；
     * 2. 特殊分隔符，例如请求行和 Header 使用 CRLF；
     * 3. 长度字段，例如 HTTP 的 Content-Length；
     * 4. 自描述协议，例如完整 JSON 解析或 Protobuf 长度前缀。
     */
    const char *findCRLF() const
    {
        const char *crlf = std::search(peek(), beginWrite(), "\r\n", "\r\n" + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    /**
     * 消费从当前 peek() 到 end 之前的所有字节。
     *
     * HttpContext 在解析完一行后通常传入 crlf + 2，从而同时消费：
     *     行内容 + '\r' + '\n'
     * end 必须指向当前 Buffer 的可读区域或可读区域末尾。
     *
     * 【易错点】为什么不能只消费到 crlf？
     * 因为 crlf 指向 '\r'，如果不额外消费两个字节，下一轮解析仍会从同一个
     * "\r\n" 开始，解析器可能不断遇到空行而无法向前推进。
     */
    void retrieveUntil(const char *end)
    {
        retrieve(static_cast<size_t>(end - peek()));
    }
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readerIndex_ += len; // 说明应用只读取了可读缓冲区数据的一部分，就是len长度 还剩下readerIndex+=len到writerIndex_的数据未读
        }
        else // len == readableBytes()
        {
            retrieveAll();
        }
    }
    void retrieveAll()
    {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    // 把onMessage函数上报的Buffer数据 转成string类型的数据返回
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }
    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len); // 上面一句把缓冲区中可读的数据已经读取出来 这里肯定要对缓冲区进行复位操作
        return result;
    }
    //写入数据：移动 writerIndex_
    // buffer_.size - writerIndex_
    void ensureWritableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容
        }
    }

    // 把[data, data+len]内存上的数据添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data+len, beginWrite());
        writerIndex_ += len;
    }

    // std::string 版本只是便捷重载，最终仍复用上面的字节追加逻辑。
    void append(const std::string &data)
    {
        append(data.data(), data.size());
    }
    char *beginWrite() { return begin() + writerIndex_; }
    const char *beginWrite() const { return begin() + writerIndex_; }

    // 从fd上读取数据
    ssize_t readFd(int fd, int *saveErrno);
    // 通过fd发送数据
    ssize_t writeFd(int fd, int *saveErrno);

private:
    // vector底层数组首元素的地址 也就是数组的起始地址
    char *begin() { return &*buffer_.begin(); }
    const char *begin() const { return &*buffer_.begin(); }

    //扩容
    void makeSpace(size_t len)
    {
        /**
         * | kCheapPrepend |xxx| reader | writer |                     // xxx标示reader中已读的部分
         * | kCheapPrepend | reader ｜          len          |
         **/
        //策略 A：直接扩容底层vector
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) // 也就是说 len > xxx前面剩余的空间 + writer的部分
        {
            buffer_.resize(writerIndex_ + len);
        }

        //策略 B：Buffer 不是每次空间不够都扩容，而是优先复用前面已经腾出来的空间。
        else // 这里说明 len <= xxx + writer 把reader搬到从xxx开始 使得xxx后面是一段连续空间
        {
            size_t readable = readableBytes(); // readable = reader的长度
            // 将当前缓冲区中从readerIndex_到writerIndex_的数据
            // 拷贝到缓冲区起始位置kCheapPrepend处，以便腾出更多的可写空间
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;//一个可扩容的连续字节数组充当buffer
    size_t readerIndex_;
    size_t writerIndex_;
};

#include <strings.h>
#include <string.h>

#include "mywebserver/InetAddress.h"

InetAddress::InetAddress(uint16_t port, std::string ip)
{
    ::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;//ipv4
    addr_.sin_port = ::htons(port); // 端口
    /*
    htons() 用来把主机字节序的 16 位整数转换成网络字节序。
    因为网络通信统一规定使用大端序，而很多主机是小端序，所以像端口号这种字段在写入 sockaddr_in 时要用 htons() 转换。
    */
    addr_.sin_addr.s_addr = ::inet_addr(ip.c_str());//把字符串 IP，比如 "127.0.0.1"，转换成网络字节序的 IPv4 地址。
}

std::string InetAddress::toIp() const
{
    // addr_
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    return buf;
}

std::string InetAddress::toIpPort() const
{
    // ip:port
    /*
    这个函数先用 inet_ntop 把二进制 IP 转成字符串写到 buf 里，然后通过 strlen(buf) 找到当前字符串结尾，再用 sprintf(buf + end, ":%u", port) 把端口号追加到后面，最终形成 "ip:port" 的字符串。
其中端口号要先通过 ntohs 从网络字节序转成主机字节序。
    */
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    size_t end = ::strlen(buf);
    uint16_t port = ::ntohs(addr_.sin_port);//net2host
    sprintf(buf+end, ":%u", port);
    return buf;
}

uint16_t InetAddress::toPort() const
{
    return ::ntohs(addr_.sin_port);
}

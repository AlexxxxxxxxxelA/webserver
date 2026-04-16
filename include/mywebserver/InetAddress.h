#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>


/*
InetAddress 是对 sockaddr_in 的一层封装，用来表示一个 IPv4 网络地址，也就是 ip + port。
它内部保存一个 sockaddr_in addr_，对外提供构造、转字符串、获取端口、获取原始 socket 地址等接口，方便和 Linux socket API 配合使用。
*/
// 封装socket地址类型
class InetAddress
{
public:
    explicit InetAddress(uint16_t port = 0, std::string ip = "127.0.0.1");
    explicit InetAddress(const sockaddr_in &addr)//拷贝构造
        : addr_(addr){}

    std::string toIp() const;//toIp() 把内部保存的二进制 IPv4 地址转换成字符串形式，比如 "127.0.0.1"。
    std::string toIpPort() const;//转成ip:port
    uint16_t toPort() const;//获取端口号

    const sockaddr_in *getSockAddr() const { return &addr_; }
    void setSockAddr(const sockaddr_in &addr) { addr_ = addr; }

private:
    sockaddr_in addr_;//保存ipv4的结构体
};
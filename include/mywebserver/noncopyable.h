#pragma once


/*
因为很多类内部持有系统资源或有唯一语义。
比如一个对象持有一个文件描述符 fd，如果直接拷贝，就会出现两个对象管理同一个 fd 的情况，析构时可能重复关闭，或者状态被多个对象同时修改，造成严重 bug。所以这类对象应该只允许唯一拥有，不能随便复制。
*/
class noncopyable{
public:
    noncopyable(const noncopyable&)=delete;//禁止拷贝构造
    noncopyable& operator=(const noncopyable&)=delete;//禁止拷贝赋值

protected://放在 protected 是为了表达这个类只能作为基类使用，不希望外部直接创建 noncopyable 对象。
    noncopyable()=default;
    ~noncopyable()=default;
};

/*
这个类为什么析构函数不写成 virtual？
因为 noncopyable 不是为了多态使用的，它只是一个工具基类，不应该通过 noncopyable* 指针去删除派生类对象。
如果加 virtual 析构函数，会给派生类引入虚表开销，没必要。
muduo 这种网络库比较重视性能和对象语义，所以这里通常不加 virtual。
*/
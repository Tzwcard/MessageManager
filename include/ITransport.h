#pragma once

#include <cstddef>

namespace MessageManager
{
class ITransport
{
public:
    virtual ~ITransport() = default;

    // 非阻塞读：>0 字节数，0 = EOF，-EAGAIN = 暂无数据，<0 = 错误
    virtual int read(void* buf, size_t len) = 0;

    // 非阻塞写：>0 已写字节数，-EAGAIN = 暂时不可写，<0 = 错误
    virtual int write(const void* buf, size_t len) = 0;

    // 等待可读/可写；timeout_ms < 0 表示一直等
    // 返回 bitmask: 1=readable, 2=writable；0=timeout；<0=错误
    virtual int wait(bool want_write, int timeout_ms) = 0;

    // 从其他线程唤醒 wait()
    virtual void wake() = 0;

    // 让阻塞中的 read/write/wait 退出
    virtual void shutdown() = 0;

    virtual void close() = 0;
};

};

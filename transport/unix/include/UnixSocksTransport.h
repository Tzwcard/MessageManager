#pragma once

#include "ITransport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace MessageManager
{
// AF_UNIX SOCK_STREAM transport，自己管理 path / listener / connect / accept。
//
// server:
//   CUnixSocksTransport t;
//   t.CreateSocks("/run/msgmgr/conn.sock");   // bind + listen + accept
//   mgr.assign_transport(t);
//
// client:
//   CUnixSocksTransport t;
//   t.ConnectSocks("/run/msgmgr/conn.sock");
//   mgr.assign_transport(t);
//
// Windows 的 AF_UNIX 是 pathname socket：bind(path) 会在文件系统上创建
// socket 文件（NTFS reparse point）。CreateSocks / ListenSocks 会先清理
// 残留路径；AcceptSocks 成功后删除路径。Windows 没有 socketpair()。
//
// 用 assign_transport(t) 这种非拥有方式时，t 必须比 MessageManager 活得久。
class CUnixSocksTransport : public ITransport
{
public:
    CUnixSocksTransport();
    ~CUnixSocksTransport() override;

    CUnixSocksTransport(const CUnixSocksTransport&) = delete;
    CUnixSocksTransport& operator=(const CUnixSocksTransport&) = delete;

    // server: bind + listen + accept。阻塞到有 client 连上。
    // 返回 0 成功；<0 错误（Linux -errno，Windows -WSA*）。
    int CreateSocks(const std::string& path, int backlog = 1);

    // server: 只 bind + listen，不阻塞；之后调用 AcceptSocks()。
    int ListenSocks(const std::string& path, int backlog = 1);

    // server: accept 一个连接，关闭 listener，删除 path。
    int AcceptSocks();

    // client: connect。
    int ConnectSocks(const std::string& path);

    // 包装一条已经建立好的 AF_UNIX fd。
    // take_ownership=false 时内部 dup / WSADuplicateSocket。
    int AdoptSocks(int fd, bool take_ownership = true);

    bool IsOpen() const
    {
        return _fd >= 0;
    }
    bool IsListening() const
    {
        return _listener >= 0;
    }
    int fd() const
    {
        return static_cast<int>(_fd);
    }

    ITransport* Get()
    {
        return this;
    }
    const ITransport* Get() const
    {
        return this;
    }

    int read(void* buf, size_t len) override;
    int write(const void* buf, size_t len) override;
    int wait(bool want_write, int timeout_ms) override;
    void wake() override;
    void shutdown() override;
    void close() override;

private:
    void reset();
    bool make_wake_pair();
    void drain_wake();
    void close_handles();
    void close_listener();
    void remove_socket_path();

    intptr_t _fd = -1;
    intptr_t _listener = -1;
    intptr_t _wake_read = -1;
    intptr_t _wake_write = -1;

    std::string _path;
    bool _owns_path = false;

    std::atomic<bool> _shutdown{false};
    std::mutex _handle_mutex;
    bool _closed = false;
};

} // namespace MessageManager

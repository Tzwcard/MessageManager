# MessageManagerUnix

`MessageManager` 的默认 fallback transport，基于 **AF_UNIX SOCK_STREAM**。

`CUnixSocksTransport` 把整条链路包在一个类里：

```text
path 清理 -> socket -> bind -> listen -> accept -> connect
         -> 非阻塞 read/write -> poll/WSAPoll -> wake -> close
```

## 用法

server：

```cpp
#include <UnixSocksTransport.h>

MM::CUnixSocksTransport t;
if (t.CreateSocks("/run/msgmgr/conn.sock") == 0)   // bind + listen + accept
    mgr.assign_transport(t);
```

client：

```cpp
MM::CUnixSocksTransport t;
if (t.ConnectSocks("/run/msgmgr/conn.sock") == 0)
    mgr.assign_transport(t);
```

`assign_transport(t)` 是非拥有形式，`t` 必须比 `MessageManager` 活得久。
也可以写 `mgr.assign_transport(t.Get())`。

需要自己控制 accept 时机时：

```cpp
MM::CUnixSocksTransport t;
t.ListenSocks(path);      // bind + listen，立即返回
// ... 等 client ...
t.AcceptSocks();          // accept，关闭 listener，删除 path
mgr.assign_transport(t);
```

已经有一条 AF_UNIX fd 时：

```cpp
t.AdoptSocks(fd, true);   // take_ownership
mgr.assign_transport(t);
```

## Windows 的 AF_UNIX

Windows 的 AF_UNIX 不是 Linux 的无名 socketpair：

- **只支持 pathname socket**：`bind(path)` 会在文件系统上创建一个
  NTFS reparse point（socket 文件），**真的落盘**；
- `sockaddr_un::sun_path` 是 `char[108]`，路径必须 `< 108` 字节；
- Winsock **没有 `socketpair()`**，本类用 listener + connect + accept 拼；
- abstract / unnamed socket 在 Windows 上实际不可用，不作为跨平台依赖；
- `CreateSocks` / `ListenSocks` 会先 `RemoveUnixSocketFile(path)` 清残留；
- `AcceptSocks` 成功后删除 path；`close()` 时如果还持有 path 也会清理。

wake 机制不使用 AF_UNIX：Linux 用 `pipe()`，Windows 用 loopback TCP，
避免每条 transport 都在磁盘上留一个 socket 文件。

## API

```cpp
class CUnixSocksTransport : public ITransport
{
public:
    int CreateSocks(const std::string& path, int backlog = 1); // server, 阻塞 accept
    int ListenSocks(const std::string& path, int backlog = 1); // server, 不阻塞
    int AcceptSocks();                                          // server
    int ConnectSocks(const std::string& path);                  // client
    int AdoptSocks(int fd, bool take_ownership = true);
    ITransport* Get();

    bool IsOpen() const;
    bool IsListening() const;
    int fd() const;

    // ITransport
    int read(void* buf, size_t len) override;
    int write(const void* buf, size_t len) override;
    int wait(bool want_write, int timeout_ms) override;
    void wake() override;
    void shutdown() override;
    void close() override;
};
```

返回值：`0` 成功，负数失败（Linux `-errno`，Windows `-WSA*`）。

## 独立 helpers

`UnixSocket.h` 仍然提供低层函数，主要给测试和特殊场景用：

```cpp
int ListenUnix(const std::string& path, int backlog = 1);
int ConnectUnix(const std::string& path);
bool RemoveUnixSocketFile(const std::string& path);
std::string MakeTempUnixSocketPath(const char* prefix = "mm");
bool CreateUnixSocketPair(const std::string& path, int& first, int& second);
```

普通 service 直接用 `CUnixSocksTransport` 即可。

## 依赖关系

`MessageManager` core **不依赖**这个 target：

- 不需要默认 fallback 的 service 可以只链接 `libMessageManager.a`；
- 想用自己的 socket / TLS / 共享内存实现时，直接实现 `ITransport`；
- 需要默认实现时再链接 `libMessageManagerUnix.a`。

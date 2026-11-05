#pragma once

#include <string>

namespace MessageManager
{
// AF_UNIX SOCK_STREAM helper。
//
// Windows 的 AF_UNIX 实现和 Linux 不完全一样：
//   - 只支持 pathname socket；bind(path) 会在文件系统上创建一个
//     NTFS reparse point（socket 文件，真的落盘）；
//   - sun_path 是 char[108]，路径必须 < 108 字节；
//   - Winsock 没有 socketpair()，必须 listener + connect + accept；
//   - abstract / unnamed socket 实际上不可用，不作为跨平台依赖。
//
// Linux 也统一使用 pathname socket，保证两端行为一致。
// 调用方负责 path 的生命周期：bind 前清理残留，close 后删除。

// 返回 fd >= 0；失败返回负的错误码（Linux -errno，Windows -WSA*）。
int ListenUnix(const std::string& path, int backlog = 1);
int ConnectUnix(const std::string& path);

// 删除 socket 文件。文件本来不存在也算成功。
bool RemoveUnixSocketFile(const std::string& path);

// 生成当前进程唯一的临时 socket 路径。
// 只生成字符串，不创建文件；调用方负责 RemoveUnixSocketFile()。
std::string MakeTempUnixSocketPath(const char* prefix = "mm");

// 测试 / 本地通道：在 path 上建立一对已连接的 AF_UNIX SOCK_STREAM。
// 连接建立后会自动删除 path；失败时 first/second 为 -1。
bool CreateUnixSocketPair(const std::string& path, int& first, int& second);

} // namespace MessageManager

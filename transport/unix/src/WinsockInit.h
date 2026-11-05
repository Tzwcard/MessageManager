#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>

namespace MessageManager
{
namespace detail
{
// 进程内只初始化一次 Winsock；不调用 WSACleanup，进程退出时由系统回收。
inline void EnsureWinsock()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
} // namespace detail
} // namespace MessageManager

#endif // _WIN32

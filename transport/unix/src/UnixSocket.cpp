#include "UnixSocket.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>

#include <windows.h>

#include <atomic>
#include <string>

#include "WinsockInit.h"
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#endif

namespace MessageManager
{
namespace
{
bool MakeAddress(const std::string& path, sockaddr_un& address)
{
    // sun_path 是 char[108]（两个平台一致），必须留一个 '\0'。
    if (path.empty() || path.size() >= sizeof(address.sun_path))
        return false;

    address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return true;
}

void CloseFd(int fd)
{
    if (fd < 0)
        return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return std::wstring();

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0)
        return std::wstring();

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), &result[0], length);
    return result;
}

std::string WideToUtf8(const wchar_t* text)
{
    if (!text || !*text)
        return std::string();

    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0)
        return std::string();

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], length, nullptr,
                        nullptr);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}
#endif

} // namespace

int ListenUnix(const std::string& path, int backlog)
{
    sockaddr_un address{};
    if (!MakeAddress(path, address))
        return -EINVAL;

#ifdef _WIN32
    detail::EnsureWinsock();

    const SOCKET fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -WSAGetLastError();

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        const int error = WSAGetLastError();
        closesocket(fd);
        return -error;
    }

    if (listen(fd, backlog) != 0)
    {
        const int error = WSAGetLastError();
        closesocket(fd);
        return -error;
    }

    return static_cast<int>(fd);
#else
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        const int error = errno;
        ::close(fd);
        return -error;
    }

    if (listen(fd, backlog) != 0)
    {
        const int error = errno;
        ::close(fd);
        return -error;
    }

    return fd;
#endif
}

int ConnectUnix(const std::string& path)
{
    sockaddr_un address{};
    if (!MakeAddress(path, address))
        return -EINVAL;

#ifdef _WIN32
    detail::EnsureWinsock();

    const SOCKET fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -WSAGetLastError();

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        const int error = WSAGetLastError();
        closesocket(fd);
        return -error;
    }

    return static_cast<int>(fd);
#else
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        const int error = errno;
        ::close(fd);
        return -error;
    }

    return fd;
#endif
}

bool RemoveUnixSocketFile(const std::string& path)
{
    if (path.empty())
        return false;

#ifdef _WIN32
    const std::wstring wide = Utf8ToWide(path);
    if (wide.empty())
        return false;

    if (DeleteFileW(wide.c_str()) != 0)
        return true;

    return GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    if (::unlink(path.c_str()) == 0)
        return true;
    return errno == ENOENT;
#endif
}

std::string MakeTempUnixSocketPath(const char* prefix)
{
    const char* name_prefix = (prefix && *prefix) ? prefix : "mm";
    static std::atomic<unsigned long> counter{0};
    const unsigned long id = counter.fetch_add(1);

    char name[96]{};
    std::snprintf(name, sizeof(name), "%s-%lu-%lu.sock", name_prefix,
                  static_cast<unsigned long>(
#ifdef _WIN32
                      GetCurrentProcessId()
#else
                      getpid()
#endif
                          ),
                  id);

#ifdef _WIN32
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH)
        return std::string();

    const std::string directory = WideToUtf8(temp);
    if (directory.empty())
        return std::string();

    return directory + name;
#else
    const char* directory = std::getenv("TMPDIR");
    if (!directory || !*directory)
        directory = "/tmp";

    std::string result(directory);
    if (!result.empty() && result.back() != '/')
        result.push_back('/');
    result += name;
    return result;
#endif
}

bool CreateUnixSocketPair(const std::string& path, int& first, int& second)
{
    first = -1;
    second = -1;

    // Windows 上残留的 socket 文件会让 bind 失败；先清掉。
    RemoveUnixSocketFile(path);

    const int listener = ListenUnix(path, 1);
    if (listener < 0)
        return false;

    const int client = ConnectUnix(path);
    if (client < 0)
    {
        CloseFd(listener);
        RemoveUnixSocketFile(path);
        return false;
    }

    int server = -1;
#ifdef _WIN32
    server = static_cast<int>(accept(static_cast<SOCKET>(listener), nullptr, nullptr));
#else
    for (;;)
    {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd >= 0)
        {
            server = fd;
            break;
        }
        if (errno != EINTR)
            break;
    }
#endif

    CloseFd(listener);
    RemoveUnixSocketFile(path);

    if (server < 0)
    {
        CloseFd(client);
        return false;
    }

    first = client;
    second = server;
    return true;
}

} // namespace MessageManager

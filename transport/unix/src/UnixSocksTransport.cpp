#include "UnixSocksTransport.h"

#include "UnixSocket.h"
#include "WinsockInit.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace MessageManager
{
namespace
{
void CloseRawFd(intptr_t fd)
{
    if (fd < 0)
        return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

#ifdef _WIN32

bool SetNonBlocking(intptr_t handle)
{
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &mode) == 0;
}

intptr_t DuplicateSocket(intptr_t handle)
{
    WSAPROTOCOL_INFOW info{};
    if (WSADuplicateSocketW(static_cast<SOCKET>(handle), GetCurrentProcessId(),
                            &info) != 0)
        return -1;

    const SOCKET copy = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                   FROM_PROTOCOL_INFO, &info, 0, 0);
    if (copy == INVALID_SOCKET)
        return -1;
    return static_cast<intptr_t>(copy);
}

int AcceptFd(intptr_t listener)
{
    const SOCKET fd = accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (fd == INVALID_SOCKET)
        return -WSAGetLastError();
    return static_cast<int>(fd);
}

int WriteWakeByte(intptr_t handle)
{
    const char byte = 1;
    return send(static_cast<SOCKET>(handle), &byte, 1, 0);
}

#else

bool SetNonBlocking(intptr_t fd)
{
    const int flags = fcntl(static_cast<int>(fd), F_GETFL, 0);
    if (flags < 0)
        return false;
    if (fcntl(static_cast<int>(fd), F_SETFL, flags | O_NONBLOCK) < 0)
        return false;

    const int fd_flags = fcntl(static_cast<int>(fd), F_GETFD, 0);
    if (fd_flags >= 0)
        fcntl(static_cast<int>(fd), F_SETFD, fd_flags | FD_CLOEXEC);
    return true;
}

intptr_t DuplicateFd(int fd)
{
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    const int copy = dup(fd);
    if (copy >= 0)
        fcntl(copy, F_SETFD, FD_CLOEXEC);
    return copy;
#endif
}

int AcceptFd(intptr_t listener)
{
    for (;;)
    {
        const int fd = accept(static_cast<int>(listener), nullptr, nullptr);
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return -errno;
    }
}

#endif
} // namespace

CUnixSocksTransport::CUnixSocksTransport() = default;

CUnixSocksTransport::~CUnixSocksTransport()
{
    close();
}

void CUnixSocksTransport::reset()
{
    std::lock_guard<std::mutex> lock(_handle_mutex);
    close_handles();
    _closed = false;
    _shutdown.store(false);
}

void CUnixSocksTransport::close_listener()
{
    if (_listener >= 0)
    {
        CloseRawFd(_listener);
        _listener = -1;
    }
}

void CUnixSocksTransport::remove_socket_path()
{
    if (_owns_path && !_path.empty())
        RemoveUnixSocketFile(_path);

    _owns_path = false;
    _path.clear();
}

void CUnixSocksTransport::close_handles()
{
    close_listener();

    if (_fd >= 0)
    {
        CloseRawFd(_fd);
        _fd = -1;
    }

    if (_wake_read >= 0)
    {
        CloseRawFd(_wake_read);
        _wake_read = -1;
    }

    if (_wake_write >= 0)
    {
        CloseRawFd(_wake_write);
        _wake_write = -1;
    }

    remove_socket_path();
}

bool CUnixSocksTransport::make_wake_pair()
{
    // Windows 的 AF_UNIX 是 pathname socket，bind 会落盘。
    // wake 是内部机制，不应该每条 transport 都产生一个 socket 文件，
    // 所以这里用 pipe（Linux）/ loopback TCP（Windows）。
#ifdef _WIN32
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    int length = static_cast<int>(sizeof(address));
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        closesocket(listener);
        return false;
    }

    SOCKET writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (writer == INVALID_SOCKET)
    {
        closesocket(listener);
        return false;
    }

    if (connect(writer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        closesocket(writer);
        closesocket(listener);
        return false;
    }

    const SOCKET reader = accept(listener, nullptr, nullptr);
    closesocket(listener);

    if (reader == INVALID_SOCKET)
    {
        closesocket(writer);
        return false;
    }

    SetNonBlocking(static_cast<intptr_t>(reader));
    SetNonBlocking(static_cast<intptr_t>(writer));

    _wake_read = static_cast<intptr_t>(reader);
    _wake_write = static_cast<intptr_t>(writer);
    return true;
#else
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return false;

    SetNonBlocking(fds[0]);
    SetNonBlocking(fds[1]);

    _wake_read = fds[0];
    _wake_write = fds[1];
    return true;
#endif
}

int CUnixSocksTransport::ListenSocks(const std::string& path, int backlog)
{
    reset();

    // Windows / Linux 都一样：上次残留的 socket 文件会让 bind 失败。
    RemoveUnixSocketFile(path);

    const int listener = ListenUnix(path, backlog);
    if (listener < 0)
        return listener;

    std::lock_guard<std::mutex> lock(_handle_mutex);
    _listener = listener;
    _path = path;
    _owns_path = true;
    return 0;
}

int CUnixSocksTransport::AcceptSocks()
{
    intptr_t listener = -1;
    {
        std::lock_guard<std::mutex> lock(_handle_mutex);
        listener = _listener;
    }

    if (listener < 0)
        return -EINVAL;

    const int client = AcceptFd(listener);
    if (client < 0)
    {
        std::lock_guard<std::mutex> lock(_handle_mutex);
        close_listener();
        remove_socket_path();
        return client;
    }

    {
        std::lock_guard<std::mutex> lock(_handle_mutex);
        close_listener();
        remove_socket_path();
        _fd = client;
    }

    if (!SetNonBlocking(static_cast<intptr_t>(client)))
    {
        close();
        return -EINVAL;
    }

    make_wake_pair();
    return 0;
}

int CUnixSocksTransport::CreateSocks(const std::string& path, int backlog)
{
    const int rc = ListenSocks(path, backlog);
    if (rc < 0)
        return rc;

    const int accepted = AcceptSocks();
    if (accepted < 0)
        close();
    return accepted;
}

int CUnixSocksTransport::ConnectSocks(const std::string& path)
{
    reset();

    const int fd = ConnectUnix(path);
    if (fd < 0)
        return fd;

    {
        std::lock_guard<std::mutex> lock(_handle_mutex);
        _fd = fd;
    }

    if (!SetNonBlocking(static_cast<intptr_t>(fd)))
    {
        close();
        return -EINVAL;
    }

    make_wake_pair();
    return 0;
}

int CUnixSocksTransport::AdoptSocks(int fd, bool take_ownership)
{
    reset();

    if (fd < 0)
        return -EINVAL;

    intptr_t handle = fd;
    if (!take_ownership)
    {
#ifdef _WIN32
        handle = DuplicateSocket(fd);
#else
        handle = DuplicateFd(fd);
#endif
        if (handle < 0)
            return -EINVAL;
    }

    if (!SetNonBlocking(handle))
    {
        CloseRawFd(handle);
        return -EINVAL;
    }

    {
        std::lock_guard<std::mutex> lock(_handle_mutex);
        _fd = handle;
    }

    make_wake_pair();
    return 0;
}

int CUnixSocksTransport::read(void* buf, size_t len)
{
    if (_fd < 0 || !buf || len == 0)
        return -EBADF;

#ifdef _WIN32
    for (;;)
    {
        const int n = recv(static_cast<SOCKET>(_fd), static_cast<char*>(buf),
                           static_cast<int>(len), 0);
        if (n >= 0)
            return n;

        const int error = WSAGetLastError();
        if (error == WSAEINTR)
            continue;
        if (error == WSAEWOULDBLOCK)
            return -EAGAIN;
        return -error;
    }
#else
    for (;;)
    {
        const ssize_t n = ::read(static_cast<int>(_fd), buf, len);
        if (n >= 0)
            return static_cast<int>(n);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -EAGAIN;
        return -errno;
    }
#endif
}

int CUnixSocksTransport::write(const void* buf, size_t len)
{
    if (_fd < 0 || (!buf && len != 0))
        return -EBADF;
    if (len == 0)
        return 0;

#ifdef _WIN32
    for (;;)
    {
        const int n = send(static_cast<SOCKET>(_fd), static_cast<const char*>(buf),
                           static_cast<int>(len), 0);
        if (n >= 0)
            return n;

        const int error = WSAGetLastError();
        if (error == WSAEINTR)
            continue;
        if (error == WSAEWOULDBLOCK)
            return -EAGAIN;
        return -error;
    }
#else
    for (;;)
    {
        ssize_t n = ::send(static_cast<int>(_fd), buf, len,
#ifdef MSG_NOSIGNAL
                           MSG_NOSIGNAL
#else
                           0
#endif
        );

        if (n < 0 && (errno == ENOTSOCK || errno == EOPNOTSUPP))
            n = ::write(static_cast<int>(_fd), buf, len);

        if (n >= 0)
            return static_cast<int>(n);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -EAGAIN;
        return -errno;
    }
#endif
}

int CUnixSocksTransport::wait(bool want_write, int timeout_ms)
{
    if (_fd < 0)
        return -EBADF;

#ifdef _WIN32
    WSAPOLLFD fds[2]{};

    fds[0].fd = static_cast<SOCKET>(_fd);
    fds[0].events = POLLRDNORM | (want_write ? POLLWRNORM : 0);

    int count = 1;
    if (_wake_read >= 0)
    {
        fds[1].fd = static_cast<SOCKET>(_wake_read);
        fds[1].events = POLLRDNORM;
        count = 2;
    }

    const int rc = WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
    if (rc == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        if (error == WSAEINTR)
            return 0;
        return -error;
    }
    if (rc == 0)
        return 0;

    if (count == 2 && (fds[1].revents & (POLLRDNORM | POLLHUP | POLLERR)))
        drain_wake();

    int mask = 0;
    if (fds[0].revents & (POLLRDNORM | POLLHUP | POLLERR))
        mask |= 1;
    if (fds[0].revents & (POLLWRNORM | POLLERR | POLLHUP))
        mask |= 2;
    if (mask == 0)
        mask = 1; // 只有 wakeup / 重新检查队列
    return mask;
#else
    pollfd fds[2]{};

    fds[0].fd = static_cast<int>(_fd);
    fds[0].events = POLLIN | (want_write ? POLLOUT : 0);

    int count = 1;
    if (_wake_read >= 0)
    {
        fds[1].fd = static_cast<int>(_wake_read);
        fds[1].events = POLLIN;
        count = 2;
    }

    const int rc = poll(fds, static_cast<nfds_t>(count), timeout_ms);
    if (rc < 0)
    {
        if (errno == EINTR)
            return 0;
        return -errno;
    }
    if (rc == 0)
        return 0;

    if (count == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
        drain_wake();

    int mask = 0;
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
        mask |= 1;
    if (fds[0].revents & POLLOUT)
        mask |= 2;
    if (mask == 0)
        mask = 1;
    return mask;
#endif
}

void CUnixSocksTransport::drain_wake()
{
    if (_wake_read < 0)
        return;

    char buffer[64];
#ifdef _WIN32
    while (recv(static_cast<SOCKET>(_wake_read), buffer, sizeof(buffer), 0) > 0)
    {
    }
#else
    while (::read(static_cast<int>(_wake_read), buffer, sizeof(buffer)) > 0)
    {
    }
#endif
}

void CUnixSocksTransport::wake()
{
    std::lock_guard<std::mutex> lock(_handle_mutex);
    if (_closed || _wake_write < 0)
        return;

#ifdef _WIN32
    WriteWakeByte(_wake_write);
#else
    const uint8_t byte = 1;
    const ssize_t ignored = ::write(static_cast<int>(_wake_write), &byte, 1);
    (void)ignored;
#endif
}

void CUnixSocksTransport::shutdown()
{
    _shutdown.store(true);
    wake();
}

void CUnixSocksTransport::close()
{
    std::lock_guard<std::mutex> lock(_handle_mutex);
    if (_closed)
        return;
    _closed = true;
    close_handles();
}

} // namespace MessageManager
